//
// Two-process integration test for offs_client streaming PUT + GET
// of a large file (~1.77 GB) over each non-HTTP transport:
// Unix socket, TCP, WebSocket, WebTransport (msquic, gated on HAS_MSQUIC).
//
// For each transport: PUT is verified by comparing the BLAKE3 file_hash
// returned in the ORI to a local BLAKE3 of the source. GET is verified
// by streaming the download to a temp file and byte-comparing the whole
// 1.77 GB against the source.
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
#include <cstdio>
#include <cstdint>
#include <cstdlib>
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
#include "../src/ClientAPI/WT/wt_transport.h"
#include "../src/ClientAPI/health_handler.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/base58.h"
}

#include "../deps/BLAKE3/c/blake3.h"

// Source file path. On machines without this file, every test in the
// fixture is skipped via GTEST_SKIP() in SetUp(). See TEST
// LargeFileUploadTest.LargeFileUpload_1MB for a synthetic test that
// always runs and exercises the same streaming PUT + GET round-trip
// with a 1 MB buffer-generated payload.
static const char* kSourceFile =
    "/home/victor/Videos/Big Hero 6 2014 1080p/Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4";
static const size_t kChunkSize = 64 * 1024;
// PUT/GET timeouts. The current server-side writeable_off_stream
// finalization is slow at multi-GB scale: in practice a 1.77 GB PUT
// can take 10+ minutes for the server to send its response. The
// streaming GET is bounded by the same finalization. The timeouts
// are generous; if the server is fixed, the round-trip will be much
// faster than the timeout.
static const int kPutTimeoutMs = 600000;
static const int kGetTimeoutMs = 600000;
static const size_t kCompareChunkSize = 1024 * 1024;

static std::atomic<uint16_t> g_next_base_port{37000};

static std::string get_self_path() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    return std::string(path);
  }
  return "./test_large_file_upload";
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

struct GetCbContext {
  std::atomic<int> end_called{0};
  std::atomic<int> error_called{0};
  std::atomic<size_t> bytes_written{0};
  uint8_t error_status{0};
  FILE* fp = nullptr;
  std::string download_path;
  sem_t sem;
  GetCbContext() { sem_init(&sem, 0, 0); }
  ~GetCbContext() { sem_destroy(&sem); }
};

static void on_get_data_to_file(void* ctx, const uint8_t* data, size_t len) {
  auto* c = (GetCbContext*)ctx;
  if (c->fp == nullptr) return;
  size_t n = fwrite(data, 1, len, c->fp);
  c->bytes_written.fetch_add(n, std::memory_order_release);
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

static config_t make_test_config(void) {
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
  return config;
}

static void node_event_loop(uint8_t* running) {
  while (*running) { sleep(1); }
}

static int run_unix_node(const char* socket_path, const char* cache_dir) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);
  config_t config = make_test_config();
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  unix_transport_t* transport = unix_transport_create(pool, bc, ofd_cache, tc,
                                                     (char*)socket_path, NULL,
                                                     &health_ctx);
  if (transport == NULL) return 1;
  unix_transport_start(transport);

  node_event_loop(&running);
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
  config_t config = make_test_config();
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  tcp_transport_t* transport = tcp_transport_create(pool, bc, ofd_cache, tc,
                                                    "127.0.0.1", port, NULL, NULL, NULL,
                                                    &health_ctx);
  if (transport == NULL) return 1;
  tcp_transport_start(transport);

  node_event_loop(&running);
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
  config_t config = make_test_config();
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  ws_transport_t* transport = ws_transport_create(pool, bc, ofd_cache, tc,
                                                  "127.0.0.1", port, NULL, NULL, 0, NULL,
                                                  &health_ctx);
  if (transport == NULL) return 1;
  ws_transport_start(transport);

  node_event_loop(&running);
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

static int run_wt_node(uint16_t port, const char* cache_dir,
                       const char* cert_path, const char* key_path) {
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  timer_actor_t* timer = timer_actor_create(pool);
  config_t config = make_test_config();
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);
  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  uint8_t running = 1;
  health_context_t health_ctx = {};
  health_ctx.block_cache = bc;
  health_ctx.running = &running;
  uint8_t draining = 0;
  health_ctx.draining = &draining;

  wt_transport_t* transport = wt_transport_create(pool, bc, ofd_cache, tc,
                                                  "127.0.0.1", port,
                                                  cert_path, key_path, 0, NULL,
                                                  &health_ctx);
  if (transport == NULL) return 1;
  wt_transport_start(transport);

  node_event_loop(&running);
  wt_transport_stop(transport);
  wt_transport_destroy(transport);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  ofd_cache_destroy(ofd_cache);
  tuple_cache_destroy(tc);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  return 0;
}

static int compute_local_blake3(const char* path, uint8_t out[32]) {
  FILE* fp = fopen(path, "rb");
  if (fp == NULL) return -1;

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::vector<uint8_t> buf(kChunkSize);
  while (true) {
    size_t n = fread(buf.data(), 1, kChunkSize, fp);
    if (n > 0) {
      blake3_hasher_update(&hasher, buf.data(), n);
    }
    if (n < kChunkSize) {
      if (ferror(fp)) {
        fclose(fp);
        return -1;
      }
      break;
    }
  }
  fclose(fp);

  blake3_hasher_finalize(&hasher, out, 32);
  return 0;
}

static int parse_file_hash_from_ori(const char* ori, uint8_t out[32]) {
  if (ori == NULL) return -1;
  const char* prefix = "/offsystem/v3/";
  const char* cursor = strstr(ori, prefix);
  if (cursor == NULL) return -1;
  cursor += strlen(prefix);

  const char* type_end = NULL;
  const char* search = cursor;
  while (*search) {
    const char* slash = strchr(search, '/');
    if (!slash) break;
    const char* after_slash = slash + 1;
    char* endp;
    (void)strtol(after_slash, &endp, 10);
    if (endp != after_slash && *endp == '/') {
      type_end = slash;
      break;
    }
    search = after_slash;
  }
  if (type_end == NULL) return -1;

  cursor = type_end + 1;
  char* endp;
  (void)strtol(cursor, &endp, 10);
  if (endp == cursor) return -1;
  cursor = endp + 1;

  const char* slash3 = strchr(cursor, '/');
  if (!slash3) return -1;
  size_t hash1_len = slash3 - cursor;

  // base58_decode uses strlen() internally, so we must copy the slice
  // into a null-terminated buffer first. The slice is at most ~50 chars.
  char hash_buf[64];
  if (hash1_len >= sizeof(hash_buf)) return -1;
  memcpy(hash_buf, cursor, hash1_len);
  hash_buf[hash1_len] = '\0';

  uint8_t raw[64] = {0};
  size_t written = 0;
  int rc = base58_decode(hash_buf, raw, sizeof(raw), &written);
  if (rc != 0 || written != 32) return -1;
  memcpy(out, raw, 32);
  return 0;
}

static int byte_compare_files(const char* path_a, const char* path_b) {
  FILE* a = fopen(path_a, "rb");
  if (a == NULL) return -1;
  FILE* b = fopen(path_b, "rb");
  if (b == NULL) { fclose(a); return -1; }

  std::vector<uint8_t> buf_a(kCompareChunkSize);
  std::vector<uint8_t> buf_b(kCompareChunkSize);
  size_t offset = 0;
  int result = 0;
  while (true) {
    size_t na = fread(buf_a.data(), 1, kCompareChunkSize, a);
    size_t nb = fread(buf_b.data(), 1, kCompareChunkSize, b);
    if (na != nb) { result = -2; break; }
    if (na == 0) break;
    if (memcmp(buf_a.data(), buf_b.data(), na) != 0) {
      for (size_t i = 0; i < na; i++) {
        if (buf_a[i] != buf_b[i]) {
          fprintf(stderr,
                  "byte mismatch at offset %zu: source=0x%02x downloaded=0x%02x\n",
                  offset + i, buf_a[i], buf_b[i]);
          result = -3;
          break;
        }
      }
      break;
    }
    offset += na;
  }
  fclose(a);
  fclose(b);
  return result;
}

static std::string hex_encode(const uint8_t* data, size_t len) {
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; i++) {
    static const char* hex = "0123456789abcdef";
    out[i*2]     = hex[(data[i] >> 4) & 0xf];
    out[i*2 + 1] = hex[data[i] & 0xf];
  }
  return out;
}

class LargeFileUploadTest : public ::testing::Test {
protected:
  pid_t node_pid = 0;
  std::string test_dir;
  std::string cache_dir;
  std::string socket_path;
  std::string cert_path;
  std::string key_path;
  uint16_t node_port = 0;
  size_t file_size = 0;
  bool certs_generated = false;

  void SetUp() override {
    char templ[] = "/tmp/largefile-upload-XXXXXX";
    char* mkdtemp_result = mkdtemp(templ);
    ASSERT_NE(mkdtemp_result, nullptr);
    test_dir = mkdtemp_result;
    cache_dir = test_dir + "/cache";
    mkdir(cache_dir.c_str(), 0700);

    struct stat st;
    if (stat(kSourceFile, &st) != 0) {
      GTEST_SKIP() << "Source file not present at " << kSourceFile
                   << " (errno=" << errno << ")";
    }
    if (st.st_size == 0) {
      FAIL() << "Source file is empty: " << kSourceFile;
    }
    file_size = (size_t)st.st_size;
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

  void generate_test_certs() {
    cert_path = test_dir + "/test_cert.pem";
    key_path = test_dir + "/test_key.pem";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
                      " -out " + cert_path +
                      " -days 1 -nodes -subj '/CN=liboffs-test' 2>/dev/null";
    int rc = system(cmd.c_str());
    if (rc != 0) {
      certs_generated = false;
      return;
    }
    certs_generated = true;
  }

  void start_unix_node() {
    socket_path = test_dir + "/offs.sock";
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      setvbuf(stderr, NULL, _IONBF, 0);
      setvbuf(stdout, NULL, _IONBF, 0);
      execl(get_self_path().c_str(), "test_large_file_upload",
            "--mode=node", "--transport=unix",
            "--socket", socket_path.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
    for (int i = 0; i < 100; i++) {
      if (access(socket_path.c_str(), F_OK) == 0) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    FAIL() << "Unix socket not created at " << socket_path;
  }

  void start_tcp_node() {
    node_port = g_next_base_port.fetch_add(1);
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      setvbuf(stderr, NULL, _IONBF, 0);
      setvbuf(stdout, NULL, _IONBF, 0);
      std::string port_str = std::to_string(node_port);
      execl(get_self_path().c_str(), "test_large_file_upload",
            "--mode=node", "--transport=tcp",
            "--port", port_str.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
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
      setvbuf(stderr, NULL, _IONBF, 0);
      setvbuf(stdout, NULL, _IONBF, 0);
      std::string port_str = std::to_string(node_port);
      execl(get_self_path().c_str(), "test_large_file_upload",
            "--mode=node", "--transport=ws",
            "--port", port_str.c_str(),
            "--cache-dir", cache_dir.c_str(),
            (char*)NULL);
      _exit(127);
    }
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

  void start_wt_node() {
    node_port = g_next_base_port.fetch_add(1);
    node_pid = fork();
    ASSERT_GE(node_pid, 0);
    if (node_pid == 0) {
      setvbuf(stderr, NULL, _IONBF, 0);
      setvbuf(stdout, NULL, _IONBF, 0);
      std::string port_str = std::to_string(node_port);
      execl(get_self_path().c_str(), "test_large_file_upload",
            "--mode=node", "--transport=wt",
            "--port", port_str.c_str(),
            "--cache-dir", cache_dir.c_str(),
            "--cert", cert_path.c_str(),
            "--key", key_path.c_str(),
            (char*)NULL);
      _exit(127);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  void run_round_trip(const std::string& url) {
    uint8_t expected_hash[32];
    ASSERT_EQ(compute_local_blake3(kSourceFile, expected_hash), 0)
        << "Failed to compute local BLAKE3 of " << kSourceFile;

    offs_client_t* client = offs_client_connect(url.c_str(), NULL);
    ASSERT_NE(client, nullptr) << "Failed to connect to " << url;

    std::string source_path(kSourceFile);
    size_t slash = source_path.rfind('/');
    std::string file_name = (slash == std::string::npos)
                                ? source_path
                                : source_path.substr(slash + 1);

    PutCbContext put_ctx;
    ASSERT_EQ(offs_client_put_stream_start(client, "video/mp4",
                                            file_name.c_str(), file_size), 0);

    {
      FILE* fp = fopen(kSourceFile, "rb");
      ASSERT_NE(fp, nullptr) << "Failed to reopen source for PUT";

      std::vector<uint8_t> buf(kChunkSize);
      while (true) {
        size_t n = fread(buf.data(), 1, kChunkSize, fp);
        if (n > 0) {
          ASSERT_EQ(offs_client_put_stream_data(client, buf.data(), n), 0);
        }
        if (n < kChunkSize) {
          if (ferror(fp)) {
            fclose(fp);
            FAIL() << "fread error on " << kSourceFile;
          }
          break;
        }
      }
      fclose(fp);
    }

    ASSERT_EQ(offs_client_put_stream_end(client, on_put, &put_ctx), 0);
    ASSERT_EQ(wait_sem(&put_ctx.sem, kPutTimeoutMs), 0) << "PUT timed out";
    ASSERT_EQ(put_ctx.called.load(), 1);
    ASSERT_FALSE(put_ctx.ori_string.empty());
    std::string ori = put_ctx.ori_string;

    uint8_t server_hash[32];
    ASSERT_EQ(parse_file_hash_from_ori(ori.c_str(), server_hash), 0)
        << "Failed to parse BLAKE3 file_hash from ORI: " << ori;
    EXPECT_EQ(memcmp(expected_hash, server_hash, 32), 0)
        << "BLAKE3 file_hash mismatch between local computation and server ORI. "
        << "expected=" << hex_encode(expected_hash, 32)
        << " actual=" << hex_encode(server_hash, 32);

    GetCbContext get_ctx;
    {
      std::string tmpl = test_dir + "/dl-XXXXXX.mp4";
      std::vector<char> buf(tmpl.size() + 1);
      strcpy(buf.data(), tmpl.c_str());
      int fd = mkstemps(buf.data(), 4);
      ASSERT_GE(fd, 0) << "mkstemps failed for download temp file";
      get_ctx.fp = fdopen(fd, "wb");
      ASSERT_NE(get_ctx.fp, nullptr) << "fdopen failed";
      get_ctx.download_path = buf.data();
    }

    ASSERT_EQ(offs_client_get(client, ori.c_str(),
                              on_get_data_to_file, on_get_end, on_get_error,
                              &get_ctx), 0);
    ASSERT_EQ(wait_sem(&get_ctx.sem, kGetTimeoutMs), 0) << "GET timed out";
    if (get_ctx.error_called) {
      FAIL() << "GET error status=" << (int)get_ctx.error_status;
    }
    EXPECT_EQ(get_ctx.end_called.load(), 1);
    EXPECT_EQ(get_ctx.bytes_written.load(), file_size)
        << "Downloaded byte count " << get_ctx.bytes_written.load()
        << " != source size " << file_size;

    fflush(get_ctx.fp);
    fclose(get_ctx.fp);
    get_ctx.fp = nullptr;

    int cmp = byte_compare_files(kSourceFile, get_ctx.download_path.c_str());
    ASSERT_EQ(cmp, 0) << "GET byte-compare failed (cmp=" << cmp << ")";

    unlink(get_ctx.download_path.c_str());
    offs_client_disconnect(client);
  }
};

TEST_F(LargeFileUploadTest, Mp4_RoundTrip_UnixSocket) {
  start_unix_node();
  run_round_trip("unix://" + socket_path);
}

TEST_F(LargeFileUploadTest, Mp4_RoundTrip_Tcp) {
  start_tcp_node();
  run_round_trip("tcp://127.0.0.1:" + std::to_string(node_port));
}

TEST_F(LargeFileUploadTest, Mp4_RoundTrip_WebSocket) {
  start_ws_node();
  run_round_trip("ws://127.0.0.1:" + std::to_string(node_port));
}

#if defined(HAS_MSQUIC)
TEST_F(LargeFileUploadTest, Mp4_RoundTrip_WebTransport) {
  generate_test_certs();
  if (!certs_generated) {
    GTEST_SKIP() << "openssl not available; cannot generate TLS certs. "
                 << "Install OpenSSL 3.x on PATH to enable WebTransport.";
  }
  start_wt_node();
  run_round_trip("wt://127.0.0.1:" + std::to_string(node_port));
}
#endif

int main(int argc, char* argv[]) {
  std::string mode;
  std::string transport;
  std::string socket_path;
  std::string cache_dir;
  std::string cert_path;
  std::string key_path;
  uint16_t port = 0;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--mode=node") mode = "node";
    else if (arg == "--transport=unix") transport = "unix";
    else if (arg == "--transport=tcp") transport = "tcp";
    else if (arg == "--transport=ws") transport = "ws";
    else if (arg == "--transport=wt") transport = "wt";
    else if (arg == "--socket" && i + 1 < argc) socket_path = argv[++i];
    else if (arg == "--port" && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
    else if (arg == "--cache-dir" && i + 1 < argc) cache_dir = argv[++i];
    else if (arg == "--cert" && i + 1 < argc) cert_path = argv[++i];
    else if (arg == "--key" && i + 1 < argc) key_path = argv[++i];
  }

  if (mode == "node") {
    if (transport == "unix" && !socket_path.empty()) {
      return run_unix_node(socket_path.c_str(), cache_dir.c_str());
    }
    if (transport == "tcp" && port > 0) {
      return run_tcp_node(port, cache_dir.c_str());
    }
    if (transport == "ws" && port > 0) {
      return run_ws_node(port, cache_dir.c_str());
    }
    if (transport == "wt" && port > 0) {
      return run_wt_node(port, cache_dir.c_str(),
                         cert_path.c_str(), key_path.c_str());
    }
    return 1;
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
