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
#include "../src/Scheduler/scheduler.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
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
  EXPECT_GE(captures.length, 2);
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

class TestHttpServer : public testing::Test {
public:
  scheduler_pool_t* pool;
  http_server_t* server;
  uint16_t port = 18080;

  void SetUp() override {
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

  usleep(100000);

  ssize_t received = recv(fd, response, response_size - 1, 0);
  if (received < 0) return -1;
  response[received] = '\0';
  return 0;
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

  usleep(50000);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  char response[4096];
  const char* request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  int result = _send_and_recv(fd, request, response, sizeof(response));
  EXPECT_EQ(result, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "Hello, World!"), nullptr);

  close(fd);
}

} // namespace http_test