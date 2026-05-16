#include <gtest/gtest.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <filesystem>

#include "test_control_protocol.h"

extern "C" int node_main(int argc, char* argv[]);

static std::atomic<uint16_t> next_base_port{15000};

struct Process {
  pid_t pid = -1;
  uint16_t control_port = 0;
  int control_fd = -1;
  std::string cache_dir;
};

class FileTransferIntegrationTest : public ::testing::Test {
protected:
  Process relay_proc;
  std::vector<Process> nodes;
  uint16_t base_port;
  std::string test_dir;

  void SetUp() override {
    base_port = next_base_port.fetch_add(100);
    std::ostringstream dir_stream;
    dir_stream << "/tmp/test_fft_" << getpid() << "_" << base_port;
    test_dir = dir_stream.str();
    std::filesystem::create_directories(test_dir);
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

    if (relay_proc.pid > 0) {
      kill(relay_proc.pid, SIGTERM);
      int status = 0;
      for (int attempt = 0; attempt < 10; attempt++) {
        if (waitpid(relay_proc.pid, &status, WNOHANG) != 0) {
          relay_proc.pid = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (relay_proc.pid > 0) {
        kill(relay_proc.pid, SIGKILL);
        waitpid(relay_proc.pid, &status, 0);
        relay_proc.pid = -1;
      }
    }

    if (!test_dir.empty()) {
      std::filesystem::remove_all(test_dir);
    }
  }

  void start_relay(uint16_t port) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    if (pid == 0) {
      std::string port_str = std::to_string(port);
      execl("./relay_server", "relay_server", "--port", port_str.c_str(), nullptr);
      _exit(1);
    }
    relay_proc.pid = pid;
    relay_proc.control_port = port;
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
      execl("./test_file_transfer_integration",
            "test_file_transfer_integration",
            "--mode=node",
            "--port", np.c_str(),
            "--control-port", cp.c_str(),
            "--cache-dir", cache_dir.c_str(),
            "--relay-host", "127.0.0.1",
            "--relay-port", rp.c_str(),
            nullptr);
      _exit(1);
    }

    Process proc;
    proc.pid = pid;
    proc.control_port = control_port;
    proc.cache_dir = cache_dir;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_control_socket(control_port);
    ASSERT_GE(fd, 0) << "Failed to connect to node control socket on port " << control_port;
    proc.control_fd = fd;

    nodes.push_back(proc);
  }

  void start_node_no_relay(uint16_t node_port, uint16_t control_port,
                           const std::string& cache_dir) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    if (pid == 0) {
      std::string np = std::to_string(node_port);
      std::string cp = std::to_string(control_port);
      execl("./test_file_transfer_integration",
            "test_file_transfer_integration",
            "--mode=node",
            "--port", np.c_str(),
            "--control-port", cp.c_str(),
            "--cache-dir", cache_dir.c_str(),
            nullptr);
      _exit(1);
    }

    Process proc;
    proc.pid = pid;
    proc.control_port = control_port;
    proc.cache_dir = cache_dir;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_control_socket(control_port);
    ASSERT_GE(fd, 0) << "Failed to connect to node control socket on port " << control_port;
    proc.control_fd = fd;

    nodes.push_back(proc);
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