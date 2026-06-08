//
// Two-process integration test for offs_client.
// Verifies that a node process running a transport (Unix/TCP/WS/WT)
// can be driven by the offs_client library for PUT and GET operations.
//

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <linux/limits.h>
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <sstream>
#include <vector>
#include <semaphore.h>

extern "C" {
#include "../src/ClientLibs/c/offs_client.h"
#include "../src/ClientAPI/Unix/unix_transport.h"
#include "../src/ClientAPI/TCP/tcp_transport.h"
#include "../src/ClientAPI/WS/ws_transport.h"
#include "../src/ClientAPI/WS/ws_frame.h"
#include "../src/ClientAPI/health_handler.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
}

static std::atomic<uint16_t> g_next_base_port{37000};

static std::string get_self_path() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    return std::string(path);
  }
  return "./test_offs_client_integration";
}

struct GetCbContext {
  std::atomic<size_t> data_len{0};
  std::atomic<int> end_called{0};
  std::atomic<int> error_called{0};
  uint8_t error_status{0};
  std::vector<uint8_t> data;
  sem_t sem;
  GetCbContext() { sem_init(&sem, 0, 0); }
  ~GetCbContext() { sem_destroy(&sem); }
};

static void on_get_data(void* ctx, const uint8_t* data, size_t len) {
  auto* c = (GetCbContext*)ctx;
  c->data.insert(c->data.end(), data, data + len);
  c->data_len.store(c->data.size(), std::memory_order_release);
}

static void on_get_end(void* ctx) {
  auto* c = (GetCbContext*)ctx;
  c->end_called.store(1, std::memory_order_release);
  sem_post(&c->sem);
}

static void on_get_error(void* ctx, uint8_t status_code, const char* message) {
  auto* c = (GetCbContext*)ctx;
  c->error_status = status_code;
  c->error_called.store(1, std::memory_order_release);
  sem_post(&c->sem);
  (void)message;
}

struct PutCbContext {
  std::atomic<int> called{0};
  std::string ori_string;
  sem_t sem;
  PutCbContext() { sem_init(&sem, 0, 0); }
  ~PutCbContext() { sem_destroy(&sem); }
};

static void on_put(void* ctx, const char* ori_string) {
  auto* c = (PutCbContext*)ctx;
  if (ori_string != NULL) c->ori_string = ori_string;
  c->called.store(1, std::memory_order_release);
  sem_post(&c->sem);
}

static int wait_sem(sem_t* sem, int timeout_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  return sem_timedwait(sem, &ts);
}

static std::vector<uint8_t> make_data(size_t size) {
  std::vector<uint8_t> out(size);
  uint32_t state = 1;
  for (size_t i = 0; i < size; i++) {
    state = state * 1103515245u + 12345u;
    out[i] = (uint8_t)(state >> 16);
  }
  return out;
}

/* ---- Node main: runs a transport, blocking on epoll/select. ---- */

static int run_unix_node(const char* socket_path, const char* cache_dir) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  config_t config = {
      .index_bucket_size = 10,
      .index_wait = 1000,
      .index_max_wait = 5000,
      .section_size = 128000,
      .section_wait = 1000,
      .section_max_wait = 5000,
      .cache_size = 50,
      .max_tuple_size = 30,
      .lru_size = 50
  };
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  unix_transport_t* transport = unix_transport_create(pool, bc, ofd_cache, tc, (char*)socket_path, NULL, &health_ctx);
  if (transport == NULL) return 1;
  unix_transport_start(transport);

  while (running) {
    sleep(1);
  }
  unix_transport_stop(transport);
  unix_transport_destroy(transport);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  ofd_cache_destroy(ofd_cache);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  return 0;
}

static int run_tcp_node(uint16_t port, const char* cache_dir) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  config_t config = {
      .index_bucket_size = 10,
      .index_wait = 1000,
      .index_max_wait = 5000,
      .section_size = 128000,
      .section_wait = 1000,
      .section_max_wait = 5000,
      .cache_size = 50,
      .max_tuple_size = 30,
      .lru_size = 50
  };
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  tcp_transport_t* transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, NULL, &health_ctx);
  if (transport == NULL) return 1;
  tcp_transport_start(transport);

  while (running) {
    sleep(1);
  }
  tcp_transport_stop(transport);
  tcp_transport_destroy(transport);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  ofd_cache_destroy(ofd_cache);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  return 0;
}

static int run_ws_node(uint16_t port, const char* cache_dir) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);

  config_t config = {
      .index_bucket_size = 10,
      .index_wait = 1000,
      .index_max_wait = 5000,
      .section_size = 128000,
      .section_wait = 1000,
      .section_max_wait = 5000,
      .cache_size = 50,
      .max_tuple_size = 30,
      .lru_size = 50
  };
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  ws_transport_t* transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port, NULL, NULL, 0, NULL, &health_ctx);
  if (transport == NULL) return 1;
  ws_transport_start(transport);

  while (running) {
    sleep(1);
  }
  ws_transport_stop(transport);
  ws_transport_destroy(transport);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  ofd_cache_destroy(ofd_cache);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  return 0;
}

/* ---- Test fixture: forks a node and runs a PUT/GET round-trip. ---- */

class OffsClientIntegrationTest : public ::testing::Test {
protected:
  pid_t node_pid = 0;
  std::string test_dir;
  std::string cache_dir;
  std::string socket_path;
  uint16_t node_port = 0;

  void SetUp() override {
    char templ[] = "/tmp/offsclient-inttest-XXXXXX";
    char* mkdtemp_result = mkdtemp(templ);
    ASSERT_NE(mkdtemp_result, nullptr);
    test_dir = mkdtemp_result;
    cache_dir = test_dir + "/cache";
    mkdir(cache_dir.c_str(), 0700);
  }

  void TearDown() override {
    if (node_pid > 0) {
      kill(node_pid, SIGTERM);
      int status = 0;
      for (int i = 0; i < 20; i++) {
        if (waitpid(node_pid, &status, WNOHANG) != 0) {
          node_pid = 0;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (node_pid > 0) {
        kill(node_pid, SIGKILL);
        waitpid(node_pid, &status, 0);
        node_pid = 0;
      }
    }
    if (!test_dir.empty()) {
      rm_rf(test_dir.c_str());
    }
  }

  void start_unix_node() {
    socket_path = test_dir + "/offs.sock";
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      execl(get_self_path().c_str(), "test_offs_client_integration",
            "--mode=node", "--transport=unix",
            "--socket", socket_path.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
    // Wait for the socket to appear
    for (int i = 0; i < 100; i++) {
      if (access(socket_path.c_str(), F_OK) == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_EQ(access(socket_path.c_str(), F_OK), 0) << "Unix socket not created";
  }

  void start_tcp_node() {
    node_port = g_next_base_port.fetch_add(1);
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      std::string port_str = std::to_string(node_port);
      execl(get_self_path().c_str(), "test_offs_client_integration",
            "--mode=node", "--transport=tcp",
            "--port", port_str.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
    // Wait for the port to be listening
    for (int i = 0; i < 100; i++) {
      int sock = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(node_port);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
        close(sock);
        return;
      }
      close(sock);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    FAIL() << "TCP port not listening on " << node_port;
  }

  void start_ws_node() {
    node_port = g_next_base_port.fetch_add(1);
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      std::string port_str = std::to_string(node_port);
      execl(get_self_path().c_str(), "test_offs_client_integration",
            "--mode=node", "--transport=ws",
            "--port", port_str.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
    /* Poll for the WS port to be ready by attempting a TCP connect. */
    for (int i = 0; i < 100; i++) {
      int sock = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(node_port);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
        close(sock);
        return;
      }
      close(sock);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    FAIL() << "WS port not listening on " << node_port;
  }

  void run_round_trip(const std::string& url, size_t size) {
    auto data = make_data(size);

    offs_client_t* client = offs_client_connect(url.c_str(), NULL);
    ASSERT_NE(client, nullptr) << "Failed to connect to " << url;

    PutCbContext put_ctx;
    int ret = offs_client_put(client, "application/octet-stream", "test.bin",
                              size, data.data(), size, on_put, &put_ctx);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(wait_sem(&put_ctx.sem, 30000), 0) << "PUT timed out";
    ASSERT_EQ(put_ctx.called.load(), 1);
    ASSERT_FALSE(put_ctx.ori_string.empty());
    std::string ori = put_ctx.ori_string;

    GetCbContext get_ctx;
    ret = offs_client_get(client, ori.c_str(), on_get_data, on_get_end, on_get_error, &get_ctx);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(wait_sem(&get_ctx.sem, 30000), 0) << "GET timed out";
    if (get_ctx.error_called) {
      FAIL() << "GET error status=" << (int)get_ctx.error_status;
    }
    EXPECT_EQ(get_ctx.end_called.load(), 1);
    EXPECT_EQ(get_ctx.data_len.load(), size);
    EXPECT_EQ(get_ctx.data.size(), size);
    if (get_ctx.data.size() == size) {
      EXPECT_EQ(memcmp(get_ctx.data.data(), data.data(), size), 0);
    }

    offs_client_disconnect(client);
  }
};

TEST_F(OffsClientIntegrationTest, Unix_1MB) {
  start_unix_node();
  std::string url = "unix://" + socket_path;
  run_round_trip(url, 1024 * 1024);
}

TEST_F(OffsClientIntegrationTest, TCP_1MB) {
  start_tcp_node();
  std::string url = "tcp://127.0.0.1:" + std::to_string(node_port);
  run_round_trip(url, 1024 * 1024);
}

TEST_F(OffsClientIntegrationTest, WS_1MB) {
  start_ws_node();
  std::string url = "ws://127.0.0.1:" + std::to_string(node_port);
  run_round_trip(url, 1024 * 1024);
}

TEST_F(OffsClientIntegrationTest, Unix_512KB) {
  start_unix_node();
  std::string url = "unix://" + socket_path;
  run_round_trip(url, 512 * 1024);
}

TEST_F(OffsClientIntegrationTest, Unix_256KB) {
  start_unix_node();
  std::string url = "unix://" + socket_path;
  run_round_trip(url, 256 * 1024);
}

/* ---- Node main: dispatches based on --transport flag. ---- */

int main(int argc, char* argv[]) {
  std::string transport;
  std::string socket_path;
  std::string cache_dir;
  uint16_t port = 0;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--mode=node") continue;
    if (arg == "--transport=unix") transport = "unix";
    else if (arg == "--transport=tcp") transport = "tcp";
    else if (arg == "--transport=ws") transport = "ws";
    else if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
    else if (arg == "--port" && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
    else if (arg == "--cache-dir" && i + 1 < argc) cache_dir = argv[++i];
  }

  if (transport == "unix" && !socket_path.empty()) {
    return run_unix_node(socket_path.c_str(), cache_dir.c_str());
  }
  if (transport == "tcp" && port > 0) {
    return run_tcp_node(port, cache_dir.c_str());
  }
  if (transport == "ws" && port > 0) {
    return run_ws_node(port, cache_dir.c_str());
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
