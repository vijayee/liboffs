//
// Created by victor on 6/26/26.
//
// In-process WSS (WebSocket-over-TLS) regression test (cross-platform). Mirrors
// test_ws_transport.cpp's HTTP-upgrade + WS-frame round-trip but over TLS, so
// it drives the server-side SSL read+write paths that the plain WS test never
// touches — in particular the upgrade-over-TLS path, where the HTTP upgrade
// request arrives as ciphertext and must be decrypted before the WS state
// machine can parse it.
//
// On Windows (IOCP) the server-side TLS read path used to deadlock: poll-dancer
// drained the kernel socket into a watcher buffer before the worker ran
// SSL_read, so SSL_read saw EAGAIN / SSL_ERROR_WANT_READ and the handshake
// never advanced — no WSS upgrade could complete. The Windows memory-BIO fix
// feeds the IOCP-drained ciphertext into an SSL memory BIO on the worker, where
// SSL_read decrypts it; the decrypted plaintext is handed straight to
// _ws_connection_feed_plaintext (the same upgrade + WS-frame parser the plain
// path uses). This test drives that path with a real OpenSSL client (TLS
// handshake + HTTP upgrade + small PUT round-trip). On POSIX it guards the
// existing SSL_set_fd server path against regression.
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
#include "../src/ClientAPI/WS/ws_transport.h"
#include "../src/ClientAPI/WS/ws_frame.h"
#include "../src/ClientAPI/client_api_wire.h"
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

#include <atomic>

namespace {

/* Pre-staged PEMs (copied into the test working dir by test/CMakeLists.txt). */
static const char* kCertPath = "certs/leaf_cert.pem";
static const char* kKeyPath = "certs/leaf_key.pem";

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

/* Send a fully-buffered byte slice over TLS (blocking, bounded retry). */
static int _tls_send_all(SSL* ssl, const uint8_t* buf, size_t len) {
  size_t sent_total = 0;
  while (sent_total < len) {
    int w = SSL_write(ssl, buf + sent_total, (int)(len - sent_total));
    if (w <= 0) return -1;
    sent_total += (size_t)w;
  }
  return 0;
}

/* Perform the WebSocket HTTP upgrade handshake over TLS. */
static int _ws_upgrade_ssl(SSL* ssl) {
  const char* upgrade_request =
      "GET /offs HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  if (_tls_send_all(ssl, (const uint8_t*)upgrade_request, strlen(upgrade_request)) != 0) {
    return -1;
  }

  char response[4096];
  memset(response, 0, sizeof(response));
  size_t total = 0;
  for (int attempts = 0; attempts < 500; attempts++) {
    if (total + 1 >= sizeof(response)) break;
    int r = SSL_read(ssl, response + total, (int)(sizeof(response) - total - 1));
    if (r > 0) {
      total += (size_t)r;
      response[total] = '\0';
      if (strstr(response, "101") != NULL) return 0;
    } else if (r == 0) {
      return -1;  /* peer closed */
    } else {
      int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        platform_sleep_ms(10);
        continue;
      }
      return -1;  /* timeout or hard error */
    }
  }
  return -1;
}

/* Build a masked binary WebSocket frame (client-to-server, opcode 0x02). */
static uint8_t* _ws_build_frame(const uint8_t* payload, size_t payload_len, size_t* out_len) {
  size_t header_len;
  if (payload_len < 126) header_len = 6;
  else if (payload_len < 65536) header_len = 8;
  else header_len = 14;
  uint8_t* frame = (uint8_t*)malloc(header_len + payload_len);
  if (frame == NULL) return NULL;
  size_t pos = 0;
  frame[pos++] = 0x82; /* FIN + binary opcode */
  if (payload_len < 126) {
    frame[pos++] = 0x80 | (uint8_t)payload_len;
  } else if (payload_len < 65536) {
    frame[pos++] = 0x80 | 126;
    frame[pos++] = (payload_len >> 8) & 0xFF;
    frame[pos++] = payload_len & 0xFF;
  } else {
    frame[pos++] = 0x80 | 127;
    for (int i = 56; i >= 0; i -= 8) frame[pos++] = (payload_len >> i) & 0xFF;
  }
  /* Masking key: all zeros for test simplicity */
  frame[pos++] = 0x00; frame[pos++] = 0x00; frame[pos++] = 0x00; frame[pos++] = 0x00;
  memcpy(frame + pos, payload, payload_len);
  *out_len = header_len + payload_len;
  return frame;
}

/* Send a CBOR message as a masked binary WebSocket frame over TLS. */
static int _ws_send_cbor_ssl(SSL* ssl, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);
  if (cbor_buf == NULL) return -1;

  size_t ws_len;
  uint8_t* ws_frame = _ws_build_frame(cbor_buf, cbor_len, &ws_len);
  free(cbor_buf);
  if (ws_frame == NULL) return -1;
  int rc = _tls_send_all(ssl, ws_frame, ws_len);
  free(ws_frame);
  return rc;
}

/* Receive one WebSocket frame from the server over TLS and return its decoded
 * CBOR payload, or nullptr on close/timeout. Server frames are unmasked. */
static cbor_item_t* _ws_recv_cbor_ssl(SSL* ssl, int timeout_ms = 5000) {
  uint8_t buf[65536];
  size_t buf_len = 0;
  for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
    if (buf_len >= 2) {
      uint8_t opcode = buf[0] & 0x0F;
      if (opcode == 0x08) return nullptr; /* close frame */
      if (opcode == 0x02) {
        uint8_t len_byte = buf[1];
        size_t payload_offset = 0;
        size_t payload_len = 0;
        int have_header = 0;
        if (len_byte < 126) {
          payload_len = len_byte; payload_offset = 2; have_header = 1;
        } else if (len_byte == 126) {
          if (buf_len >= 4) {
            payload_len = ((size_t)buf[2] << 8) | buf[3];
            payload_offset = 4; have_header = 1;
          }
        } else {
          if (buf_len >= 10) {
            payload_len = 0;
            for (int i = 4; i < 10; i++) payload_len = (payload_len << 8) | buf[i];
            payload_offset = 10; have_header = 1;
          }
        }
        if (have_header && buf_len >= payload_offset + payload_len) {
          struct cbor_load_result load_result;
          cbor_item_t* item = cbor_load(buf + payload_offset, payload_len, &load_result);
          if (item != NULL && load_result.error.code == CBOR_ERR_NONE) return item;
          if (item != NULL) cbor_decref(&item);
          return nullptr;
        }
      }
    }
    if (buf_len >= sizeof(buf)) return nullptr;
    int r = SSL_read(ssl, buf + buf_len, (int)(sizeof(buf) - buf_len));
    if (r > 0) {
      buf_len += (size_t)r;
    } else if (r == 0) {
      return nullptr;
    } else {
      int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        platform_sleep_ms(10);
        continue;
      }
      return nullptr;
    }
  }
  return nullptr;
}

class TestWsTransportSsl : public testing::Test {
protected:
  scheduler_pool_t* pool;
  timer_actor_t* timer;
  block_cache_t* bc;
  ofd_cache_t* ofd_cache;
  tuple_cache_t* tc;
  ws_transport_t* transport;
  char* cache_dir;
  uint16_t port;

  void SetUp() override {
    static std::atomic<uint16_t> next_port{40080};
    port = next_port.fetch_add(1) + (uint16_t)((platform_getpid() % 113) * 4);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer = timer_actor_create(pool);

    char dir_template[] = "/tmp/test_ws_transport_ssl_XXXXXX";
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

    transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port,
                                    kCertPath, kKeyPath, 0, NULL, NULL);
    for (int retry = 0; transport == nullptr && retry < 10; retry++) {
      port = next_port.fetch_add(1) + (uint16_t)((platform_getpid() % 113) * 4);
      transport = ws_transport_create(pool, bc, ofd_cache, tc, "127.0.0.1", port,
                                      kCertPath, kKeyPath, 0, NULL, NULL);
    }
    ASSERT_NE(transport, nullptr);
    ws_transport_start(transport);
  }

  void TearDown() override {
    if (transport != nullptr) ws_transport_stop(transport);
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    ofd_cache_destroy(ofd_cache);
    tuple_cache_destroy(tc);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    if (transport != nullptr) ws_transport_destroy(transport);
    scheduler_pool_destroy(pool);
    rm_rf(cache_dir);
    free(cache_dir);
  }
};

/* TLS handshake + HTTP upgrade over TLS. This is the ws-specific deadlock
 * surface: the upgrade request arrives as ciphertext that must be decrypted
 * before the WS state machine can parse it. Before the memory-BIO fix the
 * handshake deadlocked on Windows and the upgrade never completed. */
TEST_F(TestWsTransportSsl, ConnectAndUpgrade) {
  tls_conn_t c = _tls_connect("127.0.0.1", port);
  ASSERT_NE(c.ssl, nullptr) << "TLS handshake failed (server SSL read deadlock?)";
  EXPECT_EQ(_ws_upgrade_ssl(c.ssl), 0);
  _tls_close(c);
}

/* Full PUT round-trip over WSS: upgrade, then a CBOR PUT request in a masked
 * binary WS frame, then the PUT_RESPONSE frame back. Exercises the server SSL
 * read path (decrypt the upgrade + the WS-framed PUT) and the write path (101
 * response + PUT_RESPONSE frame emitted via the write BIO). */
TEST_F(TestWsTransportSsl, PutSmallData) {
  tls_conn_t c = _tls_connect("127.0.0.1", port);
  ASSERT_NE(c.ssl, nullptr) << "TLS handshake failed (server SSL read deadlock?)";
  ASSERT_EQ(_ws_upgrade_ssl(c.ssl), 0);

  const char* content_type = "application/octet-stream";
  const char* file_name = "test_wss_file.bin";
  const uint8_t data[] = "hello wss world";

  client_api_put_request_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.content_type = (char*)content_type;
  msg.file_name = (char*)file_name;
  msg.stream_length = sizeof(data) - 1;
  msg.server_address = NULL;
  msg.data = (uint8_t*)data;
  msg.data_size = sizeof(data) - 1;

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(_ws_send_cbor_ssl(c.ssl, frame), 0);

  cbor_item_t* response = _ws_recv_cbor_ssl(c.ssl);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(client_api_wire_get_type(response), CLIENT_API_PUT_RESPONSE);

  client_api_put_response_t put_resp;
  memset(&put_resp, 0, sizeof(put_resp));
  EXPECT_EQ(client_api_put_response_decode(response, &put_resp), 0);
  if (put_resp.ori_string != nullptr) client_api_put_response_destroy(&put_resp);
  cbor_decref(&response);

  _tls_close(c);
}

} // namespace