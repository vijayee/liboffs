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
#include <thread>

#include "test_control_protocol.h"

extern "C" {
#include "../tools/offs-ca/ca_ops.h"
}

extern "C" int node_main(int argc, char* argv[]);

static std::string get_self_path() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    return std::string(path);
  }
  return "./test_file_transfer_integration";
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

static std::atomic<uint16_t> next_base_port{15000};

struct Process {
  pid_t pid = -1;
  uint16_t control_port = 0;
  int control_fd = -1;
  std::string cache_dir;
};

class FileTransferIntegrationTest : public ::testing::Test {
protected:
  std::vector<Process> relays;
  std::vector<Process> nodes;
  uint16_t base_port;
  std::string test_dir;
  std::string cert_path;
  std::string key_path;

  void SetUp() override {
    base_port = next_base_port.fetch_add(100);
    std::ostringstream dir_stream;
    dir_stream << "/tmp/test_fft_" << getpid() << "_" << base_port;
    test_dir = dir_stream.str();
    std::filesystem::create_directories(test_dir);
    generate_test_certs();
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

    for (auto& relay : relays) {
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
    }
    relays.clear();

    if (!test_dir.empty()) {
      std::filesystem::remove_all(test_dir);
    }
  }

  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    int rc = ca_generate("/CN=liboffs-test", 1, "rsa",
                         cert_path.c_str(), key_path.c_str());
    ASSERT_EQ(rc, 0) << "Failed to generate test certificates";
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
    Process proc;
    proc.pid = pid;
    proc.control_port = port;
    relays.push_back(proc);
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
            "test_file_transfer_integration",
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

    Process proc;
    proc.pid = pid;
    proc.control_port = control_port;
    proc.cache_dir = cache_dir;

    nodes.push_back(proc);

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
            "test_file_transfer_integration",
            "--mode=node",
            "--port", np.c_str(),
            "--control-port", cp.c_str(),
            "--cache-dir", cache_dir.c_str(),
            "--cert", cert_path.c_str(),
            "--key", key_path.c_str(),
            nullptr);
      _exit(1);
    }

    Process proc;
    proc.pid = pid;
    proc.control_port = control_port;
    proc.cache_dir = cache_dir;

    nodes.push_back(proc);

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

    char buf[4096];
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

protected:
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

  void force_relay_only() {
    // Wait for both nodes to connect to the relay server before forcing relay-only.
    // Without this, endpoint IDs will be 0 and relay messages will be undeliverable.
    bool relay_a = wait_for_relay(nodes[0].control_fd);
    bool relay_b = wait_for_relay(nodes[1].control_fd);
    if (!relay_a || !relay_b) {
      ADD_FAILURE() << "Relay not connected: node_a=" << relay_a
                   << " node_b=" << relay_b;
      return;
    }

    // Get each node's status to extract node_id and relay endpoint
    std::string status_a = send_command(nodes[0].control_fd, CTRL_STATUS);
    std::string status_b = send_command(nodes[1].control_fd, CTRL_STATUS);

    std::string node_id_a = parse_node_id_from_status(status_a);
    std::string node_id_b = parse_node_id_from_status(status_b);
    uint32_t endpoint_a = parse_endpoint_from_status(status_a);
    uint32_t endpoint_b = parse_endpoint_from_status(status_b);

    if (node_id_a.empty() || node_id_b.empty()) {
      ADD_FAILURE() << "Node IDs not set: node_id_a=" << node_id_a
                   << " node_id_b=" << node_id_b;
      return;
    }
    if (endpoint_a == 0 || endpoint_b == 0) {
      ADD_FAILURE() << "Relay endpoint IDs not set: endpoint_a=" << endpoint_a
                   << " endpoint_b=" << endpoint_b;
      return;
    }

    // Add each peer to the other node's connection manager with relay endpoint ID.
    // This avoids requiring a direct QUIC connection for relay-only testing.
    // Node A learns about node B (with B's relay endpoint), and vice versa.
    send_command(nodes[0].control_fd,
        std::string(CTRL_ADD_PEER) + " " + node_id_b + " " + std::to_string(endpoint_b));
    send_command(nodes[1].control_fd,
        std::string(CTRL_ADD_PEER) + " " + node_id_a + " " + std::to_string(endpoint_a));
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

TEST_F(FileTransferIntegrationTest, FixtureCompiles) {
  EXPECT_TRUE(true);
}

/* ── Category A: Direct Peer-to-Peer (No Relay) ─────────────────────── */

TEST_F(FileTransferIntegrationTest, DirectSmallFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port + 0;
    uint16_t ctrl_a = base_port + 10;
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_b = base_port + 11;

    start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_b);
    send_command(nodes[0].control_fd, peer_cmd);
    send_command(nodes[0].control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, DirectLargeFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port + 0;
    uint16_t ctrl_a = base_port + 10;
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_b = base_port + 11;

    start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_b);
    send_command(nodes[0].control_fd, peer_cmd);
    send_command(nodes[0].control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 640000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, DirectLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t port_a = base_port + 0;
    uint16_t ctrl_a = base_port + 10;
    uint16_t port_b = base_port + 1;
    uint16_t ctrl_b = base_port + 11;

    start_node_no_relay(port_a, ctrl_a, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    start_node_no_relay(port_b, ctrl_b, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    std::string peer_cmd = std::string(CTRL_PEER_ADD) + " 127.0.0.1:" + std::to_string(port_a);
    send_command(nodes[1].control_fd, peer_cmd);
    send_command(nodes[1].control_fd, std::string(CTRL_WAIT_FOR_PEER) + " 1");

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

/* ── Category B: Relay-Mediated ─────────────────────────────────────── */

TEST_F(FileTransferIntegrationTest, RelaySmallFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    // Use relay-only peer discovery (no direct QUIC connection needed).
    // ADD_PEER sets up the peer relationship with relay endpoint IDs
    // so conn_state_send uses the relay path for all messages.
    force_relay_only();

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, RelayLargeFileTransfer) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    // Use relay-only peer discovery (no direct QUIC connection needed).
    force_relay_only();

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 640000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, RelayLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    // Use relay-only peer discovery (no direct QUIC connection needed).
    force_relay_only();

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, NATDetectionOpen) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay1_port = base_port + 0;
    uint16_t relay2_port = base_port + 1;
    uint16_t port_a = base_port + 2;
    uint16_t ctrl_a = base_port + 12;

    start_relay(relay1_port);
    start_relay(relay2_port);

    start_node(port_a, ctrl_a, relay1_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    std::string connect_cmd = std::string(CTRL_CONNECT_RELAY) + " 127.0.0.1:" + std::to_string(relay2_port);
    send_command(nodes[0].control_fd, connect_cmd);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::string resp = send_command(nodes[0].control_fd, CTRL_STATUS);
    EXPECT_NE(resp.find("nat=open"), std::string::npos) << "NAT not detected as open: " << resp;
#endif
}

/* ── Category C: Multi-Node Distribution ────────────────────────────── */

TEST_F(FileTransferIntegrationTest, ThreeNodePropagation) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_c = base_port + 13;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(nodes[2].pid, 0);
    ASSERT_GE(nodes[2].control_fd, 0);

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[2].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, ThreeNodeLateJoin) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_c = base_port + 13;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::this_thread::sleep_for(std::chrono::seconds(3));

    start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(nodes[2].pid, 0);
    ASSERT_GE(nodes[2].control_fd, 0);

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";
    resp = send_command(nodes[2].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH: " << resp;

    std::istringstream data_stream(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex;
    size_t data_size;
    data_stream >> fetched_checksum_hex >> data_size;
    EXPECT_GT(data_size, 0u);
    EXPECT_EQ(fetched_checksum_hex, stored_checksum_hex) << "Data integrity check failed";
#endif
}

TEST_F(FileTransferIntegrationTest, ConcurrentDownloads) {
#ifndef HAS_MSQUIC
    GTEST_SKIP() << "msquic not available";
#else
    uint16_t relay_port = base_port + 0;
    uint16_t port_a = base_port + 1;
    uint16_t ctrl_a = base_port + 11;
    uint16_t port_b = base_port + 2;
    uint16_t ctrl_b = base_port + 12;
    uint16_t port_c = base_port + 3;
    uint16_t ctrl_c = base_port + 13;

    start_relay(relay_port);

    start_node(port_a, ctrl_a, relay_port, test_dir + "/cache_a");
    ASSERT_GT(nodes[0].pid, 0);
    ASSERT_GE(nodes[0].control_fd, 0);

    start_node(port_b, ctrl_b, relay_port, test_dir + "/cache_b");
    ASSERT_GT(nodes[1].pid, 0);
    ASSERT_GE(nodes[1].control_fd, 0);

    start_node(port_c, ctrl_c, relay_port, test_dir + "/cache_c");
    ASSERT_GT(nodes[2].pid, 0);
    ASSERT_GE(nodes[2].control_fd, 0);

    std::string resp = send_command(nodes[0].control_fd, std::string(CTRL_STORE_FILE) + " 100000 2 3");
    ASSERT_NE(resp.find(CTRL_RESP_HASH), std::string::npos) << "STORE: " << resp;

    std::istringstream hash_stream(resp.substr(strlen(CTRL_RESP_HASH) + 1));
    std::string desc_hash_hex, file_hash_hex, stored_checksum_hex;
    size_t final_byte;
    hash_stream >> desc_hash_hex >> file_hash_hex >> final_byte >> stored_checksum_hex;

    std::string fetch_cmd = std::string(CTRL_FETCH_FILE) + " " + desc_hash_hex + " " +
                            file_hash_hex + " " + std::to_string(final_byte) + " 2 3";

    resp = send_command(nodes[1].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH B: " << resp;

    std::istringstream data_stream_b(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex_b;
    size_t data_size_b;
    data_stream_b >> fetched_checksum_hex_b >> data_size_b;
    EXPECT_GT(data_size_b, 0u);
    EXPECT_EQ(fetched_checksum_hex_b, stored_checksum_hex) << "Node B data integrity check failed";

    resp = send_command(nodes[2].control_fd, fetch_cmd);
    ASSERT_NE(resp.find(CTRL_RESP_DATA), std::string::npos) << "FETCH C: " << resp;

    std::istringstream data_stream_c(resp.substr(strlen(CTRL_RESP_DATA) + 1));
    std::string fetched_checksum_hex_c;
    size_t data_size_c;
    data_stream_c >> fetched_checksum_hex_c >> data_size_c;
    EXPECT_GT(data_size_c, 0u);
    EXPECT_EQ(fetched_checksum_hex_c, stored_checksum_hex) << "Node C data integrity check failed";
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