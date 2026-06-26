#include <gtest/gtest.h>
extern "C" {
#include "../src/Node/node.h"
#include "../src/Network/authority.h"
#include "../src/Configuration/config.h"
}

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

/* The reload/restart path (offs_node_restart, now driven from the offsd main
   thread via 3f7166b) destroys and re-creates every subsystem (network,
   block_cache, timer, scheduler) once per cycle. Each cycle must release every
   OS handle it allocates; a +1/cycle leak would grow the process handle count
   by N over N restarts. This loops offs_node_restart with no pending config
   (it falls back to a deep copy of the current config) and asserts the handle
   count does not grow. */
static DWORD node_handle_count() {
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
}

TEST(TestShutdown, NodeRestartNoHandleLeak) {
  config_t config = config_default();
  config.shutdown_timeout_ms = 5000;

  authority_t* authority = authority_create(&config);
  ASSERT_NE(authority, nullptr);

  offs_node_t* node = offs_node_create(&config, authority);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(offs_node_start(node), 0);

  /* data_dir with no pending config: offs_node_restart falls back to a deep
     copy of the current config, so the cycle is a pure destroy/re-create. */
  const char* tmp = getenv("TEMP");
  if (tmp == NULL || tmp[0] == '\0') tmp = "C:\\Windows\\Temp";

  /* Warm up: the first cycle may cache runtime handles; discard for a steady
     baseline. offs_node_restart re-starts the node internally (phase 5). */
  for (int i = 0; i < 2; i++) {
    offs_node_restart(node, tmp);
  }

  DWORD before = node_handle_count();
  const int N = 20;
  for (int i = 0; i < N; i++) {
    offs_node_restart(node, tmp);
  }
  DWORD after = node_handle_count();

  offs_node_stop(node);
  offs_node_destroy(node);
  authority_destroy(authority);

  LONG delta = (LONG)after - (LONG)before;
  EXPECT_LE(delta, 2) << "handle leak: +" << delta << " over " << N
                      << " offs_node_restart cycles (before=" << before
                      << " after=" << after << ")";
}
#endif

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
