//
// Created by victor on 6/26/26.
//
// In-process HTTPS regression test (cross-platform).
//
// On Windows (IOCP) the server-side TLS read path used to deadlock: poll-dancer
// drained the kernel socket into a watcher buffer before the worker ran SSL_read,
// so SSL_read saw EAGAIN / SSL_ERROR_WANT_READ and the handshake never advanced
// — no HTTPS request could complete. The Windows memory-BIO fix feeds the
// IOCP-drained ciphertext into an SSL memory BIO on the worker thread, where
// SSL_read decrypts it. This test drives that path with a real OpenSSL client
// (handshake + small GET + large GET + large POST echo). On POSIX it guards the
// existing SSL_set_fd server path against regression.
//
// A socket recv timeout makes a deadlock fail the test (recv-timeout error)
// instead of hanging the process.

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
#else
# include <sys/socket.h>
# include <sys/time.h>
# include <unistd.h>
#endif

#include <gtest/gtest.h>

extern "C" {
#include "../src/ClientAPI/HTTP/http_server.h"
#include "../src/ClientAPI/HTTP/http_request.h"
#include "../src/ClientAPI/HTTP/http_response.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>
}

#include <openssl/ssl.h>

#include <string>
#include <vector>
#include <atomic>
#include <sstream>

namespace {

/* Pre-staged PEMs (copied into the test working dir by test/CMakeLists.txt). */
static const char* kCertPath = "certs/leaf_cert.pem";
static const char* kKeyPath = "certs/leaf_key.pem";

/* ≥384 KiB — the historical large-response hang point. A response this size
 * spans many TLS records, exercising the Windows write path (SSL_write into the
 * memory BIO, drain wbio to ciphertext, bounded-retry socket send). */
static const size_t kLargeSize = 512 * 1024;

static std::vector<char>& large_body() {
  static std::vector<char> body;
  if (body.empty()) {
    body.resize(kLargeSize);
    for (size_t i = 0; i < kLargeSize; i++) {
      body[i] = (char)(i & 0xFF);
    }
  }
  return body;
}

static void _handle_small(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "text/plain");
  http_response_write(response, "Hello, TLS!", 11);
  http_response_end(response);
}

static void _handle_large(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/octet-stream");
  std::vector<char>& body = large_body();
  http_response_write(response, body.data(), body.size());
  http_response_end(response);
}

static void _handle_echo(http_request_t* request, http_response_t* response, void* user_data) {
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/octet-stream");
  if (request->body != NULL && request->body->size > 0) {
    http_response_write(response, (const char*)request->body->data, request->body->size);
  }
  http_response_end(response);
}

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

/* One HTTPS request over a fresh blocking TLS connection. Returns 0 on success
 * and fills out_body with the response body. Reads until the full
 * Content-Length body is received (not until EOF), so the test does not depend
 * on the server sending a TLS close_notify. A 5s recv timeout turns a server
 * deadlock into a test failure instead of a hang. */
static int _tls_request(uint16_t port, const char* method, const char* path,
                        const std::string& req_body, std::string& out_body,
                        std::string& err) {
  out_body.clear();
  err.clear();

  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (sock == NULL) {
    err = "platform_socket_create";
    return -1;
  }

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.addr = 0x0100007f; /* 127.0.0.1 in network byte order */
  addr.inet.port = port;

  int connected = -1;
  for (int attempt = 0; attempt < 200; attempt++) {
    connected = platform_socket_connect(sock, &addr);
    if (connected == 0) break;
    platform_sleep_ms(20);
  }
  if (connected != 0) {
    err = "platform_socket_connect";
    platform_socket_destroy(sock);
    return -1;
  }
  _set_recv_timeout(sock, 5000);

  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) {
    err = "SSL_CTX_new";
    platform_socket_destroy(sock);
    return -1;
  }
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

  SSL* ssl = SSL_new(ctx);
  if (ssl == NULL) {
    err = "SSL_new";
    SSL_CTX_free(ctx);
    platform_socket_destroy(sock);
    return -1;
  }
  SSL_set_fd(ssl, platform_socket_fd(sock));
  SSL_set_connect_state(ssl);

  int rc = SSL_connect(ssl);
  if (rc != 1) {
    int e = SSL_get_error(ssl, rc);
    std::ostringstream os;
    os << "SSL_connect rc=" << rc << " err=" << e;
    err = os.str();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    platform_socket_destroy(sock);
    return -1;
  }

  std::ostringstream rs;
  rs << method << " " << path << " HTTP/1.1\r\n"
     << "Host: 127.0.0.1\r\n"
     << "Content-Length: " << req_body.size() << "\r\n"
     << "Connection: close\r\n\r\n"
     << req_body;
  std::string request = rs.str();

  size_t sent = 0;
  while (sent < request.size()) {
    int w = SSL_write(ssl, request.data() + sent, (int)(request.size() - sent));
    if (w <= 0) {
      int e = SSL_get_error(ssl, w);
      std::ostringstream os;
      os << "SSL_write err=" << e;
      err = os.str();
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      platform_socket_destroy(sock);
      return -1;
    }
    sent += (size_t)w;
  }

  /* Read headers + body up to Content-Length. */
  std::string all;
  size_t content_length = (size_t)-1;
  size_t body_start = 0;
  for (;;) {
    if (content_length != (size_t)-1 && all.size() >= body_start + content_length) {
      break; /* full body received */
    }
    char buf[8192];
    int r = SSL_read(ssl, buf, (int)sizeof(buf));
    if (r > 0) {
      all.append(buf, (size_t)r);
      if (content_length == (size_t)-1) {
        size_t sep = all.find("\r\n\r\n");
        if (sep != std::string::npos) {
          body_start = sep + 4;
          size_t cl = all.find("Content-Length: ");
          if (cl != std::string::npos && cl < sep) {
            content_length = (size_t)strtoul(all.c_str() + cl + 16, NULL, 10);
          } else {
            content_length = 0; /* no body expected */
          }
        }
      }
      continue;
    }
    if (r == 0) {
      break; /* peer closed */
    }
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      continue; /* shouldn't happen on a blocking fd, but be safe */
    }
    std::ostringstream os;
    os << "SSL_read err=" << e;
    err = os.str();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    platform_socket_destroy(sock);
    return -1;
  }

  SSL_free(ssl);
  SSL_CTX_free(ctx);
  platform_socket_destroy(sock);

  if (content_length == (size_t)-1) {
    err = "no header/body separator received";
    return -1;
  }
  if (all.size() < body_start + content_length) {
    std::ostringstream os;
    os << "short body: have " << (all.size() - body_start) << " of " << content_length;
    err = os.str();
    return -1;
  }
  out_body.assign(all, body_start, content_length);
  return 0;
}

class TestHttpServerSsl : public testing::Test {
public:
  scheduler_pool_t* pool;
  http_server_t* server;
  uint16_t port;

  void SetUp() override {
    static std::atomic<uint16_t> next_port{19456};
    port = next_port.fetch_add(1) + (uint16_t)((platform_getpid() % 97) * 4);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    server = NULL;
  }

  void TearDown() override {
    if (server != NULL) {
      http_server_stop(server);
    }
    if (pool != NULL) {
      scheduler_pool_wait_for_idle(pool);
      scheduler_pool_stop(pool);
    }
    if (server != NULL) {
      http_server_destroy(server);
    }
    if (pool != NULL) {
      scheduler_pool_destroy(pool);
    }
  }
};

TEST_F(TestHttpServerSsl, SmallGet) {
  server = http_server_create_ssl(pool, "127.0.0.1", port, kCertPath, kKeyPath);
  ASSERT_TRUE(server != NULL) << "could not create SSL server (certs missing?)";
  http_server_get(server, "^/small$", _handle_small, NULL);
  http_server_listen(server);

  std::string body;
  std::string err;
  ASSERT_EQ(_tls_request(port, "GET", "/small", "", body, err), 0) << err;
  EXPECT_EQ(body, std::string("Hello, TLS!"));
}

TEST_F(TestHttpServerSsl, LargeGet) {
  server = http_server_create_ssl(pool, "127.0.0.1", port, kCertPath, kKeyPath);
  ASSERT_TRUE(server != NULL);
  http_server_get(server, "^/large$", _handle_large, NULL);
  http_server_listen(server);

  std::string body;
  std::string err;
  ASSERT_EQ(_tls_request(port, "GET", "/large", "", body, err), 0) << err;
  EXPECT_EQ(body.size(), kLargeSize);
  std::vector<char>& expected = large_body();
  EXPECT_EQ(body, std::string(expected.begin(), expected.end()));
}

TEST_F(TestHttpServerSsl, LargePostEcho) {
  server = http_server_create_ssl(pool, "127.0.0.1", port, kCertPath, kKeyPath);
  ASSERT_TRUE(server != NULL);
  http_server_post(server, "^/echo$", _handle_echo, NULL);
  http_server_listen(server);

  std::vector<char>& expected = large_body();
  std::string req_body(expected.begin(), expected.end());
  std::string body;
  std::string err;
  ASSERT_EQ(_tls_request(port, "POST", "/echo", req_body, body, err), 0) << err;
  EXPECT_EQ(body.size(), kLargeSize);
  EXPECT_EQ(body, req_body);
}

} // namespace