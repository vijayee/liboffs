//
// Created by victor on 5/7/26.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/HTTP/http_server.h"
#include "../src/HTTP/http_request.h"
#include "../src/HTTP/http_response.h"
#include "../src/HTTP/http_connection.h"
#include "../src/HTTP/http_route.h"
#include "../src/HTTP/http_headers.h"
#include "../src/HTTP/cors.h"
#include "../src/Scheduler/scheduler.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <poll.h>

#ifdef _WIN32
#define platform_usleep(ms) Sleep(ms)
#else
#define platform_usleep(us) usleep(us)
#endif
}

namespace http_test {

static void _test_get_handler(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "text/plain");
  http_response_write(response, "Hello, World!", 13);
  http_response_end(response);
}

static void _test_post_handler(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_status(response, HTTP_STATUS_CREATED);
  http_response_end(response);
}

static void _test_put_handler(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_end(response);
}

static void _test_delete_handler(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_end(response);
}

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
    port = _next_port++;
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
  }

  void TearDown() override {
    if (server != NULL) {
      http_server_stop(server);
      http_server_destroy(server);
    }
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
  }
};

static int _connect_to_server(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int _send_and_recv(int fd, const char* request, char* response, size_t response_size) {
  size_t req_len = strlen(request);
  ssize_t sent = send(fd, request, req_len, 0);
  if (sent != (ssize_t)req_len) return -1;

  size_t total_received = 0;
  for (int attempts = 0; attempts < 100; attempts++) {
    struct pollfd poll_fd;
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    int poll_result = poll(&poll_fd, 1, 10);
    if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
      ssize_t received = recv(fd, response + total_received, response_size - total_received - 1, 0);
      if (received > 0) {
        total_received += (size_t)received;
        response[total_received] = '\0';
        char* header_end = strstr(response, "\r\n\r\n");
        if (header_end != NULL) {
          size_t header_len = (header_end - response) + 4;
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
      }
    }
  }

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

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestPostRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_post(server, "^/items$", _test_post_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "POST /items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "201"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestPutRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_put(server, "^/items/([0-9]+)$", _test_put_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "PUT /items/42 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestDeleteRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_delete(server, "^/items/([0-9]+)$", _test_delete_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "DELETE /items/42 HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestRequestBody) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_post(server, "^/echo$", _test_echo_body_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nHello World";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello World"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestNotFoundRoute) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_get(server, "^/exists$", _test_get_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  close(fd);
}

// --- Middleware Tests ---

static int _test_middleware_stop(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "X-Middleware", "stopped");
  http_response_end(response);
  return 1;
}

static int _test_middleware_continue(http_request_t* request, http_response_t* response, void* user_data) {
  http_response_set_header(response, "X-Middleware", "passed");
  return 0;
}

TEST_F(TestHttpServer, TestMiddlewareStopsChain) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_use(server, _test_middleware_stop, NULL, NULL);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "X-Middleware: stopped"), nullptr);
  EXPECT_EQ(strstr(response, "Hello, World!"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestMiddlewareContinuesChain) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  http_server_use(server, _test_middleware_continue, NULL, NULL);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "X-Middleware: passed"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  close(fd);
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

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "OPTIONS /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "204"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Origin: *"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Methods:"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Headers:"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Max-Age:"), nullptr);

  close(fd);
}

TEST_F(TestHttpServer, TestCorsOnGetRequest) {
  server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_TRUE(server != NULL);

  cors_config_t* cors_config = cors_config_offsystem();
  http_server_use(server, cors_middleware, cors_config,
                  (void (*)(void*))cors_config_destroy);
  http_server_get(server, "^/hello$", _test_get_handler, NULL);
  http_server_listen(server);

  int fd = -1;
  for (int attempts = 0; attempts < 50; attempts++) {
    platform_usleep(10000);
    fd = _connect_to_server(port);
    if (fd >= 0) break;
  }
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Access-Control-Allow-Origin: *"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  close(fd);
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

} // namespace http_test