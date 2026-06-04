#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientAPI/HTTP/health_routes.h"
#include "../src/ClientAPI/HTTP/http_server.h"
#include "../src/ClientAPI/health_handler.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
}

namespace health_http_test {

static uint16_t _next_port = 19200;

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

static int _send_and_recv(int fd, const char* request, size_t req_len,
                          char* response, size_t response_size, int timeout_ms) {
  ssize_t sent = send(fd, request, req_len, 0);
  if (sent != (ssize_t)req_len) return -1;

  size_t total_received = 0;
  for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
    struct pollfd poll_fd;
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    int poll_result = poll(&poll_fd, 1, 10);
    if (poll_result > 0 && (poll_fd.revents & POLLIN)) {
      ssize_t received = recv(fd, response + total_received,
                              response_size - total_received - 1, 0);
      if (received > 0) {
        total_received += (size_t)received;
        response[total_received] = '\0';
        char* header_end = strstr(response, "\r\n\r\n");
        if (header_end != NULL) {
          size_t header_len = (header_end - response) + 4;
          char* content_length_str = strstr(response, "Content-Length: ");
          if (content_length_str != NULL && content_length_str < header_end) {
            size_t content_length = (size_t)atol(content_length_str + 16);
            if (total_received >= header_len + content_length) return 0;
          }
          if (strstr(response, "Connection: close") != NULL &&
              total_received > header_len) return 0;
        }
      }
    }
    if (poll_result < 0 && errno != EINTR) return -1;
  }
  return -1;
}

TEST(HealthHTTP, GetHealthReturns200) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  config_t config = config_default();
  block_cache_t* bc = block_cache_create(config, (char*)"/tmp/test_health_http",
      standard, timer, pool, NULL, 1024 * 1024);
  ASSERT_NE(bc, nullptr);

  uint16_t port = _next_port++;
  http_server_t* server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_NE(server, nullptr);

  uint8_t running = 1;
  uint8_t draining = 0;
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.block_cache = bc;
  ctx.running = &running;
  ctx.draining = &draining;

  health_routes_register(server, &ctx);
  http_server_listen(server);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[16384];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "application/json"), nullptr);
  EXPECT_NE(strstr(response, "\"status\""), nullptr);
  EXPECT_NE(strstr(response, "\"running\""), nullptr);
  EXPECT_NE(strstr(response, "\"block_cache\""), nullptr);

  close(fd);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  timer_actor_destroy(timer);
  block_cache_destroy(bc);
  scheduler_pool_destroy(pool);
}

TEST(HealthHTTP, NonHealthPathNotIntercepted) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  uint16_t port = _next_port++;
  http_server_t* server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_NE(server, nullptr);

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  health_routes_register(server, &ctx);
  http_server_listen(server);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "GET /other HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[4096];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  close(fd);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(HealthHTTP, PostHealthNotIntercepted) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  uint16_t port = _next_port++;
  http_server_t* server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_NE(server, nullptr);

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  health_routes_register(server, &ctx);
  http_server_listen(server);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "POST /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  char response[4096];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  close(fd);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(HealthHTTP, HealthResponseContainsExpectedFields) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);
  ASSERT_NE(timer, nullptr);

  config_t config = config_default();
  block_cache_t* bc = block_cache_create(config, (char*)"/tmp/test_health_http2",
      standard, timer, pool, NULL, 1024 * 1024);
  ASSERT_NE(bc, nullptr);

  uint16_t port = _next_port++;
  http_server_t* server = http_server_create(pool, "127.0.0.1", port);
  ASSERT_NE(server, nullptr);

  uint8_t running = 1;
  uint8_t draining = 0;
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.block_cache = bc;
  ctx.running = &running;
  ctx.draining = &draining;

  health_routes_register(server, &ctx);
  http_server_listen(server);

  int fd = _connect_to_server(port);
  ASSERT_GE(fd, 0);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[16384];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(fd, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "\"status\""), nullptr);
  EXPECT_NE(strstr(response, "\"uptime_seconds\""), nullptr);
  EXPECT_NE(strstr(response, "\"peer_count\""), nullptr);
  EXPECT_NE(strstr(response, "\"total_connections\""), nullptr);
  EXPECT_NE(strstr(response, "\"avg_hebbian_weight\""), nullptr);
  EXPECT_NE(strstr(response, "\"block_cache\""), nullptr);
  EXPECT_NE(strstr(response, "\"rate_limits\""), nullptr);
  EXPECT_NE(strstr(response, "\"rpc_calls\""), nullptr);

  close(fd);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  timer_actor_destroy(timer);
  block_cache_destroy(bc);
  scheduler_pool_destroy(pool);
}

}  // namespace health_http_test
