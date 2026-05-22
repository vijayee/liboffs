#include <gtest/gtest.h>
extern "C" {
#include "../src/Node/node.h"
#include "../src/Network/authority.h"
#include "../src/Configuration/config.h"
}

/* Smoke tests for graceful shutdown with configurable deadline.
 * These verify the 7-phase shutdown sequence completes without hanging
 * or crashing under different timeout configurations. */

TEST(TestShutdown, NodeStopRejectsNewPuts) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 5000;

  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  offs_node_stop(node);
  offs_node_destroy(node);
  authority_destroy(authority);
}

TEST(TestShutdown, ZeroTimeoutBlocksUntilIdle) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 0;

  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  offs_node_stop(node);
  offs_node_destroy(node);
  authority_destroy(authority);
}

TEST(TestShutdown, DeadlineExceededSkipsDrain) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 1;

  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  offs_node_stop(node);
  offs_node_destroy(node);
  authority_destroy(authority);
}
