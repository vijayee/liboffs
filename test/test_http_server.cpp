//
// Created by victor on 5/7/26.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/ClientAPI/HTTP/http_server.h"
#include "../src/ClientAPI/HTTP/http_request.h"
#include "../src/ClientAPI/HTTP/http_response.h"
#include "../src/ClientAPI/HTTP/http_connection.h"
#include "../src/ClientAPI/HTTP/http_route.h"
#include "../src/ClientAPI/HTTP/http_headers.h"
#include "../src/ClientAPI/HTTP/cors.h"
#include "../src/ClientAPI/HTTP/auth_middleware.h"
#include "../src/ClientAPI/HTTP/config_routes.h"
#include "../src/Configuration/config.h"
#include "../src/Node/node.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* usleep is POSIX-only; platform_sleep_ms is the cross-platform equivalent.
 * Call sites pass microsecond values (e.g. 10000 == 10ms), so divide by 1000.
 * The previous Win32 branch slept in milliseconds, making 10000 mean 10s. */
#define platform_usleep(us) platform_sleep_ms((us) / 1000)
}

namespace http_test {

static void _test_get_handler(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "text/plain");
  http_response_write(response, "Hello, World!", 13);
  http_response_end(response);
}

static void _test_post_handler(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_CREATED);
  http_response_end(response);
}

static void _test_put_handler(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_end(response);
}

static void _test_delete_handler(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_end(response);
}

PLATFORM_UNUSED
static void _test_param_handler(http_request_t* request, http_response_t* response, void* user_data) {
  const char* name = http_request_param(request, 1);
  if (name != NULL) {
    http_response_set_status(response, HTTP_STATUS_OK);
    http_response_write(response, name, strlen(name));
  } else {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
  }
  http_response_end(response);
}

static void _test_echo_body_handler(http_request_t* request, http_response_t* response, void* user_data) {
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  if (request->body != NULL && request->body->size > 0) {
    http_response_write(response, (const char*)request->body->data, request->body->size);
  }
  http_response_end(response);
}

// --- Unit Tests ---

TEST(TestHttpHeaders, TestSetGetRemove) {
  http_headers_t headers;
  http_headers_init(&headers);

  http_headers_set(&headers, "Content-Type", "text/html");
  http_headers_set(&headers, "Content-Length", "42");
  http_headers_set(&headers, "Host", "localhost:8080");

  EXPECT_STREQ(http_headers_get(&headers, "Content-Type"), "text/html");
  EXPECT_STREQ(http_headers_get(&headers, "content-type"), "text/html");
  EXPECT_STREQ(http_headers_get(&headers, "CONTENT-TYPE"), "text/html");
  EXPECT_STREQ(http_headers_get(&headers, "Content-Length"), "42");
  EXPECT_STREQ(http_headers_get(&headers, "Host"), "localhost:8080");
  EXPECT_EQ(http_headers_get(&headers, "X-Not-Found"), nullptr);

  http_headers_set(&headers, "Content-Type", "application/json");
  EXPECT_STREQ(http_headers_get(&headers, "Content-Type"), "application/json");
  EXPECT_EQ(http_headers_count(&headers), 3u);

  http_headers_remove(&headers, "Content-Length");
  EXPECT_EQ(http_headers_get(&headers, "Content-Length"), nullptr);
  EXPECT_EQ(http_headers_count(&headers), 2u);

  http_headers_deinit(&headers);
}

TEST(TestHttpRoute, TestRouteMatch) {
  http_route_t route;
  http_route_init(&route, HTTP_GET, "^/hello$", NULL, NULL, NULL);

  vec_capture_t captures;
  vec_init(&captures);

  EXPECT_EQ(http_route_match(&route, HTTP_GET, "/hello", &captures), 1);
  vec_capture_deinit(&captures);

  EXPECT_EQ(http_route_match(&route, HTTP_POST, "/hello", &captures), 0);
  EXPECT_EQ(http_route_match(&route, HTTP_GET, "/goodbye", &captures), 0);

  http_route_deinit(&route);
}

TEST(TestHttpRoute, TestRouteCaptureGroups) {
  http_route_t route;
  http_route_init(&route, HTTP_GET, "^/users/([a-zA-Z0-9_-]+)/?$", NULL, NULL, NULL);

  vec_capture_t captures;
  vec_init(&captures);

  EXPECT_EQ(http_route_match(&route, HTTP_GET, "/users/victor", &captures), 1);
  EXPECT_EQ(captures.length, 2);
  EXPECT_STREQ(captures.data[1].match, "victor");
  vec_capture_deinit(&captures);

  http_route_deinit(&route);
}

TEST(TestHttpStatus, TestStatusPhrase) {
  EXPECT_STREQ(http_status_str(HTTP_STATUS_OK), "OK");
  EXPECT_STREQ(http_status_str(HTTP_STATUS_NOT_FOUND), "Not Found");
  EXPECT_STREQ(http_status_str(HTTP_STATUS_INTERNAL_SERVER_ERROR), "Internal Server Error");
  EXPECT_STREQ(http_status_str(HTTP_STATUS_CREATED), "Created");
  EXPECT_STREQ(http_status_str(HTTP_STATUS_BAD_REQUEST), "Bad Request");
}

// --- Integration Tests ---

static uint16_t _next_port = 18080;

class TestHttpServer : public testing::Test {
public:
  scheduler_pool_t* pool;
  http_server_t* server;
  uint16_t port;

  void SetUp() override {
    port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
  }

  void TearDown() override {
    if (server != NULL) {
      http_server_stop(server);
    }
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    if (server != NULL) {
      http_server_destroy(server);
    }
    scheduler_pool_destroy(pool);
  }
};

static platform_socket_t* _connect_to_server(uint16_t port) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (sock == NULL) return NULL;

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_INET;
  addr.inet.addr = 0x0100007f; /* 127.0.0.1 in network byte order */
  addr.inet.port = port;

  if (platform_socket_connect(sock, &addr) != 0) {
    platform_socket_destroy(sock);
    return NULL;
  }
  platform_socket_set_nonblocking(sock);
  return sock;
}

static int _send_all(platform_socket_t* sock, const char* buf, size_t len) {
  size_t sent_total = 0;
  for (int attempts = 0; attempts < 1000 && sent_total < len; attempts++) {
    ssize_t sent = platform_socket_send(sock, buf + sent_total, len - sent_total);
    if (sent > 0) {
      sent_total += (size_t)sent;
    } else if (sent == 0) {
      return -1;
    } else {
      /* Nonblocking socket: EWOULDBLOCK means try again shortly. */
      platform_sleep_ms(10);
    }
  }
  return (sent_total == len) ? 0 : -1;
}

static int _send_and_recv(platform_socket_t* sock, const char* request,
                          char* response, size_t response_size) {
  size_t req_len = strlen(request);
  if (_send_all(sock, request, req_len) != 0) return -1;

  size_t total_received = 0;
  for (int attempts = 0; attempts < 1000; attempts++) {
    if (total_received + 1 >= response_size) break;
    ssize_t received = platform_socket_recv(sock, response + total_received,
                                             response_size - total_received - 1);
    if (received > 0) {
      total_received += (size_t)received;
      response[total_received] = '\0';
      char* header_end = strstr(response, "\r\n\r\n");
      if (header_end != NULL) {
        size_t header_len = (size_t)(header_end - response) + 4;
        char* content_length_str = strstr(response, "Content-Length: ");
        if (content_length_str != NULL && content_length_str < header_end) {
          size_t content_length = (size_t)atol(content_length_str + 16);
          if (total_received >= header_len + content_length) {
            return 0;
          }
        } else if (strstr(response, "Content-Length: 0") != NULL ||
                   strstr(response, "Connection: close") != NULL) {
          if (total_received > header_len) {
            return 0;
          }
        }
        if (total_received > header_len) {
          return 0;
        }
      }
    } else if (received == 0) {
      /* Peer closed the connection. */
      response[total_received] = '\0';
      return (total_received > 0) ? 0 : -1;
    } else {
      /* EWOULDBLOCK on a nonblocking socket: back off and retry. */
      platform_sleep_ms(10);
    }
  }

  response[total_received] = '\0';
  return total_received > 0 ? 0 : -1;
}

TEST_F(TestHttpServer, TestCreateDestroy) {
  server = http_server_create(pool, "127.0.0.1", port);
  EXPECT_TRUE(server != NULL);
}

TEST_F(TestHttpServer, TestRouteRegistration) {
  server = http_server_create(pool, "127.0.0.1", port);
  EXPECT_TRUE(server != NULL);

  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_post(server, "^/hello$", _test_post_handler, NULL);
  EXPECT_EQ(server->routes.length, 2);
}

TEST_F(TestHttpServer, TestGetRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  platform_socket_destroy(sock);
}

/* When active_connections reaches max_connections, the accept path destroys
   the next accepted socket without serving it. The client sees a quick close
   (recv returns 0/RST) and no HTTP response. This exercises the
   max_connections gate (http_server.c _accept_handler). */
TEST_F(TestHttpServer, MaxConnectionsRefusesExcess) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_set_max_connections(server, 2);
  http_server_listen(server);

  /* Open 2 connections (the limit). Hold them open without sending a request
     so they remain counted as active. */
  platform_socket_t* sock1 = NULL;
  platform_socket_t* sock2 = NULL;
  for (int attempts = 0; attempts < 50 && sock1 == NULL; attempts++) {
    platform_usleep(10000);
    sock1 = _connect_to_server(port);
  }
  for (int attempts = 0; attempts < 50 && sock2 == NULL; attempts++) {
    platform_usleep(10000);
    sock2 = _connect_to_server(port);
  }
  ASSERT_NE(sock1, nullptr);
  ASSERT_NE(sock2, nullptr);

  /* Give the server's I/O loop a moment to accept both so
     active_connections reaches the limit (2). */
  platform_sleep_ms(100);

  /* The 3rd connection exceeds the limit. The accept path destroys the
     server-side socket; the client sees a quick close with no HTTP response.
     The TCP connect may succeed before the server's accept runs, so we send a
     request and inspect the response: a refused connection yields no "200".
     Retry briefly to tolerate the accept loop's async timing. */
  bool refused = false;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    platform_socket_t* sock3 = _connect_to_server(port);
    if (sock3 == NULL) { refused = true; break; }
    char response[256];
    response[0] = '\0';
    _send_and_recv(sock3, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n",
                   response, sizeof(response));
    platform_socket_destroy(sock3);
    if (strstr(response, "200") == NULL) { refused = true; break; }
  }
  EXPECT_TRUE(refused);

  platform_socket_destroy(sock1);
  platform_socket_destroy(sock2);
}

/* Slowloris defense: a connection that sends a partial request (no
   \r\n\r\n) and then goes silent must be closed by the idle timer. With
   idle_timeout_ms = 100 the server should close the socket within a few
   hundred ms. recv returns 0 (peer closed) or -1 (RST) — never a response. */
TEST_F(TestHttpServer, SlowlorisConnectionClosedOnIdleTimeout) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_set_timeouts(server, 100, 1000);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50 && sock == NULL; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
  }
  ASSERT_NE(sock, nullptr);

  /* Send a partial request — no terminating \r\n\r\n, so the request never
     completes and the connection sits idle waiting for more bytes. */
  const char* partial = "GET /hello HTTP/1.1\r\nHost: localhost\r\n";
  EXPECT_EQ(_send_all(sock, partial, strlen(partial)), 0);

  /* Poll recv until the server closes the connection (returns 0 or -1). */
  char buf[64];
  int got = 1;
  for (int attempts = 0; attempts < 50 && got > 0; attempts++) {
    platform_usleep(20000);
    got = (int)platform_socket_recv(sock, buf, sizeof(buf));
  }
  EXPECT_LE(got, 0);

  platform_socket_destroy(sock);
}

/* A normal request (full \r\n\r\n) that completes within the idle timeout
   must not be closed by the timer. The route returns 200 + "Hello, World!". */
TEST_F(TestHttpServer, NormalRequestNotTimedOut) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_set_timeouts(server, 100, 1000);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50 && sock == NULL; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);
  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestPostRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_post(server, "^/items$", _test_post_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "POST /items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "201"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestPutRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_put(server, "^/items/([0-9]+)$", _test_put_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "PUT /items/42 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestDeleteRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_delete(server, "^/items/([0-9]+)$", _test_delete_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "DELETE /items/42 HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestRequestBody) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_post(server, "^/echo$", _test_echo_body_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nHello World";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello World"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestNotFoundRoute) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/exists$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  platform_socket_destroy(sock);
}

// --- Middleware Tests ---

static int _test_middleware_stop(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "X-Middleware", "stopped");
  http_response_end(response);
  return 1;
}

static int _test_middleware_continue(http_request_t* request, http_response_t* response, void* user_data) {
  (void)request;
  (void)user_data;
  http_response_set_header(response, "X-Middleware", "passed");
  return 0;
}

TEST_F(TestHttpServer, TestMiddlewareStopsChain) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_use(server, _test_middleware_stop, NULL, NULL);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "X-Middleware: stopped"), nullptr);
  EXPECT_EQ(strstr(response, "Hello, World!"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestMiddlewareContinuesChain) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_use(server, _test_middleware_continue, NULL, NULL);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "X-Middleware: passed"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  platform_socket_destroy(sock);
}

// --- CORS Tests ---

TEST_F(TestHttpServer, TestCorsPreflight) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  cors_config_t* cors_config = cors_config_offsystem();
  http_server_use(server, cors_middleware, cors_config,
                  (void (*)(void*))cors_config_destroy);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "OPTIONS /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "204"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Origin: *"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Methods:"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Headers:"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Max-Age:"), nullptr);

  platform_socket_destroy(sock);
}

TEST_F(TestHttpServer, TestCorsOnGetRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  cors_config_t* cors_config = cors_config_offsystem();
  http_server_use(server, cors_middleware, cors_config,
                  (void (*)(void*))cors_config_destroy);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  platform_socket_t* sock = NULL;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    sock = _connect_to_server(port);
    if (sock != NULL) break;
  }
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Origin: *"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  platform_socket_destroy(sock);
}

TEST(TestCorsConfig, TestDefaultConfig) {
  cors_config_t* config = cors_config_default();
  ASSERT_TRUE(config != NULL);
  EXPECT_STREQ(config->allow_origin, "*");
  EXPECT_STREQ(config->allow_methods, "GET, PUT, POST, DELETE, OPTIONS");
  EXPECT_STREQ(config->allow_headers, "Content-Type");
  EXPECT_STREQ(config->max_age, "86400");
  EXPECT_EQ(config->allow_credentials, 0);
  cors_config_destroy(config);
}

TEST(TestCorsConfig, TestOffsystemConfig) {
  cors_config_t* config = cors_config_offsystem();
  ASSERT_TRUE(config != NULL);
  EXPECT_STREQ(config->allow_origin, "*");
  EXPECT_NE(strstr(config->allow_headers, "type"), nullptr);
  EXPECT_NE(strstr(config->allow_headers, "file-name"), nullptr);
  EXPECT_NE(strstr(config->allow_headers, "stream-length"), nullptr);
  EXPECT_NE(strstr(config->allow_headers, "server-address"), nullptr);
  cors_config_destroy(config);
}

// --- Local-Binding Auth Tests ---
//
// config_local_binding_no_auth (default false) controls whether the auth
// middleware skips bearer on loopback. Default: bearer required even on
// 127.0.0.1. Opt-out (true): no-bearer allowed on loopback. Config mutations
// (PUT /config, POST /config/restart) are refused on non-loopback bindings
// regardless of bearer.

static const char* k_local_auth_test_hash =
    "$2b$04$MTIzNDU2Nzg5MDEyMzQ1NePheb5yq4/5.giE2KzFrDwx2yMnwVtpW"; /* "test-key" */
static const char* k_local_auth_test_key = "test-key";

class LocalBindingAuth : public testing::Test {
public:
  scheduler_pool_t* pool;
  http_server_t* server;
  uint16_t port;
  config_t config;
  offs_node_t node;
  char* data_dir;

  void SetUp() override {
    port = _next_port++ + (uint16_t)((platform_getpid() % 127) * 100);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    server = NULL;
    memset(&node, 0, sizeof(node));
    config = config_default();
    config.api_key_hash = strdup(k_local_auth_test_hash);
    node.config = &config;
    char dir_template[] = "/tmp/test_local_auth_XXXXXX";
    data_dir = mkdtemp(dir_template);
    ASSERT_NE(data_dir, nullptr);
    data_dir = strdup(data_dir);
  }

  void TearDown() override {
    if (server != NULL) {
      http_server_stop(server);
    }
    scheduler_pool_wait_for_idle(pool);
    scheduler_pool_stop(pool);
    if (server != NULL) {
      http_server_destroy(server);
    }
    scheduler_pool_destroy(pool);
    rmdir(data_dir);
    free(data_dir);
    free(config.api_key_hash);
    config.api_key_hash = NULL;
  }

  /* Create the server bound to host, register auth middleware + config routes,
     and start listening. */
  void setup_server(const char* host, bool local_no_auth) {
    server = http_server_create(pool, host, port);
    ASSERT_TRUE(server != NULL);
    node.http_server = server;
    config.config_local_binding_no_auth = local_no_auth;

    auth_middleware_t* auth = auth_middleware_create(config.api_key_hash,
                                                      local_no_auth, server);
    ASSERT_TRUE(auth != NULL);
    http_server_use(server, auth_middleware_handler(), auth,
                    (void (*)(void*))auth_middleware_destroy);

    config_routes_register(server, &node, &config, data_dir, NULL, NULL);
    http_server_listen(server);
  }

  platform_socket_t* connect() {
    platform_socket_t* sock = NULL;
    for (int attempts = 0; attempts < 50; attempts++) {
      platform_usleep(10000);
      sock = _connect_to_server(port);
      if (sock != NULL) break;
    }
    return sock;
  }
};

/* Default (flag=false): no bearer on loopback → 401 from auth middleware. */
TEST_F(LocalBindingAuth, BearerRequiredOnLoopbackByDefault) {
  setup_server("127.0.0.1", false);
  platform_socket_t* sock = connect();
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request =
      "PUT /config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 2\r\n\r\n"
      "{}";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);
  EXPECT_NE(strstr(response, "401"), nullptr);

  platform_socket_destroy(sock);
}

/* Opt-out (flag=true): no bearer on loopback → 200 from config PUT handler. */
TEST_F(LocalBindingAuth, NoAuthOnLoopbackWhenOptOut) {
  setup_server("127.0.0.1", true);
  platform_socket_t* sock = connect();
  ASSERT_NE(sock, nullptr);

  char response[4096];
  const char* request =
      "PUT /config HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 2\r\n\r\n"
      "{}";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);
  EXPECT_NE(strstr(response, "200"), nullptr);

  platform_socket_destroy(sock);
}

/* Non-loopback binding: even with a valid bearer, config mutation → 403. */
TEST_F(LocalBindingAuth, ConfigMutationRefusedNonLoopback) {
  setup_server("0.0.0.0", false);
  platform_socket_t* sock = connect();
  ASSERT_NE(sock, nullptr);

  char response[4096];
  std::string request =
      std::string("PUT /config HTTP/1.1\r\n") +
      "Host: localhost\r\n" +
      "Authorization: Bearer " + k_local_auth_test_key + "\r\n" +
      "Content-Length: 2\r\n\r\n" +
      "{}";
  int result = _send_and_recv(sock, request.c_str(), response, sizeof(response));
  EXPECT_EQ(result, 0);
  EXPECT_NE(strstr(response, "403"), nullptr);

  platform_socket_destroy(sock);
}

/* Opt-out (flag=true): GET /config on loopback with no bearer → 200. */
TEST_F(LocalBindingAuth, ConfigGetAllowedOnLoopback) {
  setup_server("127.0.0.1", true);
  platform_socket_t* sock = connect();
  ASSERT_NE(sock, nullptr);

  char response[8192];
  const char* request = "GET /config HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(sock, request, response, sizeof(response));
  EXPECT_EQ(result, 0);
  EXPECT_NE(strstr(response, "200"), nullptr);

  platform_socket_destroy(sock);
}

} // namespace http_test