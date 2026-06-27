//
// Created by victor on 6/26/26.
//
// In-process TLS-TCP regression test (cross-platform). Mirrors
// test_tcp_transport.cpp's OFFS CBOR-over-TCP wire-protocol round-trip but over
// TLS, so it drives the server-side SSL read+write paths that the plain test
// never touches.
//
// On Windows (IOCP) the server-side TLS read path used to deadlock: poll-dancer
// drained the kernel socket into a watcher buffer before the worker ran
// SSL_read, so SSL_read saw EAGAIN / SSL_ERROR_WANT_READ and the handshake
// never advanced — no TLS-TCP request could complete. The Windows memory-BIO
// fix feeds the IOCP-drained ciphertext into an SSL memory BIO on the worker
// thread, where SSL_read decrypts it. This test drives that path with a real
// OpenSSL client (handshake + small PUT/GET round-trip + large 1 MiB GET that
// spans many TLS records and exercises the Windows write-BIO drain). On POSIX
// it guards the existing SSL_set_fd server path against regression.
//
// A socket recv timeout makes a server deadlock fail the test (recv-timeout
// error) instead of hanging the process.

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
#endif

#include <gtest/gtest.h>

extern "C" {
#include "../src/ClientAPI/TCP/tcp_transport.h"
#include "../src/ClientAPI/client_api_wire.h"
#include "../src/Network/stream_framer.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/block.h"
#include "../src/OFFStreams/ofd_cache.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Util/rm_rf.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>
}

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <string>
#include <vector>
#include <atomic>

namespace {

/* Pre-staged PEMs (copied into the test working dir by test/CMakeLists.txt). */
static const char* kCertPath = "certs/leaf_cert.pem";
static const char* kKeyPath = "certs/leaf_key.pem";

/* 1 MiB — exceeds one loopback send, so the server's GET_DATA frame forces the
 * Windows write-BIO drain (SSL_write into wbio, drain to ciphertext, bounded-
 * retry socket send) across many TLS records. */
static const size_t kLargeSize = 1024 * 1024;

static int _set_recv_timeout(platform_socket_t* sock, int ms) {
  int fd = platform_socket_fd(sock);
#ifdef _WIN32
  DWORD t = (DWORD)ms;
  return setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, (int)sizeof(t));
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/* A blocking TLS client connection: the platform socket plus the OpenSSL
 * session layered over it. _tls_connect owns both; _tls_close frees both. */
struct tls_conn_t {
  SSL* ssl;
  SSL_CTX* ctx;
  platform_socket_t* sock;
};

static void _tls_close(tls_conn_t c);

/* Connect a blocking TLS client to host:port and complete the handshake. A 5s
 * recv timeout turns a server-side handshake deadlock into a test failure
 * instead of a hang. Returns a tls_conn_t (or an all-NULL struct on failure). */
static tls_conn_t _tls_connect(const char* host, uint16_t port) {
  tls_conn_t c = {NULL, NULL, NULL};
  c.sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (c.sock == NULL) return c;

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  if (platform_address_parse(&addr, host, port) != 0) {
    platform_socket_destroy(c.sock);
    c.sock = NULL;
    return c;
  }
  int connected = -1;
  for (int attempt = 0; attempt < 200; attempt++) {
    connected = platform_socket_connect(c.sock, &addr);
    if (connected == 0) break;
    platform_sleep_ms(20);
  }
  if (connected != 0) {
    platform_socket_destroy(c.sock);
    c.sock = NULL;
    return c;
  }
  _set_recv_timeout(c.sock, 5000);

  c.ctx = SSL_CTX_new(TLS_client_method());
  if (c.ctx == NULL) {
    platform_socket_destroy(c.sock);
    c.sock = NULL;
    return c;
  }
  SSL_CTX_set_verify(c.ctx, SSL_VERIFY_NONE, NULL);
  c.ssl = SSL_new(c.ctx);
  if (c.ssl == NULL) {
    SSL_CTX_free(c.ctx);
    c.ctx = NULL;
    platform_socket_destroy(c.sock);
    c.sock = NULL;
    return c;
  }
  SSL_set_fd(c.ssl, platform_socket_fd(c.sock));
  SSL_set_connect_state(c.ssl);
  if (SSL_connect(c.ssl) != 1) {
    _tls_close(c);
    return tls_conn_t{NULL, NULL, NULL};
  }
  return c;
}

static void _tls_close(tls_conn_t c) {
  if (c.ssl != NULL) SSL_free(c.ssl);
  if (c.ctx != NULL) SSL_CTX_free(c.ctx);
  if (c.sock != NULL) platform_socket_destroy(c.sock);
}

/* Send one CBOR frame over TLS, length-prefixed by the stream framer. */
static int _tls_send_frame(SSL* ssl, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);
  if (cbor_buf == NULL) return -1;

  size_t framed_len;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);
  if (framed == NULL) return -1;

  size_t sent_total = 0;
  while (sent_total < framed_len) {
    int w = SSL_write(ssl, framed + sent_total, (int)(framed_len - sent_total));
    if (w <= 0) {
      free(framed);
      return -1;
    }
    sent_total += (size_t)w;
  }
  free(framed);
  return 0;
}

/* Receive one CBOR frame over TLS, using a client-side framer to reassemble
 * length-prefixed frames. Returns the decoded frame or nullptr on timeout/close. */
static cbor_item_t* _tls_recv_frame(SSL* ssl, stream_framer_t* framer, int timeout_ms = 10000) {
  uint8_t buf[65536];
  for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
    size_t frame_len;
    uint8_t* frame_data = stream_framer_next(framer, &frame_len);
    if (frame_data != NULL) {
      struct cbor_load_result load_result;
      cbor_item_t* item = cbor_load(frame_data, frame_len, &load_result);
      free(frame_data);
      if (item != NULL && load_result.error.code == CBOR_ERR_NONE) return item;
      if (item != NULL) cbor_decref(&item);
    }
    int r = SSL_read(ssl, buf, (int)sizeof(buf));
    if (r > 0) {
      stream_framer_feed(framer, buf, (size_t)r);
      continue;
    }
    if (r == 0) return nullptr;  /* peer closed */
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      platform_sleep_ms(10);
      continue;
    }
    return nullptr;  /* timeout or hard error */
  }
  return nullptr;
}

class TestTcpTransportSsl : public testing::Test {
protected:
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  tcp_transport_t* transport;
  char* cache_dir;
  uint16_t port;

  void SetUp() override {
    static std::atomic<uint16_t> next_port{30080};
    port = next_port.fetch_add(1) + (uint16_t)((platform_getpid() % 113) * 4);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer = timer_actor_create(pool);

    char dir_template[] = "/tmp/test_tcp_transport_ssl_XXXXXX";
    cache_dir = mkdtemp(dir_template);
    cache_dir = strdup(cache_dir);

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
    bc = block_cache_create(config, cache_dir, standard, timer, pool, NULL, 0);
    ofd_cache = ofd_cache_create(pool, bc, 300000);
    tc = tuple_cache_create(100, pool);

    transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port,
                                     kCertPath, kKeyPath, NULL, NULL);
    for (int retry = 0; transport == nullptr && retry < 10; retry++) {
      port = next_port.fetch_add(1) + (uint16_t)((platform_getpid() % 113) * 4);
      transport = tcp_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port,
                                       kCertPath, kKeyPath, NULL, NULL);
    }
    ASSERT_NE(transport, nullptr);
    tcp_transport_start(transport);
  }

  void TearDown() override {
    if (transport != nullptr) tcp_transport_stop(transport);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    ofd_cache_destroy(ofd_cache);
    tuple_cache_destroy(tc);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    if (transport != nullptr) tcp_transport_destroy(transport);
    scheduler_pool_destroy(pool);
    rm_rf(cache_dir);
    free(cache_dir);
  }
};

/* Small PUT then GET round-trip over TLS. Exercises the server SSL read path
 * (handshake + decrypt ClientHello/PUT request) and the write path (PUT_RESPONSE
 * + GET_DATA frames emitted via the write BIO). */
TEST_F(TestTcpTransportSsl, PutAndGetRoundTrip) {
  tls_conn_t c = _tls_connect("127.0.0.1", port);
  ASSERT_NE(c.ssl, nullptr) << "TLS handshake failed (server SSL read deadlock?)";

  const char* content_type = "application/octet-stream";
  const char* file_name = "roundtrip_tls.bin";
  const uint8_t data[] = "tls round trip test data";

  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)content_type;
  put_req.file_name = (char*)file_name;
  put_req.stream_length = sizeof(data) - 1;
  put_req.server_address = NULL;
  put_req.data = (uint8_t*)data;
  put_req.data_size = sizeof(data) - 1;

  cbor_item_t* frame = client_api_put_request_encode(&put_req);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(_tls_send_frame(c.ssl, frame), 0);

  stream_framer_t* framer = stream_framer_create();
  cbor_item_t* response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  ASSERT_EQ(client_api_wire_get_type(response), CLIENT_API_PUT_RESPONSE);

  client_api_put_response_t put_resp;
  memset(&put_resp, 0, sizeof(put_resp));
  ASSERT_EQ(client_api_put_response_decode(response, &put_resp), 0);
  ASSERT_NE(put_resp.ori_string, nullptr);
  char* ori_string = strdup(put_resp.ori_string);
  client_api_put_response_destroy(&put_resp);
  cbor_decref(&response);

  /* GET */
  client_api_get_request_t get_req;
  memset(&get_req, 0, sizeof(get_req));
  get_req.ori_string = ori_string;
  get_req.has_range = 0;
  frame = client_api_get_request_encode(&get_req);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(_tls_send_frame(c.ssl, frame), 0);
  free(ori_string);

  response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_GET_RESPONSE_START);
  cbor_decref(&response);

  response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  ASSERT_EQ(client_api_wire_get_type(response), CLIENT_API_GET_DATA);
  client_api_get_data_t get_data;
  memset(&get_data, 0, sizeof(get_data));
  ASSERT_EQ(client_api_get_data_decode(response, &get_data), 0);
  EXPECT_EQ(get_data.data_size, sizeof(data) - 1);
  EXPECT_EQ(memcmp(get_data.data, data, sizeof(data) - 1), 0);
  client_api_get_data_destroy(&get_data);
  cbor_decref(&response);

  response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_GET_END);
  cbor_decref(&response);

  stream_framer_destroy(framer);
  _tls_close(c);
}

/* Large (1 MiB) PUT then GET over TLS. The GET_DATA frame forces the server
 * write-BIO drain across many TLS records on Windows IOCP (where PD_EVENT_WRITE
 * is never delivered), and the client reads span many TLS records on the read
 * path. Before the memory-BIO fix this hung at the first read on Windows. */
TEST_F(TestTcpTransportSsl, PutAndGetLargeData) {
  tls_conn_t c = _tls_connect("127.0.0.1", port);
  ASSERT_NE(c.ssl, nullptr) << "TLS handshake failed (server SSL read deadlock?)";

  const char* content_type = "application/octet-stream";
  const char* file_name = "large_tls.bin";
  uint8_t* data = (uint8_t*)malloc(kLargeSize);
  ASSERT_NE(data, nullptr);
  for (size_t i = 0; i < kLargeSize; i++) data[i] = (uint8_t)(i * 7 + 3);

  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)content_type;
  put_req.file_name = (char*)file_name;
  put_req.stream_length = kLargeSize;
  put_req.server_address = NULL;
  put_req.data = data;
  put_req.data_size = kLargeSize;

  cbor_item_t* frame = client_api_put_request_encode(&put_req);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(_tls_send_frame(c.ssl, frame), 0);

  stream_framer_t* framer = stream_framer_create();
  cbor_item_t* response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  ASSERT_EQ(client_api_wire_get_type(response), CLIENT_API_PUT_RESPONSE);

  client_api_put_response_t put_resp;
  memset(&put_resp, 0, sizeof(put_resp));
  ASSERT_EQ(client_api_put_response_decode(response, &put_resp), 0);
  ASSERT_NE(put_resp.ori_string, nullptr);
  char* ori_string = strdup(put_resp.ori_string);
  client_api_put_response_destroy(&put_resp);
  cbor_decref(&response);

  client_api_get_request_t get_req;
  memset(&get_req, 0, sizeof(get_req));
  get_req.ori_string = ori_string;
  get_req.has_range = 0;
  frame = client_api_get_request_encode(&get_req);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(_tls_send_frame(c.ssl, frame), 0);
  free(ori_string);

  response = _tls_recv_frame(c.ssl, framer);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_GET_RESPONSE_START);
  cbor_decref(&response);

  /* Reassemble the full GET_DATA payload across TLS records. */
  std::vector<uint8_t> assembled;
  for (;;) {
    response = _tls_recv_frame(c.ssl, framer, 30000);
    ASSERT_NE(response, nullptr);
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_GET_DATA) {
      client_api_get_data_t get_data;
      memset(&get_data, 0, sizeof(get_data));
      ASSERT_EQ(client_api_get_data_decode(response, &get_data), 0);
      assembled.insert(assembled.end(), get_data.data, get_data.data + get_data.data_size);
      client_api_get_data_destroy(&get_data);
    } else if (type == CLIENT_API_GET_END) {
      cbor_decref(&response);
      break;
    }
    cbor_decref(&response);
  }

  EXPECT_EQ(assembled.size(), kLargeSize);
  EXPECT_EQ(memcmp(assembled.data(), data, kLargeSize), 0);

  free(data);
  stream_framer_destroy(framer);
  _tls_close(c);
}

} // namespace