#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/ClientAPI/health_handler.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Network/topology_metrics.h"
#include <time.h>
}

TEST(HealthHandler, CollectNullContext) {
  health_data_t data = health_data_collect(NULL);
  EXPECT_STREQ(data.status, "unknown");
  EXPECT_EQ(data.uptime_seconds, 0u);
  EXPECT_EQ(data.node_id_str[0], '\0');
  EXPECT_EQ(data.peer_count, 0u);
  EXPECT_EQ(data.total_connections, 0u);
  EXPECT_EQ(data.avg_hebbian_weight, 0.0f);
}

TEST(HealthHandler, CollectNullFields) {
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "unknown");
}

TEST(HealthHandler, CollectRunningStatus) {
  uint8_t running = 1;
  uint8_t draining = 0;
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "running");
}

TEST(HealthHandler, CollectDrainingStatus) {
  uint8_t running = 0;
  uint8_t draining = 1;
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "draining");
}

TEST(HealthHandler, CollectStoppedStatus) {
  uint8_t running = 0;
  uint8_t draining = 0;
  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.running = &running;
  ctx.draining = &draining;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.status, "stopped");
}

TEST(HealthHandler, CollectUptime) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t past_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000 - 5000;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.start_time_ms = &past_ms;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_GE(data.uptime_seconds, 5u);
  EXPECT_LE(data.uptime_seconds, 6u);
}

TEST(HealthHandler, CollectNodeId) {
  node_id_t node;
  memset(&node, 0, sizeof(node));
  strncpy(node.str, "test-node-123", sizeof(node.str) - 1);

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.node_id = &node;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_STREQ(data.node_id_str, "test-node-123");
}

TEST(HealthHandler, CollectTopologyMetrics) {
  scheduler_pool_t* pool = scheduler_pool_create(1);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  topology_metrics_t* metrics = topology_metrics_create(pool);
  ASSERT_NE(metrics, nullptr);
  metrics->peer_snapshot_count = 3;
  metrics->total_connections = 7;
  metrics->avg_hebbian_weight = 0.75f;
  metrics->total_rate_limit_accepted[0] = 100;
  metrics->total_rate_limit_rejected[0] = 5;
  metrics->total_rpc_calls[0] = 42;

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.topology_metrics = metrics;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_EQ(data.peer_count, 3u);
  EXPECT_EQ(data.total_connections, 7u);
  EXPECT_FLOAT_EQ(data.avg_hebbian_weight, 0.75f);
  EXPECT_EQ(data.rate_limit_accepted[0], 100u);
  EXPECT_EQ(data.rate_limit_rejected[0], 5u);
  EXPECT_EQ(data.total_rpc_calls[0], 42u);

  topology_metrics_destroy(metrics);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(HealthHandler, CollectBlockCacheStats) {
  scheduler_pool_t* pool = scheduler_pool_create(1);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create();
  ASSERT_NE(timer, nullptr);

  config_t config = config_default();
  block_cache_t* bc = block_cache_create(config, (char*)"/tmp/test_health_bc",
      standard, timer, pool, NULL, 1024 * 1024);
  ASSERT_NE(bc, nullptr);

  health_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.block_cache = bc;

  health_data_t data = health_data_collect(&ctx);
  EXPECT_EQ(data.block_cache_max_bytes, 1024u * 1024u);
  EXPECT_EQ(data.block_cache_current_bytes, 0u);
  EXPECT_EQ(data.block_cache_block_count, 0u);

  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(HealthHandler, ToJsonProducesValidOutput) {
  health_data_t data;
  memset(&data, 0, sizeof(data));
  data.status = "running";
  data.uptime_seconds = 3600;
  data.peer_count = 5;
  data.total_connections = 10;

  char buf[4096];
  size_t len = health_data_to_json(&data, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  EXPECT_LT(len, sizeof(buf));

  EXPECT_NE(strstr(buf, "\"status\": \"running\""), nullptr);
  EXPECT_NE(strstr(buf, "\"uptime_seconds\": 3600"), nullptr);
  EXPECT_NE(strstr(buf, "\"peer_count\": 5"), nullptr);
  EXPECT_NE(strstr(buf, "\"total_connections\": 10"), nullptr);
}

TEST(HealthHandler, ToJsonHandlesSmallBuffer) {
  health_data_t data;
  memset(&data, 0, sizeof(data));
  data.status = "running";

  char buf[8];
  size_t len = health_data_to_json(&data, buf, sizeof(buf));
  EXPECT_EQ(len, 0u);
}

TEST(HealthHandler, ToJsonIncludesRpcCalls) {
  health_data_t data;
  memset(&data, 0, sizeof(data));
  data.status = "running";
  data.total_rpc_calls[0] = 99;

  char buf[4096];
  size_t len = health_data_to_json(&data, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  EXPECT_NE(strstr(buf, "\"name\": \"ping\""), nullptr);
  EXPECT_NE(strstr(buf, "\"count\": 99"), nullptr);
}

TEST(HealthHandler, ToJsonSkipsZeroRpcCalls) {
  health_data_t data;
  memset(&data, 0, sizeof(data));
  data.status = "running";

  char buf[4096];
  size_t len = health_data_to_json(&data, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  EXPECT_EQ(strstr(buf, "\"name\": \"ping\""), nullptr);
}
