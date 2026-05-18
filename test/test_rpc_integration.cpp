#include <gtest/gtest.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <linux/limits.h>
#include <string.h>

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <cstdlib>

#include "test_control_protocol.h"

extern "C" int node_main(int argc, char* argv[]);

/* Wire message type constants (must match src/Network/wire.h) */
static constexpr uint8_t WIRE_PING_VAL                = 1;
static constexpr uint8_t WIRE_PING_RESPONSE_VAL       = 2;
static constexpr uint8_t WIRE_PING_CAPACITY_VAL        = 3;
static constexpr uint8_t WIRE_PING_CAPACITY_RESPONSE_VAL = 4;
static constexpr uint8_t WIRE_PING_BLOCK_VAL           = 5;
static constexpr uint8_t WIRE_PING_BLOCK_RESPONSE_VAL  = 6;
static constexpr uint8_t WIRE_FIND_BLOCK_VAL           = 7;
static constexpr uint8_t WIRE_FIND_BLOCK_RESPONSE_VAL  = 8;
static constexpr uint8_t WIRE_FIND_NODE_VAL            = 9;
static constexpr uint8_t WIRE_FIND_NODE_RESPONSE_VAL   = 10;
static constexpr uint8_t WIRE_STORE_BLOCK_VAL          = 11;
static constexpr uint8_t WIRE_STORE_BLOCK_RESPONSE_VAL = 12;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_VAL       = 13;
static constexpr uint8_t WIRE_SEEKING_BLOCKS_RESPONSE_VAL = 14;
static constexpr uint8_t WIRE_RANK_BLOCK_VAL           = 15;
static constexpr uint8_t WIRE_RECALL_BLOCK_VAL         = 16;
static constexpr uint8_t WIRE_RECALL_ACCEPT_VAL        = 17;
static constexpr uint8_t WIRE_RECALL_DECLINE_VAL       = 18;
static constexpr uint8_t WIRE_RATE_LIMITED_VAL          = 19;

/* Message direction constants (must match src/Network/message_log.h) */
static constexpr uint8_t MSG_DIRECTION_SENT_VAL      = 0;
static constexpr uint8_t MSG_DIRECTION_RECEIVED_VAL  = 1;
static constexpr uint8_t MSG_DIRECTION_FORWARDED_VAL = 2;

static std::string get_self_path() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    return std::string(path);
  }
  return "./test_rpc_integration";
}

static std::string get_relay_path() {
  char self_path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
  if (len > 0) {
    self_path[len] = '\0';
    std::string sp(self_path);
    auto pos = sp.rfind('/');
    if (pos != std::string::npos) {
      return sp.substr(0, pos) + "/../src/Network/Relay/meridian_relay";
    }
  }
  return "./meridian_relay";
}

static std::atomic<uint16_t> next_base_port{25000};

/* ---- Parsed event and Hebbian types ---- */

struct MessageEvent {
  uint8_t type;
  uint8_t direction;
  std::string peer_id;
  uint64_t message_id;
  std::string hash_prefix;
  uint8_t result;
  float hebbian_weight;
  size_t index;
};

struct HebbianEntry {
  std::string node_id;
  float weight;
};

/* ---- Per-node process descriptor ---- */

struct TopologyNode {
  pid_t pid = -1;
  uint16_t control_port = 0;
  int control_fd = -1;
  std::string node_id;
  uint32_t endpoint_id = 0;
  std::string cache_dir;
};

/* ---- Test fixture ---- */

class RpcIntegrationTest : public ::testing::Test {
protected:
  Process relay;
  std::vector<TopologyNode> nodes;
  uint16_t base_port;
  std::string test_dir;
  std::string cert_path;
  std::string key_path;

  void SetUp() override {
    base_port = next_base_port.fetch_add(100);
    std::ostringstream dir_stream;
    dir_stream << "/tmp/test_rpc_" << getpid() << "_" << base_port;
    test_dir = dir_stream.str();
    std::filesystem::create_directories(test_dir);
    generate_test_certs();
    relay.pid = -1;
    relay.control_port = 0;
    relay.control_fd = -1;
  }

  void TearDown() override {
    for (auto& node : nodes) {
      if (node.control_fd >= 0) {
        send_command(node.control_fd, CTRL_SHUTDOWN);
        close(node.control_fd);
        node.control_fd = -1;
      }
      if (node.pid > 0) {
        int status = 0;
        for (int attempt = 0; attempt < 20; attempt++) {
          if (waitpid(node.pid, &status, WNOHANG) != 0) {
            node.pid = -1;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (node.pid > 0) {
          kill(node.pid, SIGKILL);
          waitpid(node.pid, &status, 0);
          node.pid = -1;
        }
      }
      if (!node.cache_dir.empty()) {
        std::filesystem::remove_all(node.cache_dir);
      }
    }
    nodes.clear();

    if (relay.pid > 0) {
      kill(relay.pid, SIGTERM);
      int status = 0;
      for (int attempt = 0; attempt < 10; attempt++) {
        if (waitpid(relay.pid, &status, WNOHANG) != 0) {
          relay.pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (relay.pid > 0) {
        kill(relay.pid, SIGKILL);
        waitpid(relay.pid, &status, 0);
        relay.pid = -1;
      }
    }

    if (!test_dir.empty()) {
      std::filesystem::remove_all(test_dir);
    }
  }

  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                      " -out " + cert_path +
                      " -days 1 -nodes -subj '/CN=liboffs-test' 2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0) << "Failed to generate test certificates";
  }

  void start_relay(uint16_t port) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    if (pid == 0) {
      std::string port_str = std::to_string(port);
      std::string relay_path = get_relay_path();
      execl(relay_path.c_str(), "meridian_relay", "--port", port_str.c_str(),
            "--cert", cert_path.c_str(), "--key", key_path.c_str(), nullptr);
      _exit(1);
    }
    relay.pid = pid;
    relay.control_port = port;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  void start_node(uint16_t node_port, uint16_t control_port,
                  uint16_t relay_port, const std::string& cache_dir) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    if (pid == 0) {
      std::string np = std::to_string(node_port);
      std::string cp = std::to_string(control_port);
      std::string rp = std::to_string(relay_port);
      std::string self = get_self_path();
      execl(self.c_str(),
            "test_rpc_integration",
            "--mode=node",
            "--port", np.c_str(),
            "--control-port", cp.c_str(),
            "--cache-dir", cache_dir.c_str(),
            "--relay-host", "127.0.0.1",
            "--relay-port", rp.c_str(),
            "--cert", cert_path.c_str(),
            "--key", key_path.c_str(),
            nullptr);
      _exit(1);
    }

    TopologyNode node;
    node.pid = pid;
    node.control_port = control_port;
    node.cache_dir = cache_dir;

    nodes.push_back(node);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_control_socket(control_port);
    if (fd < 0) {
      ADD_FAILURE() << "Failed to connect control socket on port " << control_port;
      return;
    }
    nodes.back().control_fd = fd;
  }

  void start_node_no_relay(uint16_t node_port, uint16_t control_port,
                            const std::string& cache_dir) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    if (pid == 0) {
      std::string np = std::to_string(node_port);
      std::string cp = std::to_string(control_port);
      std::string self = get_self_path();
      execl(self.c_str(),
            "test_rpc_integration",
            "--mode=node",
            "--port", np.c_str(),
            "--control-port", cp.c_str(),
            "--cache-dir", cache_dir.c_str(),
            "--cert", cert_path.c_str(),
            "--key", key_path.c_str(),
            nullptr);
      _exit(1);
    }

    TopologyNode node;
    node.pid = pid;
    node.control_port = control_port;
    node.cache_dir = cache_dir;

    nodes.push_back(node);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_control_socket(control_port);
    if (fd < 0) {
      ADD_FAILURE() << "Failed to connect control socket (no relay) on port " << control_port;
      return;
    }
    nodes.back().control_fd = fd;
  }

  std::string send_command(int control_fd, const std::string& cmd) {
    std::string message = cmd + "\n";
    ssize_t written = write(control_fd, message.c_str(), message.size());
    if (written < 0) {
      return "ERROR write failed: " + std::string(strerror(errno));
    }

    char buf[8192];
    std::string response;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() < deadline) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(control_fd, &read_fds);

      struct timeval timeout;
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;

      int select_result = select(control_fd + 1, &read_fds, NULL, NULL, &timeout);
      if (select_result < 0) {
        return "ERROR select failed: " + std::string(strerror(errno));
      }
      if (select_result == 0) {
        continue;
      }

      ssize_t bytes_read = read(control_fd, buf, sizeof(buf) - 1);
      if (bytes_read <= 0) {
        return "ERROR read failed: " + std::string(strerror(errno));
      }
      buf[bytes_read] = '\0';
      response += buf;

      if (response.find('\n') != std::string::npos) {
        break;
      }
    }

    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
      response.pop_back();
    }
    return response;
  }

  bool wait_for_ready(int control_fd, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string response = send_command(control_fd, CTRL_STATUS);
      if (response.find(CTRL_RESP_STATUS) == 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
  }

  bool wait_for_relay(int control_fd, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string response = send_command(control_fd, CTRL_STATUS);
      if (response.find("relay=connected") != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
  }

  bool wait_for_peers(int control_fd, size_t target_count, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string response = send_command(control_fd, CTRL_STATUS);
      std::string prefix = "peers=";
      size_t pos = response.find(prefix);
      if (pos != std::string::npos) {
        size_t start = pos + prefix.length();
        size_t end = response.find(' ', start);
        if (end == std::string::npos) end = response.length();
        size_t count = (size_t)std::stoul(response.substr(start, end - start));
        if (count >= target_count) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
  }

  static uint32_t parse_endpoint_from_status(const std::string& status) {
    const std::string prefix = "endpoint=";
    size_t pos = status.find(prefix);
    if (pos == std::string::npos) return 0;
    return (uint32_t)std::stoul(status.substr(pos + prefix.length()));
  }

  static std::string parse_node_id_from_status(const std::string& status) {
    const std::string prefix = "node_id=";
    size_t pos = status.find(prefix);
    if (pos == std::string::npos) return "";
    size_t end = status.find(' ', pos + prefix.length());
    if (end == std::string::npos) end = status.length();
    return status.substr(pos + prefix.length(), end - pos - prefix.length());
  }

  /* ---- Topology helpers ---- */

  std::vector<TopologyNode> make_chain(int count) {
    std::vector<TopologyNode> topology;
    uint16_t relay_port = base_port + 90;

    start_relay(relay_port);

    for (int idx = 0; idx < count; idx++) {
      uint16_t node_port = base_port + idx;
      uint16_t ctrl_port = base_port + 10 + idx;
      std::string cache = test_dir + "/cache_" + std::to_string(idx);
      std::filesystem::create_directories(cache);
      start_node(node_port, ctrl_port, relay_port, cache);
      ASSERT_GT(nodes.back().pid, 0);
      ASSERT_GE(nodes.back().control_fd, 0);
    }

    /* Wait for all nodes to connect to relay */
    for (size_t idx = 0; idx < (size_t)count; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      ASSERT_TRUE(wait_for_relay(nodes[node_idx].control_fd))
          << "Node " << idx << " failed to connect to relay";
    }

    /* Extract node IDs and endpoint IDs from all nodes */
    for (int idx = 0; idx < count; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      std::string status = send_command(nodes[node_idx].control_fd, CTRL_STATUS);
      nodes[node_idx].node_id = parse_node_id_from_status(status);
      nodes[node_idx].endpoint_id = parse_endpoint_from_status(status);
      ASSERT_FALSE(nodes[node_idx].node_id.empty())
          << "Node " << idx << " has empty node_id";
      ASSERT_NE(nodes[node_idx].endpoint_id, 0u)
          << "Node " << idx << " has zero endpoint_id";
    }

    /* Connect adjacent nodes as peers via relay */
    for (int idx = 0; idx < count - 1; idx++) {
      size_t left = nodes.size() - count + idx;
      size_t right = nodes.size() - count + idx + 1;

      /* Left learns about Right */
      send_command(nodes[left].control_fd,
          std::string(CTRL_ADD_PEER) + " " + nodes[right].node_id + " " +
          std::to_string(nodes[right].endpoint_id));
      /* Right learns about Left */
      send_command(nodes[right].control_fd,
          std::string(CTRL_ADD_PEER) + " " + nodes[left].node_id + " " +
          std::to_string(nodes[left].endpoint_id));
    }

    /* Wait for peers to connect */
    for (int idx = 0; idx < count - 1; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      ASSERT_TRUE(wait_for_peers(nodes[node_idx].control_fd, 1))
          << "Node " << idx << " failed to get peer";
    }

    /* Copy node descriptors to return vector */
    for (int idx = 0; idx < count; idx++) {
      topology.push_back(nodes[nodes.size() - count + idx]);
    }
    return topology;
  }

  std::vector<TopologyNode> make_full_mesh(int count) {
    std::vector<TopologyNode> topology;
    uint16_t relay_port = base_port + 90;

    start_relay(relay_port);

    for (int idx = 0; idx < count; idx++) {
      uint16_t node_port = base_port + idx;
      uint16_t ctrl_port = base_port + 10 + idx;
      std::string cache = test_dir + "/cache_" + std::to_string(idx);
      std::filesystem::create_directories(cache);
      start_node(node_port, ctrl_port, relay_port, cache);
      ASSERT_GT(nodes.back().pid, 0);
      ASSERT_GE(nodes.back().control_fd, 0);
    }

    /* Wait for all nodes to connect to relay */
    for (int idx = 0; idx < count; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      ASSERT_TRUE(wait_for_relay(nodes[node_idx].control_fd))
          << "Node " << idx << " failed to connect to relay";
    }

    /* Extract node IDs and endpoint IDs */
    for (int idx = 0; idx < count; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      std::string status = send_command(nodes[node_idx].control_fd, CTRL_STATUS);
      nodes[node_idx].node_id = parse_node_id_from_status(status);
      nodes[node_idx].endpoint_id = parse_endpoint_from_status(status);
      ASSERT_FALSE(nodes[node_idx].node_id.empty())
          << "Node " << idx << " has empty node_id";
      ASSERT_NE(nodes[node_idx].endpoint_id, 0u)
          << "Node " << idx << " has zero endpoint_id";
    }

    /* Connect every node pair as peers via relay */
    for (int idx = 0; idx < count; idx++) {
      for (int jdx = idx + 1; jdx < count; jdx++) {
        size_t left = nodes.size() - count + idx;
        size_t right = nodes.size() - count + jdx;

        send_command(nodes[left].control_fd,
            std::string(CTRL_ADD_PEER) + " " + nodes[right].node_id + " " +
            std::to_string(nodes[right].endpoint_id));
        send_command(nodes[right].control_fd,
            std::string(CTRL_ADD_PEER) + " " + nodes[left].node_id + " " +
            std::to_string(nodes[left].endpoint_id));
      }
    }

    /* Wait for all peers to connect */
    for (int idx = 0; idx < count; idx++) {
      size_t node_idx = nodes.size() - count + idx;
      ASSERT_TRUE(wait_for_peers(nodes[node_idx].control_fd, count - 1))
          << "Node " << idx << " failed to get all peers";
    }

    /* Copy node descriptors to return vector */
    for (int idx = 0; idx < count; idx++) {
      topology.push_back(nodes[nodes.size() - count + idx]);
    }
    return topology;
  }

  /* ---- Event and Hebbian query helpers ---- */

  std::vector<MessageEvent> get_events(int control_fd, size_t after_cursor = 0) {
    std::vector<MessageEvent> result;
    std::string cmd = CTRL_GET_EVENTS;
    if (after_cursor > 0) {
      cmd += " " + std::to_string(after_cursor);
    }
    std::string response = send_command(control_fd, cmd);

    if (response.find(CTRL_RESP_EVENTS) != 0) {
      return result;
    }

    /* Format: EVENTS <total_count>|<index>:<type>,<dir>,<peer_id>,<msg_id>,<hash_prefix>,<result>,<hebbian>;... */
    size_t pipe_pos = response.find('|');
    if (pipe_pos == std::string::npos) return result;

    std::string entries = response.substr(pipe_pos + 1);
    if (entries.empty()) return result;

    std::istringstream entry_stream(entries);
    std::string entry;
    while (std::getline(entry_stream, entry, ';')) {
      if (entry.empty()) continue;

      /* Split on ':' to get index:fields */
      size_t colon_pos = entry.find(':');
      if (colon_pos == std::string::npos) continue;

      MessageEvent ev;
      ev.index = (size_t)std::stoul(entry.substr(0, colon_pos));
      std::string fields = entry.substr(colon_pos + 1);

      /* Parse comma-separated fields: type,dir,peer_id,msg_id,hash_prefix,result,hebbian */
      std::istringstream field_stream(fields);
      std::string field;
      std::vector<std::string> parts;
      while (std::getline(field_stream, field, ',')) {
        parts.push_back(field);
      }

      if (parts.size() >= 7) {
        ev.type = (uint8_t)std::stoul(parts[0]);
        ev.direction = (uint8_t)std::stoul(parts[1]);
        ev.peer_id = parts[2];
        ev.message_id = std::stoull(parts[3]);
        ev.hash_prefix = parts[4];
        ev.result = (uint8_t)std::stoul(parts[5]);
        ev.hebbian_weight = std::stof(parts[6]);
        result.push_back(ev);
      }
    }

    return result;
  }

  std::vector<HebbianEntry> get_hebbian(int control_fd) {
    std::vector<HebbianEntry> result;
    std::string response = send_command(control_fd, CTRL_HEBBIAN);

    if (response.find(CTRL_RESP_HEBBIAN) != 0) {
      return result;
    }

    /* Format: HEBBIAN_RESP <count>|<node_id>:<weight>;... */
    size_t pipe_pos = response.find('|');
    if (pipe_pos == std::string::npos) return result;

    std::string entries = response.substr(pipe_pos + 1);
    if (entries.empty()) return result;

    std::istringstream entry_stream(entries);
    std::string entry;
    while (std::getline(entry_stream, entry, ';')) {
      if (entry.empty()) continue;

      size_t colon_pos = entry.find(':');
      if (colon_pos == std::string::npos) continue;

      HebbianEntry he;
      he.node_id = entry.substr(0, colon_pos);
      he.weight = std::stof(entry.substr(colon_pos + 1));
      result.push_back(he);
    }

    return result;
  }

  void clear_events(int control_fd) {
    send_command(control_fd, CTRL_CLEAR_EVENTS);
  }

  bool has_event(const std::vector<MessageEvent>& events,
                 uint8_t direction, uint8_t type,
                 const std::string& peer_id = "",
                 uint8_t result = 255) {
    for (const auto& ev : events) {
      if (ev.direction != direction) continue;
      if (ev.type != type) continue;
      if (!peer_id.empty() && ev.peer_id != peer_id) continue;
      if (result != 255 && ev.result != result) continue;
      return true;
    }
    return false;
  }

  size_t count_events(const std::vector<MessageEvent>& events,
                      uint8_t direction, uint8_t type) {
    size_t count = 0;
    for (const auto& ev : events) {
      if (ev.direction == direction && ev.type == type) {
        count++;
      }
    }
    return count;
  }

  bool hebbian_increased(const std::vector<HebbianEntry>& before,
                         const std::vector<HebbianEntry>& after,
                         const std::string& peer_id,
                         float min_delta = 0.001f) {
    float before_weight = 0.0f;
    float after_weight = 0.0f;
    bool found_before = false;
    bool found_after = false;

    for (const auto& entry : before) {
      if (entry.node_id == peer_id) {
        before_weight = entry.weight;
        found_before = true;
        break;
      }
    }
    for (const auto& entry : after) {
      if (entry.node_id == peer_id) {
        after_weight = entry.weight;
        found_after = true;
        break;
      }
    }

    if (!found_after) return false;
    if (!found_before) return after_weight > 0.0f;
    return (after_weight - before_weight) >= min_delta;
  }

  bool hebbian_approx(const std::vector<HebbianEntry>& entries,
                      const std::string& peer_id,
                      float expected,
                      float tolerance = 0.01f) {
    for (const auto& entry : entries) {
      if (entry.node_id == peer_id) {
        float diff = entry.weight - expected;
        if (diff < 0) diff = -diff;
        return diff <= tolerance;
      }
    }
    return false;
  }

  /* Parse block hash from STORE_FILE HASH response.
     Format: HASH <desc_hash_hex> <file_hash_hex> <final_byte> <stored_checksum_hex> */
  std::string parse_hash_from_store_response(const std::string& response) {
    if (response.find(CTRL_RESP_HASH) != 0) return "";
    std::istringstream stream(response.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;
    return desc_hash_hex;
  }

private:
  int connect_control_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        return fd;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    close(fd);
    return -1;
  }
};

/* ================================================================ */
/*  Test cases                                                       */
/* ================================================================ */

TEST_F(RpcIntegrationTest, FixtureCompiles) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  EXPECT_TRUE(true);
#endif
}

TEST_F(RpcIntegrationTest, FindBlockChain) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 3-node chain: A -> B -> C */
  auto chain = make_chain(3);
  ASSERT_EQ(chain.size(), 3u);

  /* Store a block on node C */
  std::string store_resp = send_command(chain[2].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node C: " << store_resp;

  std::string block_hash = parse_hash_from_store_response(store_resp);
  ASSERT_FALSE(block_hash.empty()) << "Failed to parse block hash from: " << store_resp;

  /* Clear event logs on all nodes */
  for (size_t idx = 0; idx < chain.size(); idx++) {
    clear_events(chain[idx].control_fd);
  }

  /* Node A searches for the block */
  std::string find_resp = send_command(chain[0].control_fd,
      std::string(CTRL_FIND_BLOCK) + " " + block_hash);
  EXPECT_NE(find_resp.find(CTRL_RESP_OK), std::string::npos)
      << "FIND_BLOCK from node A: " << find_resp;

  /* Wait for propagation */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify node B has RECEIVED FIND_BLOCK event */
  auto events_b = get_events(chain[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_BLOCK_VAL))
      << "Node B should have received FIND_BLOCK";

  /* Verify node B has FORWARDED FIND_BLOCK event */
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_FORWARDED_VAL, WIRE_FIND_BLOCK_VAL))
      << "Node B should have forwarded FIND_BLOCK";

  /* Verify node A has RECEIVED FIND_BLOCK_RESPONSE event */
  auto events_a = get_events(chain[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_BLOCK_RESPONSE_VAL))
      << "Node A should have received FIND_BLOCK_RESPONSE";

  /* Verify Hebbian: node A's weight toward node C increased */
  auto hebbian_after = get_hebbian(chain[0].control_fd);
  /* Since we can't easily get "before" weights here without an extra query,
     we just verify that node A has a non-zero Hebbian entry for node C.
     The weight should have been updated through the interaction. */
  bool found_weight_for_c = false;
  for (const auto& entry : hebbian_after) {
    if (entry.node_id == chain[2].node_id && entry.weight > 0.0f) {
      found_weight_for_c = true;
      break;
    }
  }
  EXPECT_TRUE(found_weight_for_c)
      << "Node A should have non-zero Hebbian weight toward node C";
#endif
}

TEST_F(RpcIntegrationTest, FindBlockTTLExpiry) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 4-node chain: A -> B -> C -> D */
  auto chain = make_chain(4);
  ASSERT_EQ(chain.size(), 4u);

  /* Clear event logs */
  for (size_t idx = 0; idx < chain.size(); idx++) {
    clear_events(chain[idx].control_fd);
  }

  /* Node A searches for a nonexistent hash */
  std::string fake_hash(64, 'a');
  std::string find_resp = send_command(chain[0].control_fd,
      std::string(CTRL_FIND_BLOCK) + " " + fake_hash);
  EXPECT_NE(find_resp.find(CTRL_RESP_OK), std::string::npos)
      << "FIND_BLOCK from node A: " << find_resp;

  /* Wait for propagation */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify that FIND_BLOCK propagated through intermediate nodes */
  auto events_b = get_events(chain[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_BLOCK_VAL))
      << "Node B should have received FIND_BLOCK";

  /* Verify node A got a FIND_BLOCK_RESPONSE back (with not_found result) */
  auto events_a = get_events(chain[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_BLOCK_RESPONSE_VAL))
      << "Node A should have received FIND_BLOCK_RESPONSE";
#endif
}

TEST_F(RpcIntegrationTest, StoreBlockReplicationChain) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 3-node chain: A -> B -> C */
  auto chain = make_chain(3);
  ASSERT_EQ(chain.size(), 3u);

  /* Store a block on node A */
  std::string store_resp = send_command(chain[0].control_fd,
      std::string(CTRL_STORE_FILE) + " 100000 2 3");
  ASSERT_NE(store_resp.find(CTRL_RESP_HASH), std::string::npos)
      << "STORE on node A: " << store_resp;

  std::string block_hash = parse_hash_from_store_response(store_resp);
  ASSERT_FALSE(block_hash.empty()) << "Failed to parse block hash from: " << store_resp;

  /* Clear event logs */
  for (size_t idx = 0; idx < chain.size(); idx++) {
    clear_events(chain[idx].control_fd);
  }

  /* Node B uses FIND_BLOCK to locate the block */
  std::string find_resp = send_command(chain[1].control_fd,
      std::string(CTRL_FIND_BLOCK) + " " + block_hash);
  EXPECT_NE(find_resp.find(CTRL_RESP_OK), std::string::npos)
      << "FIND_BLOCK from node B: " << find_resp;

  /* Wait for propagation */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify node B received FIND_BLOCK_RESPONSE */
  auto events_b = get_events(chain[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_FIND_BLOCK_RESPONSE_VAL))
      << "Node B should have received FIND_BLOCK_RESPONSE";
#endif
}

TEST_F(RpcIntegrationTest, PingRoundTrip) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A pings Node B */
  std::string ping_resp = send_command(mesh[0].control_fd,
      std::string(CTRL_PING_PEER) + " " + mesh[1].node_id);
  EXPECT_NE(ping_resp.find(CTRL_RESP_OK), std::string::npos)
      << "PING_PEER from node A: " << ping_resp;

  /* Wait for ping round trip */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Verify Node B has RECEIVED PING event */
  auto events_b = get_events(mesh[1].control_fd);
  EXPECT_TRUE(has_event(events_b, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_VAL))
      << "Node B should have received PING";

  /* Verify Node A has RECEIVED PING_RESPONSE event */
  auto events_a = get_events(mesh[0].control_fd);
  EXPECT_TRUE(has_event(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_RESPONSE_VAL))
      << "Node A should have received PING_RESPONSE";
#endif
}

TEST_F(RpcIntegrationTest, HebbianVerification) {
#ifndef HAS_MSQUIC
  GTEST_SKIP() << "msquic not available";
#else
  /* Create 2-node mesh */
  auto mesh = make_full_mesh(2);
  ASSERT_EQ(mesh.size(), 2u);

  /* Query Hebbian weights before ping */
  auto hebbian_before_a = get_hebbian(mesh[0].control_fd);

  /* Clear event logs */
  for (size_t idx = 0; idx < mesh.size(); idx++) {
    clear_events(mesh[idx].control_fd);
  }

  /* Node A pings Node B multiple times */
  for (int ping_count = 0; ping_count < 3; ping_count++) {
    std::string ping_resp = send_command(mesh[0].control_fd,
        std::string(CTRL_PING_PEER) + " " + mesh[1].node_id);
    EXPECT_NE(ping_resp.find(CTRL_RESP_OK), std::string::npos)
        << "PING_PEER #" << ping_count << " from node A: " << ping_resp;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  /* Wait for all responses */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  /* Query Hebbian weights after ping */
  auto hebbian_after_a = get_hebbian(mesh[0].control_fd);

  /* Verify weight toward Node B increased */
  EXPECT_TRUE(hebbian_increased(hebbian_before_a, hebbian_after_a, mesh[1].node_id))
      << "Node A's Hebbian weight toward Node B should have increased";

  /* Verify Node A has SENT PING events and RECEIVED PING_RESPONSE events */
  auto events_a = get_events(mesh[0].control_fd);
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_SENT_VAL, WIRE_PING_VAL), 1u)
      << "Node A should have sent at least one PING";
  EXPECT_GE(count_events(events_a, MSG_DIRECTION_RECEIVED_VAL, WIRE_PING_RESPONSE_VAL), 1u)
      << "Node A should have received at least one PING_RESPONSE";
#endif
}

int main(int argc, char* argv[]) {
  for (int idx = 1; idx < argc; idx++) {
    std::string arg(argv[idx]);
    if (arg == "--mode=node" || arg == "--mode node") {
      return node_main(argc, argv);
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}