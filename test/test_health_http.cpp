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
#include "../src/Platform/platform.h"
#include "../src/Platform/platform_socket.h"
#include <stdlib.h>
}

namespace health_http_test {

static uint16_t _next_port = 19200;

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
  /* Nonblocking for the recv loop below so a slow server response doesn't
     hang the test; readiness is handled by a 10ms backoff poll. */
  platform_socket_set_nonblocking(sock);
  return sock;
}

static platform_socket_t* _connect_to_server_with_retry(uint16_t port, int max_attempts) {
  for (int attempts = 0; attempts < max_attempts; attempts++) {
    platform_socket_t* sock = _connect_to_server(port);
    if (sock != NULL) return sock;
    platform_sleep_ms(10);
  }
  return NULL;
}

static int _send_all(platform_socket_t* sock, const char* buf, size_t len) {
  size_t sent_total = 0;
  while (sent_total < len) {
    ssize_t sent = platform_socket_send(sock, buf + sent_total, len - sent_total);
    if (sent < 0) {
      /* EAGAIN / no socket buffer space right now: back off and retry. */
      platform_sleep_ms(10);
      continue;
    }
    if (sent == 0) return -1;
    sent_total += (size_t)sent;
  }
  return 0;
}

static int _send_and_recv(platform_socket_t* sock, const char* request, size_t req_len,
                          char* response, size_t response_size, int timeout_ms) {
  if (_send_all(sock, request, req_len) != 0) return -1;

  size_t total_received = 0;
  for (int attempts = 0; attempts < timeout_ms / 10; attempts++) {
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
          if (total_received >= header_len + content_length) return 0;
        }
        if (strstr(response, "Connection: close") != NULL &&
            total_received > header_len) return 0;
      }
    } else if (received == 0) {
      /* EOF: the server closed (Connection: close). Whatever we have is the
         full response. */
      response[total_received] = '\0';
      return (total_received > 0) ? 0 : -1;
    } else {
      /* EAGAIN / no data right now: back off briefly. */
      platform_sleep_ms(10);
    }
  }
  response[total_received] = '\0';
  return (total_received > 0) ? 0 : -1;
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

  platform_socket_t* sock = _connect_to_server_with_retry(port, 50);
  ASSERT_NE(sock, (platform_socket_t*)nullptr);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[16384];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(sock, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "200"), nullptr);
  EXPECT_NE(strstr(response, "application/json"), nullptr);
  EXPECT_NE(strstr(response, "\"status\""), nullptr);
  EXPECT_NE(strstr(response, "\"running\""), nullptr);
  EXPECT_NE(strstr(response, "\"block_cache\""), nullptr);

  platform_socket_destroy(sock);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
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

  platform_socket_t* sock = _connect_to_server_with_retry(port, 50);
  ASSERT_NE(sock, (platform_socket_t*)nullptr);

  const char* request = "GET /other HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[4096];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(sock, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  platform_socket_destroy(sock);
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

  platform_socket_t* sock = _connect_to_server_with_retry(port, 50);
  ASSERT_NE(sock, (platform_socket_t*)nullptr);

  const char* request = "POST /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  char response[4096];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(sock, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "404"), nullptr);

  platform_socket_destroy(sock);
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

  platform_socket_t* sock = _connect_to_server_with_retry(port, 50);
  ASSERT_NE(sock, (platform_socket_t*)nullptr);

  const char* request = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char response[16384];
  memset(response, 0, sizeof(response));
  int ret = _send_and_recv(sock, request, strlen(request), response, sizeof(response), 5000);
  ASSERT_EQ(ret, 0);

  EXPECT_NE(strstr(response, "\"status\""), nullptr);
  EXPECT_NE(strstr(response, "\"uptime_seconds\""), nullptr);
  EXPECT_NE(strstr(response, "\"peer_count\""), nullptr);
  EXPECT_NE(strstr(response, "\"total_connections\""), nullptr);
  EXPECT_NE(strstr(response, "\"avg_hebbian_weight\""), nullptr);
  EXPECT_NE(strstr(response, "\"block_cache\""), nullptr);
  EXPECT_NE(strstr(response, "\"rate_limits\""), nullptr);
  EXPECT_NE(strstr(response, "\"rpc_calls\""), nullptr);

  platform_socket_destroy(sock);
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
}

}  // namespace health_http_test
