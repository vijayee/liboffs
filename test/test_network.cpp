//
// Created by victor on 5/14/25.
//

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <unistd.h>

extern "C" {
#include "Network/network.h"
#include "Network/query.h"
#include "Network/gossip.h"
#include "Network/ring.h"
#include "Network/ring_set.h"
#include "Network/latency_cache.h"
#include "Network/net_node.h"
#include "Network/node_id.h"
#include "Network/authority.h"
#include "Network/find_block.h"
#include "Network/store_block.h"
#include "Network/eabf.h"
#include "Network/hebbian.h"
#include "Network/respiration.h"
#include "Network/rate_limit.h"
#include "Network/timing_wheel.h"
#include "Network/hebbian_config.h"
#include "Network/wire.h"
#include "Network/peer_connection.h"
#include "Network/connection_manager.h"
#include "Network/topology_metrics.h"
#include "Configuration/config.h"
#include "Util/allocator.h"
}

// === Query tests ===

class QueryTest : public ::testing::Test {
protected:
  void SetUp() override {
    query = query_create(1, QUERY_TYPE_GOSSIP, 5000);
  }
  void TearDown() override {
    query_destroy(query);
  }
  query_t* query;
};

TEST_F(QueryTest, CreateDestroy) {
  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->query_id, 1u);
  EXPECT_EQ(query->type, QUERY_TYPE_GOSSIP);
  EXPECT_EQ(query->status, QUERY_STATUS_INIT);
  EXPECT_EQ(query->num_targets, 0u);
}

TEST_F(QueryTest, AddTargets) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  net_node_t* node1 = net_node_create(&id1, 0x0A000001, 8080);
  ASSERT_NE(node1, nullptr);

  node_id_t id2 = {};
  memset(id2.hash, 0xBB, NODE_ID_HASH_SIZE);
  net_node_t* node2 = net_node_create(&id2, 0x0A000002, 8081);

  EXPECT_EQ(query_add_target(query, node1), 0);
  EXPECT_EQ(query->num_targets, 1u);
  EXPECT_EQ(query_add_target(query, node2), 0);
  EXPECT_EQ(query->num_targets, 2u);

  net_node_destroy(node1);
  net_node_destroy(node2);
}

TEST_F(QueryTest, SetLatencyAndGetClosest) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  net_node_t* node1 = net_node_create(&id1, 0x0A000001, 8080);

  node_id_t id2 = {};
  memset(id2.hash, 0xBB, NODE_ID_HASH_SIZE);
  net_node_t* node2 = net_node_create(&id2, 0x0A000002, 8081);

  query_add_target(query, node1);
  query_add_target(query, node2);

  query_set_latency(query, 0, 15000);  // 15ms
  query_set_latency(query, 1, 5000);   // 5ms

  net_node_t* closest = query_get_closest(query);
  ASSERT_NE(closest, nullptr);
  EXPECT_EQ(closest, node2);  // node2 has lower latency

  net_node_destroy(node1);
  net_node_destroy(node2);
}

TEST_F(QueryTest, Expiration) {
  EXPECT_FALSE(query_is_expired(query, 1000));  // Start time is 0, 5000ms timeout
  EXPECT_TRUE(query_is_expired(query, 6000));    // Past timeout
  EXPECT_TRUE(query_is_expired(nullptr, 0));      // NULL is expired
}

TEST_F(QueryTest, FinishAndFail) {
  EXPECT_EQ(query_finish(query), 0);
  EXPECT_EQ(query->status, QUERY_STATUS_FINISHED);
  EXPECT_TRUE(query_is_finished(query));

  query_t* query2 = query_create(2, QUERY_TYPE_CLOSEST, 1000);
  EXPECT_EQ(query_fail(query2), 0);
  EXPECT_EQ(query2->status, QUERY_STATUS_FAILED);
  EXPECT_TRUE(query_is_finished(query2));
  query_destroy(query2);
}

// === Query table tests ===

class QueryTableTest : public ::testing::Test {
protected:
  query_table_t table;
  void SetUp() override {
    query_table_init(&table, 4);
  }
  void TearDown() override {
    query_table_deinit(&table);
  }
};

TEST_F(QueryTableTest, InitDeinit) {
  EXPECT_EQ(table.count, 0u);
  EXPECT_EQ(table.capacity, 4u);
}

TEST_F(QueryTableTest, InsertAndLookup) {
  query_t* query = query_create(42, QUERY_TYPE_GOSSIP, 5000);
  ASSERT_NE(query, nullptr);
  query->start_time_ms = 1000;

  EXPECT_EQ(query_table_insert(&table, query), 0);
  EXPECT_EQ(table.count, 1u);

  query_t* found = query_table_lookup(&table, 42);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->query_id, 42u);

  // Lookup non-existent
  EXPECT_EQ(query_table_lookup(&table, 99), nullptr);
}

TEST_F(QueryTableTest, Remove) {
  query_t* query = query_create(10, QUERY_TYPE_GOSSIP, 5000);
  query_table_insert(&table, query);

  EXPECT_EQ(query_table_remove(&table, 10), 0);
  EXPECT_EQ(table.count, 0u);
  EXPECT_EQ(query_table_lookup(&table, 10), nullptr);

  // Remove non-existent
  EXPECT_EQ(query_table_remove(&table, 99), -1);
}

TEST_F(QueryTableTest, ExpireOld) {
  query_t* query1 = query_create(1, QUERY_TYPE_GOSSIP, 1000);
  query1->start_time_ms = 100;

  query_t* query2 = query_create(2, QUERY_TYPE_CLOSEST, 5000);
  query2->start_time_ms = 100;

  query_table_insert(&table, query1);
  query_table_insert(&table, query2);

  // At time 500, query1 (start=100, timeout=1000) is expired (100+1000=1100 > 500? No)
  // Actually 500 < 1100 so not expired yet. At time 2000, query1 is expired (100+1000=1100 < 2000)
  size_t expired = query_table_expire(&table, 2000);
  EXPECT_EQ(expired, 1u);
  EXPECT_EQ(table.count, 1u);

  query_t* remaining = query_table_lookup(&table, 2);
  ASSERT_NE(remaining, nullptr);
  EXPECT_EQ(remaining->query_id, 2u);
}

TEST_F(QueryTableTest, GrowOnCapacity) {
  query_table_t small_table;
  query_table_init(&small_table, 2);

  for (int index = 0; index < 5; index++) {
    query_t* query = query_create((uint64_t)index, QUERY_TYPE_GOSSIP, 5000);
    query_table_insert(&small_table, query);
  }
  EXPECT_EQ(small_table.count, 5u);
  EXPECT_GE(small_table.capacity, 5u);

  query_table_deinit(&small_table);
}

// === Gossip scheduler tests ===

class GossipSchedulerTest : public ::testing::Test {
protected:
  gossip_scheduler_t sched;
  void SetUp() override {
    gossip_scheduler_init(&sched, 2, 3, 30);
  }
};

TEST_F(GossipSchedulerTest, InitialPhase) {
  EXPECT_TRUE(sched.is_initial_phase);
  EXPECT_EQ(sched.interval_idx, 0u);
  EXPECT_EQ(sched.init_interval_s, 2u);
  EXPECT_EQ(sched.steady_state_interval_s, 30u);
}

TEST_F(GossipSchedulerTest, TickTriggersGossip) {
  // First tick at time 0 should trigger gossip (last_gossip_ms was 0)
  gossip_scheduler_tick(&sched, 1000);
  EXPECT_TRUE(sched.should_gossip);
  EXPECT_EQ(sched.interval_idx, 1u);  // Advanced one init interval
}

TEST_F(GossipSchedulerTest, InitToSteadyTransition) {
  // Tick 3 times to exhaust init phase
  gossip_scheduler_tick(&sched, 1000);
  gossip_scheduler_tick(&sched, 5000);  // Past 2s init interval
  gossip_scheduler_tick(&sched, 10000);
  // After 3 init intervals, should transition
  EXPECT_FALSE(sched.is_initial_phase);
}

TEST_F(GossipSchedulerTest, NoGossipBeforeInterval) {
  gossip_scheduler_tick(&sched, 1000);
  EXPECT_TRUE(sched.should_gossip);

  // Next tick too soon (within 2s init interval)
  gossip_scheduler_tick(&sched, 1500);
  EXPECT_FALSE(sched.should_gossip);
}

// === Gossip handle tests ===

class GossipHandleTest : public ::testing::Test {
protected:
  gossip_handle_t handle;
  void SetUp() override {
    gossip_handle_init(&handle, 2, 3, 30, 5000);
  }
  void TearDown() override {
    gossip_handle_deinit(&handle);
  }
};

TEST_F(GossipHandleTest, InitDeinit) {
  EXPECT_EQ(handle.active.length, 0);
  EXPECT_EQ(handle.next_query_id, 1u);
  EXPECT_FALSE(handle.running);
}

TEST_F(GossipHandleTest, NextQueryId) {
  EXPECT_EQ(gossip_handle_next_query_id(&handle), 1u);
  EXPECT_EQ(gossip_handle_next_query_id(&handle), 2u);
  EXPECT_EQ(gossip_handle_next_query_id(&handle), 3u);
}

// === Ring tests ===

TEST(RingTest, CreateDestroy) {
  ring_t* ring = ring_create(0, 5000);
  ASSERT_NE(ring, nullptr);
  EXPECT_EQ(ring->latency_min_us, 0u);
  EXPECT_EQ(ring->latency_max_us, 5000u);
  EXPECT_FALSE(ring->frozen);
  ring_destroy(ring);
}

TEST(RingTest, InsertPrimaryAndSecondary) {
  ring_t* ring = ring_create(0, 5000);
  node_id_t id = {};
  memset(id.hash, 0x11, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  EXPECT_EQ(ring_insert_primary(ring, node, 8), 0);
  EXPECT_EQ(ring->primary.length, 1);

  // Fill up primary (8 slots)
  for (int index = 1; index < 8; index++) {
    node_id_t nid = {};
    memset(nid.hash, 0x11 + index, NODE_ID_HASH_SIZE);
    net_node_t* inode = net_node_create(&nid, 0x0A000001 + index, 8080);
    ring_insert_primary(ring, inode, 8);
    net_node_destroy(inode);
  }
  EXPECT_EQ(ring->primary.length, 8);

  // Overflow to secondary
  node_id_t overflow_id = {};
  memset(overflow_id.hash, 0xFF, NODE_ID_HASH_SIZE);
  net_node_t* overflow = net_node_create(&overflow_id, 0x0A0000FF, 8080);
  EXPECT_EQ(ring_insert_primary(ring, overflow, 8), -1);  // Primary full
  EXPECT_EQ(ring_insert_secondary(ring, overflow, 4), 0);  // Goes to secondary
  EXPECT_EQ(ring->secondary.length, 1);

  net_node_destroy(node);
  net_node_destroy(overflow);
  ring_destroy(ring);
}

TEST(RingTest, FindAndErase) {
  ring_t* ring = ring_create(0, 5000);
  node_id_t id = {};
  memset(id.hash, 0x22, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  ring_insert_primary(ring, node, 8);

  net_node_t* found = ring_find_by_id(ring, &id);
  ASSERT_NE(found, nullptr);

  EXPECT_TRUE(ring_erase(ring, &id));
  EXPECT_EQ(ring->primary.length, 0);
  EXPECT_EQ(ring_find_by_id(ring, &id), nullptr);

  net_node_destroy(node);
  ring_destroy(ring);
}

// === RingSet tests ===

TEST(RingSetTest, CreateDestroy) {
  ring_set_t* set = ring_set_create(8, 4, 2);
  ASSERT_NE(set, nullptr);
  EXPECT_EQ(set->ring_count, (size_t)RING_MAX_RINGS);
  ring_set_destroy(set);
}

TEST(RingSetTest, InsertAndFind) {
  ring_set_t* set = ring_set_create(8, 4, 2);
  node_id_t id = {};
  memset(id.hash, 0x33, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  // Insert at 3ms latency (ring 0: [0, 5000) us)
  EXPECT_EQ(ring_set_insert(set, node, 3000), 0);

  net_node_t* found = ring_set_find_by_id(set, &id);
  ASSERT_NE(found, nullptr);

  ring_set_clear_nodes(set);
  ring_set_destroy(set);
}

TEST(RingSetTest, FindClosest) {
  ring_set_t* set = ring_set_create(8, 4, 2);

  node_id_t id1 = {};
  memset(id1.hash, 0x11, NODE_ID_HASH_SIZE);
  net_node_t* node1 = net_node_create(&id1, 0x0A000001, 8080);
  ring_set_insert(set, node1, 3000);  // Ring 0: [0, 5000) us

  node_id_t id2 = {};
  memset(id2.hash, 0x22, NODE_ID_HASH_SIZE);
  net_node_t* node2 = net_node_create(&id2, 0x0A000002, 8080);
  ring_set_insert(set, node2, 50000);  // Higher latency ring

  net_node_t* closest = ring_set_find_closest(set);
  ASSERT_NE(closest, nullptr);
  // Closest should be node1 (in lower-latency ring)
  EXPECT_TRUE(net_node_equals_by_id(closest, node1));

  ring_set_clear_nodes(set);
  ring_set_destroy(set);
}

TEST(RingSetTest, Erase) {
  ring_set_t* set = ring_set_create(8, 4, 2);
  node_id_t id = {};
  memset(id.hash, 0x44, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  ring_set_insert(set, node, 3000);
  EXPECT_TRUE(ring_set_erase(set, &id));
  EXPECT_EQ(ring_set_find_by_id(set, &id), nullptr);
  EXPECT_EQ(ring_set_total_nodes(set), 0u);

  net_node_destroy(node);
  ring_set_destroy(set);
}

TEST(RingSetTest, EligibleForReplacement) {
  ring_set_t* set = ring_set_create(8, 4, 2);

  // Not eligible: ring is empty
  EXPECT_FALSE(ring_set_eligible_for_replacement(set, 0));

  // Fill primary ring (8 slots) — use 3000us latency which maps to ring 0
  for (int index = 0; index < 8; index++) {
    node_id_t id = {};
    memset(id.hash, 0x55 + index, NODE_ID_HASH_SIZE);
    net_node_t* node = net_node_create(&id, 0x0A000001 + index, 8080);
    ring_set_insert(set, node, 3000);
  }
  EXPECT_EQ(ring_set_total_nodes(set), 8u);

  // Not eligible: primary full but secondary empty
  EXPECT_FALSE(ring_set_eligible_for_replacement(set, 0));

  // Add to secondary (overflow)
  node_id_t sec_id = {};
  memset(sec_id.hash, 0xFF, NODE_ID_HASH_SIZE);
  net_node_t* sec_node = net_node_create(&sec_id, 0x0A0000FF, 8080);
  ring_set_insert(set, sec_node, 3000);
  EXPECT_GT(ring_set_total_nodes(set), 8u);

  // Now eligible: primary full, secondary non-empty, >primary_ring_size non-rendezvous nodes
  EXPECT_TRUE(ring_set_eligible_for_replacement(set, 0));

  ring_set_clear_nodes(set);
  ring_set_destroy(set);
}

TEST(RingSetTest, PromoteSecondary) {
  ring_set_t* set = ring_set_create(2, 2, 2);

  // Fill primary
  node_id_t id1 = {};
  memset(id1.hash, 0x11, NODE_ID_HASH_SIZE);
  net_node_t* node1 = net_node_create(&id1, 0x0A000001, 8080);
  ring_set_insert(set, node1, 3000);

  node_id_t id2 = {};
  memset(id2.hash, 0x22, NODE_ID_HASH_SIZE);
  net_node_t* node2 = net_node_create(&id2, 0x0A000002, 8080);
  ring_set_insert(set, node2, 3000);

  // Add secondary (overflow)
  node_id_t id3 = {};
  memset(id3.hash, 0x33, NODE_ID_HASH_SIZE);
  net_node_t* node3 = net_node_create(&id3, 0x0A000003, 8080);
  ring_set_insert(set, node3, 3000);

  // Promote from secondary
  net_node_t* promoted = ring_set_promote_secondary(set, 0);
  ASSERT_NE(promoted, nullptr);
  EXPECT_TRUE(net_node_equals_by_id(promoted, node3));

  ring_set_clear_nodes(set);
  ring_set_destroy(set);
}

// === Latency cache tests ===

TEST(LatencyCacheTest, CreateDestroy) {
  latency_cache_t* cache = latency_cache_create(16);
  ASSERT_NE(cache, nullptr);
  latency_cache_destroy(cache);
}

TEST(LatencyCacheTest, InsertAndGet) {
  latency_cache_t* cache = latency_cache_create(16);
  node_id_t id = {};
  memset(id.hash, 0xAA, NODE_ID_HASH_SIZE);

  latency_cache_insert(cache, &id, 0x0A000001, 8080, 15.5f);

  float latency_ms = 0;
  EXPECT_EQ(latency_cache_get(cache, &id, &latency_ms), 0);
  EXPECT_FLOAT_EQ(latency_ms, 15.5f);

  latency_cache_destroy(cache);
}

TEST(LatencyCacheTest, UpdateInPlace) {
  latency_cache_t* cache = latency_cache_create(16);
  node_id_t id = {};
  memset(id.hash, 0xBB, NODE_ID_HASH_SIZE);

  latency_cache_insert(cache, &id, 0x0A000001, 8080, 10.0f);
  latency_cache_insert(cache, &id, 0x0A000001, 8080, 20.0f);

  float latency_ms = 0;
  EXPECT_EQ(latency_cache_get(cache, &id, &latency_ms), 0);
  EXPECT_FLOAT_EQ(latency_ms, 20.0f);  // Updated, not duplicated

  latency_cache_destroy(cache);
}

TEST(LatencyCacheTest, Eviction) {
  latency_cache_t* cache = latency_cache_create(2);

  node_id_t id1 = {};
  memset(id1.hash, 0x11, NODE_ID_HASH_SIZE);
  node_id_t id2 = {};
  memset(id2.hash, 0x22, NODE_ID_HASH_SIZE);
  node_id_t id3 = {};
  memset(id3.hash, 0x33, NODE_ID_HASH_SIZE);

  latency_cache_insert(cache, &id1, 1, 8080, 10.0f);
  latency_cache_insert(cache, &id2, 2, 8080, 20.0f);
  EXPECT_EQ(cache->count, 2u);

  // Third insert should evict oldest (id1)
  latency_cache_insert(cache, &id3, 3, 8080, 30.0f);
  EXPECT_EQ(cache->count, 2u);

  // id1 should be gone
  float latency_ms = 0;
  EXPECT_EQ(latency_cache_get(cache, &id1, &latency_ms), -1);
  // id2 should still be there
  EXPECT_EQ(latency_cache_get(cache, &id2, &latency_ms), 0);
  EXPECT_FLOAT_EQ(latency_ms, 20.0f);

  latency_cache_destroy(cache);
}

// === NetNode tests ===

TEST(NetNodeTest, CreateDestroy) {
  node_id_t id = {};
  memset(id.hash, 0x55, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->addr, 0x0A000001);
  EXPECT_EQ(node->port, 8080);
  net_node_destroy(node);
}

TEST(NetNodeTest, UpdateLatency) {
  node_id_t id = {};
  memset(id.hash, 0x55, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  net_node_update_latency(node, 100.0f);
  EXPECT_FLOAT_EQ(node->latency_ms, 100.0f);

  net_node_update_latency(node, 200.0f);
  // EWMA with alpha=0.1: 0.1*200 + 0.9*100 = 110
  EXPECT_FLOAT_EQ(node->latency_ms, 110.0f);

  net_node_destroy(node);
}

TEST(NetNodeTest, RecordSuccessAndFail) {
  node_id_t id = {};
  memset(id.hash, 0x55, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&id, 0x0A000001, 8080);

  // Initial availability is 0.5 (unknown baseline)
  EXPECT_FLOAT_EQ(node->availability, 0.5f);

  net_node_record_success(node);
  // 0.1*1.0 + 0.9*0.5 = 0.55
  EXPECT_NEAR(node->availability, 0.55f, 0.001f);

  net_node_record_success(node);
  // 0.1*1.0 + 0.9*0.55 = 0.595
  EXPECT_NEAR(node->availability, 0.595f, 0.001f);

  net_node_record_fail(node);
  // 0.1*0.0 + 0.9*0.595 = 0.5355
  EXPECT_NEAR(node->availability, 0.5355f, 0.001f);

  net_node_destroy(node);
}

// === FindBlock visited bloom tests ===

TEST(FindBlockVisitedTest, AddAndCheckVisited) {
  uint8_t visited[FIND_BLOCK_MAX_VISITED_BLOOM] = {};
  uint16_t count = 0;
  uint8_t block_hash[32] = {};
  memset(block_hash, 0xAA, 32);

  EXPECT_FALSE(find_block_is_visited(visited, count, block_hash));

  find_block_add_visited(visited, &count, block_hash);
  EXPECT_EQ(count, 1u);
  EXPECT_TRUE(find_block_is_visited(visited, count, block_hash));
}

TEST(FindBlockVisitedTest, DifferentHashesNotVisited) {
  uint8_t visited[FIND_BLOCK_MAX_VISITED_BLOOM] = {};
  uint16_t count = 0;
  uint8_t hash_a[32] = {};
  memset(hash_a, 0xAA, 32);
  uint8_t hash_b[32] = {};
  memset(hash_b, 0xBB, 32);

  find_block_add_visited(visited, &count, hash_a);
  EXPECT_TRUE(find_block_is_visited(visited, count, hash_a));
  EXPECT_FALSE(find_block_is_visited(visited, count, hash_b));
}

TEST(FindBlockVisitedTest, NullInputs) {
  uint8_t visited[FIND_BLOCK_MAX_VISITED_BLOOM] = {};
  uint16_t count = 0;
  uint8_t block_hash[32] = {};
  memset(block_hash, 0xCC, 32);

  find_block_add_visited(NULL, &count, block_hash);
  find_block_add_visited(visited, NULL, block_hash);
  find_block_add_visited(visited, &count, NULL);
  EXPECT_EQ(count, 0u);

  EXPECT_FALSE(find_block_is_visited(NULL, count, block_hash));
  EXPECT_FALSE(find_block_is_visited(visited, count, NULL));
  EXPECT_FALSE(find_block_is_visited(visited, 0, block_hash));
}

TEST(FindBlockVisitedTest, MultipleHashes) {
  uint8_t visited[FIND_BLOCK_MAX_VISITED_BLOOM] = {};
  uint16_t count = 0;

  for (int index = 0; index < 5; index++) {
    uint8_t hash[32] = {};
    memset(hash, (uint8_t)(0x10 + index), 32);
    find_block_add_visited(visited, &count, hash);
  }

  EXPECT_EQ(count, 5u);

  // All added hashes should be found
  for (int index = 0; index < 5; index++) {
    uint8_t hash[32] = {};
    memset(hash, (uint8_t)(0x10 + index), 32);
    EXPECT_TRUE(find_block_is_visited(visited, count, hash));
  }
}

// === FindBlock execute tests ===

class FindBlockTest : public ::testing::Test {
protected:
  eabf_table_t eabf_table;
  eabf_ttl_table_t eabf_ttl;
  ring_set_t* rings;
  node_id_t local_id;

  void SetUp() override {
    eabf_table_init(&eabf_table, 4);
    eabf_ttl_table_init(&eabf_ttl, 16);
    rings = ring_set_create(RING_K, RING_M, RING_ALPHA);
    memset(local_id.hash, 0x01, NODE_ID_HASH_SIZE);
  }

  void TearDown() override {
    ring_set_destroy(rings);
    eabf_table_deinit(&eabf_table);
    eabf_ttl_table_deinit(&eabf_ttl);
  }
};

TEST_F(FindBlockTest, TtlExpiredReturnsTtlExpired) {
  find_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.ttl = 0;  // TTL expired

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, &state,
      next_hops, &next_hop_count);

  EXPECT_EQ(result, FIND_BLOCK_TTL_EXPIRED);
  EXPECT_EQ(next_hop_count, 0u);
}

TEST_F(FindBlockTest, NullStateReturnsNotFound) {
  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, NULL,
      next_hops, &next_hop_count);

  EXPECT_EQ(result, FIND_BLOCK_NOT_FOUND);
}

TEST_F(FindBlockTest, NoPeersReturnsNotFound) {
  find_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.ttl = 6;

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, &state,
      next_hops, &next_hop_count);

  // No ring members, no EABF entries — should return NOT_FOUND
  EXPECT_EQ(result, FIND_BLOCK_NOT_FOUND);
}

TEST_F(FindBlockTest, WithRingMembersForwards) {
  // Create ring members with weights above minimum
  node_id_t id_a = {};
  memset(id_a.hash, 0x10, NODE_ID_HASH_SIZE);
  net_node_t* node_a = net_node_create(&id_a, 0x0A000001, 8080);
  node_a->weight = 0.5f;

  node_id_t id_b = {};
  memset(id_b.hash, 0x20, NODE_ID_HASH_SIZE);
  net_node_t* node_b = net_node_create(&id_b, 0x0A000002, 8081);
  node_b->weight = 0.8f;

  node_id_t id_c = {};
  memset(id_c.hash, 0x30, NODE_ID_HASH_SIZE);
  net_node_t* node_c = net_node_create(&id_c, 0x0A000003, 8082);
  node_c->weight = 0.6f;

  // Insert nodes into ring (latency in microseconds)
  ring_set_insert(rings, node_a, 5000);   // 5ms
  ring_set_insert(rings, node_b, 10000);  // 10ms
  ring_set_insert(rings, node_c, 20000);  // 20ms

  find_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.ttl = 6;

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, &state,
      next_hops, &next_hop_count);

  EXPECT_EQ(result, FIND_BLOCK_FORWARDING);
  EXPECT_GT(next_hop_count, 0u);
  EXPECT_LE(next_hop_count, (size_t)FIND_BLOCK_FORWARD_FANOUT);

  net_node_destroy(node_a);
  net_node_destroy(node_b);
  net_node_destroy(node_c);
}

TEST_F(FindBlockTest, EabfGravityWellOverrides) {
  // Create a ring member
  node_id_t id_a = {};
  memset(id_a.hash, 0x10, NODE_ID_HASH_SIZE);
  net_node_t* node_a = net_node_create(&id_a, 0x0A000001, 8080);
  node_a->weight = 0.5f;

  node_id_t id_b = {};
  memset(id_b.hash, 0x20, NODE_ID_HASH_SIZE);
  net_node_t* node_b = net_node_create(&id_b, 0x0A000002, 8081);
  node_b->weight = 0.3f;  // Low weight

  ring_set_insert(rings, node_a, 5000);
  ring_set_insert(rings, node_b, 10000);

  // Subscribe block hash in node_b's EABF (gravity well)
  eabf_t* eabf_b = eabf_table_insert(&eabf_table, &id_b);
  ASSERT_NE(eabf_b, nullptr);

  uint8_t block_hash[32] = {};
  memset(block_hash, 0xAA, 32);
  eabf_subscribe(eabf_b, block_hash, 32);

  find_block_state_t state = {};
  memcpy(state.block_hash, block_hash, 32);
  state.ttl = 6;

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, &state,
      next_hops, &next_hop_count);

  EXPECT_EQ(result, FIND_BLOCK_FORWARDING);
  // Should route to node_b (gravity well) despite its lower weight
  ASSERT_EQ(next_hop_count, 1u);
  EXPECT_TRUE(node_id_equals(&next_hops[0]->id, &id_b));

  net_node_destroy(node_a);
  net_node_destroy(node_b);
}

TEST_F(FindBlockTest, PathCycleDetection) {
  // Create a ring member that's already in the path
  node_id_t id_a = {};
  memset(id_a.hash, 0x10, NODE_ID_HASH_SIZE);
  net_node_t* node_a = net_node_create(&id_a, 0x0A000001, 8080);
  node_a->weight = 0.9f;

  ring_set_insert(rings, node_a, 5000);

  // EABF gravity well for node_a
  eabf_t* eabf_a = eabf_table_insert(&eabf_table, &id_a);
  ASSERT_NE(eabf_a, nullptr);
  uint8_t block_hash[32] = {};
  memset(block_hash, 0xAA, 32);
  eabf_subscribe(eabf_a, block_hash, 32);

  find_block_state_t state = {};
  memcpy(state.block_hash, block_hash, 32);
  state.ttl = 6;
  // Put node_a in the path — should be skipped
  memcpy(&state.path[0], &id_a, sizeof(node_id_t));
  state.path_len = 1;

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &eabf_table, &eabf_ttl, NULL, rings, &local_id, &state,
      next_hops, &next_hop_count);

  // node_a is in the path so gravity well and ring candidate should be skipped
  EXPECT_EQ(result, FIND_BLOCK_NOT_FOUND);

  net_node_destroy(node_a);
}

// === StoreBlock tests ===

class StoreBlockTest : public ::testing::Test {
protected:
  eabf_table_t eabf_table;
  ring_set_t* rings;
  node_id_t local_id;

  void SetUp() override {
    eabf_table_init(&eabf_table, 4);
    rings = ring_set_create(RING_K, RING_M, RING_ALPHA);
    memset(local_id.hash, 0x01, NODE_ID_HASH_SIZE);
  }

  void TearDown() override {
    ring_set_destroy(rings);
    eabf_table_deinit(&eabf_table);
  }
};

TEST(StoreBlockShouldAcceptTest, LowCapacityAlwaysAccepts) {
  // At capacity=0, accept probability = 1.0 - 0/0.80 = 1.0
  uint8_t hash[32] = {};
  memset(hash, 0xAA, 32);
  EXPECT_TRUE(store_block_should_accept(0.0f, NODE_PHASE_INHALE, hash, 1024));
}

TEST(StoreBlockShouldAcceptTest, ExhalePhaseDeclines) {
  uint8_t hash[32] = {};
  memset(hash, 0xAA, 32);
  EXPECT_FALSE(store_block_should_accept(0.3f, NODE_PHASE_EXHALE, hash, 1024));
}

TEST(StoreBlockShouldAcceptTest, HighCapacityDeclines) {
  uint8_t hash[32] = {};
  memset(hash, 0xAA, 32);
  // At capacity >= 0.80, probability = 0 → always decline
  EXPECT_FALSE(store_block_should_accept(0.85f, NODE_PHASE_NEUTRAL, hash, 1024));
  EXPECT_FALSE(store_block_should_accept(1.0f, NODE_PHASE_NEUTRAL, hash, 1024));
}

TEST_F(StoreBlockTest, NullStateReturnsDeclined) {
  net_node_t* next_hops[STORE_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  store_block_result_e result = store_block_execute(
      &eabf_table, NULL, rings, &local_id, 0.3f, NODE_PHASE_INHALE,
      NULL, next_hops, &next_hop_count);

  EXPECT_EQ(result, STORE_BLOCK_DECLINED);
}

TEST_F(StoreBlockTest, MaxHopsZeroReturnsMaxHopsReached) {
  store_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.max_hops = 0;  // No more hops allowed

  net_node_t* next_hops[STORE_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  store_block_result_e result = store_block_execute(
      &eabf_table, NULL, rings, &local_id, 0.9f, NODE_PHASE_NEUTRAL,
      &state, next_hops, &next_hop_count);

  EXPECT_EQ(result, STORE_BLOCK_MAX_HOPS_REACHED);
}

TEST_F(StoreBlockTest, LowCapacityAccepts) {
  srand(2);  // Deterministic seed: rand() produces 0.70 < 0.75 accept probability
  store_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.max_hops = 6;

  net_node_t* next_hops[STORE_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  // Low capacity (0.2) → should accept
  store_block_result_e result = store_block_execute(
      &eabf_table, NULL, rings, &local_id, 0.2f, NODE_PHASE_INHALE,
      &state, next_hops, &next_hop_count);

  EXPECT_EQ(result, STORE_BLOCK_ACCEPTED);
}

TEST_F(StoreBlockTest, HighCapacityForwards) {
  // Create ring members with good availability and weight
  node_id_t id_a = {};
  memset(id_a.hash, 0x10, NODE_ID_HASH_SIZE);
  net_node_t* node_a = net_node_create(&id_a, 0x0A000001, 8080);
  node_a->weight = 0.5f;
  node_a->capacity = 0.3f;  // Has room
  node_a->availability = 0.8f;
  node_a->latency_ms = 5.0f;
  net_node_record_success(node_a);

  node_id_t id_b = {};
  memset(id_b.hash, 0x20, NODE_ID_HASH_SIZE);
  net_node_t* node_b = net_node_create(&id_b, 0x0A000002, 8081);
  node_b->weight = 0.8f;
  node_b->capacity = 0.2f;  // Has room
  node_b->availability = 0.9f;
  node_b->latency_ms = 10.0f;
  net_node_record_success(node_b);

  ring_set_insert(rings, node_a, 5000);
  ring_set_insert(rings, node_b, 10000);

  store_block_state_t state = {};
  memset(state.block_hash, 0xAA, 32);
  state.max_hops = 6;

  net_node_t* next_hops[STORE_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  // High capacity (0.9) → decline locally, forward to peers with room
  store_block_result_e result = store_block_execute(
      &eabf_table, NULL, rings, &local_id, 0.9f, NODE_PHASE_NEUTRAL,
      &state, next_hops, &next_hop_count);

  EXPECT_EQ(result, STORE_BLOCK_FORWARDING);
  EXPECT_GT(next_hop_count, 0u);
  EXPECT_LE(next_hop_count, (size_t)STORE_BLOCK_FORWARD_FANOUT);

  net_node_destroy(node_a);
  net_node_destroy(node_b);
}

// === Hebbian weight table tests ===

class HebbianTest : public ::testing::Test {
protected:
  hebbian_table_t table;

  void SetUp() override {
    hebbian_table_init(&table, 4);
  }

  void TearDown() override {
    hebbian_table_deinit(&table);
  }
};

TEST_F(HebbianTest, InitDeinit) {
  EXPECT_NE(table.entries, nullptr);
  EXPECT_EQ(table.count, 0u);
  EXPECT_GE(table.capacity, 4u);
}

TEST_F(HebbianTest, GetNonexistentReturnsMin) {
  node_id_t id = {};
  memset(id.hash, 0xAA, NODE_ID_HASH_SIZE);
  float weight = hebbian_table_get(&table, &id);
  EXPECT_FLOAT_EQ(weight, HEBBIAN_MIN_WEIGHT);
}

TEST_F(HebbianTest, SetAndGet) {
  node_id_t id = {};
  memset(id.hash, 0xBB, NODE_ID_HASH_SIZE);
  hebbian_table_set(&table, &id, 0.75f);
  EXPECT_FLOAT_EQ(hebbian_table_get(&table, &id), 0.75f);
}

TEST_F(HebbianTest, FrequencyRule) {
  node_id_t holder = {};
  memset(holder.hash, 0xCC, NODE_ID_HASH_SIZE);
  hebbian_table_set(&table, &holder, 0.5f);

  hebbian_frequency(&table, &holder, 0.1f);
  float new_weight = hebbian_table_get(&table, &holder);
  EXPECT_NEAR(new_weight, 0.6f, 0.001f);  // 0.5 + 0.1
}

TEST_F(HebbianTest, FrequencyCreatesNewEntry) {
  node_id_t new_peer = {};
  memset(new_peer.hash, 0xDD, NODE_ID_HASH_SIZE);

  hebbian_frequency(&table, &new_peer, 0.15f);
  float weight = hebbian_table_get(&table, &new_peer);
  // New entry should be max(delta, INITIAL_WEIGHT) = max(0.15, 0.1) = 0.15
  EXPECT_NEAR(weight, 0.15f, 0.001f);
}

TEST_F(HebbianTest, ComputeDeltaLowLatency) {
  float delta = hebbian_compute_delta(100, HEBBIAN_FIND_BLOCK_MULTIPLIER);
  EXPECT_GT(delta, 0.0f);
  EXPECT_LT(delta, HEBBIAN_GAMMA_0);
}

TEST_F(HebbianTest, ComputeDeltaHighLatency) {
  float delta = hebbian_compute_delta(HEBBIAN_MAX_SEARCH_TIME_MS + 1000,
                                       HEBBIAN_FIND_BLOCK_MULTIPLIER);
  EXPECT_NEAR(delta, 0.0f, 0.001f);
}

TEST_F(HebbianTest, FeedbackRule) {
  node_id_t path[3];
  memset(path[0].hash, 0x10, NODE_ID_HASH_SIZE);
  memset(path[1].hash, 0x20, NODE_ID_HASH_SIZE);
  memset(path[2].hash, 0x30, NODE_ID_HASH_SIZE);

  hebbian_table_set(&table, &path[0], 0.5f);
  hebbian_table_set(&table, &path[1], 0.3f);
  hebbian_table_set(&table, &path[2], 0.2f);

  hebbian_feedback(&table, path, 3, 0.1f);

  // Feedback: w_{path[1]→path[2]} += eta_f * delta_w = 0.25 * 0.1 = 0.025
  float w_b = hebbian_table_get(&table, &path[2]);
  EXPECT_NEAR(w_b, 0.2f + 0.025f, 0.001f);
}

TEST_F(HebbianTest, SymmetryRule) {
  node_id_t path[2];
  memset(path[0].hash, 0x10, NODE_ID_HASH_SIZE);
  memset(path[1].hash, 0x20, NODE_ID_HASH_SIZE);

  hebbian_table_set(&table, &path[0], 0.5f);
  hebbian_table_set(&table, &path[1], 0.3f);

  hebbian_symmetry(&table, path, 2, 0.1f);

  // Symmetry: w_{path[1]→path[0]} += eta_s * delta_w = 0.05 * 0.1 = 0.005
  float w_reverse = hebbian_table_get(&table, &path[0]);
  EXPECT_NEAR(w_reverse, 0.5f + 0.005f, 0.001f);
}

TEST_F(HebbianTest, ApplySuccessFull) {
  node_id_t path[3];
  memset(path[0].hash, 0x10, NODE_ID_HASH_SIZE);
  memset(path[1].hash, 0x20, NODE_ID_HASH_SIZE);
  memset(path[2].hash, 0x30, NODE_ID_HASH_SIZE);

  hebbian_apply_success(&table, path, 3, 100, HEBBIAN_FIND_BLOCK_MULTIPLIER);
  EXPECT_GT(table.count, 0u);
}

TEST_F(HebbianTest, DecayReducesWeights) {
  node_id_t id = {};
  memset(id.hash, 0xEE, NODE_ID_HASH_SIZE);
  hebbian_table_set(&table, &id, 0.5f);

  hebbian_decay(&table);
  float weight = hebbian_table_get(&table, &id);
  EXPECT_NEAR(weight, 0.5f * HEBBIAN_DECAY_FACTOR, 0.001f);
}

TEST_F(HebbianTest, DecayClampsToMin) {
  node_id_t id = {};
  memset(id.hash, 0xFF, NODE_ID_HASH_SIZE);
  hebbian_table_set(&table, &id, HEBBIAN_MIN_WEIGHT);

  hebbian_decay(&table);
  float weight = hebbian_table_get(&table, &id);
  EXPECT_FLOAT_EQ(weight, HEBBIAN_MIN_WEIGHT);
}

TEST_F(HebbianTest, Remove) {
  node_id_t id = {};
  memset(id.hash, 0xAA, NODE_ID_HASH_SIZE);
  hebbian_table_set(&table, &id, 0.5f);
  EXPECT_EQ(table.count, 1u);

  hebbian_table_remove(&table, &id);
  EXPECT_EQ(table.count, 0u);
  EXPECT_FLOAT_EQ(hebbian_table_get(&table, &id), HEBBIAN_MIN_WEIGHT);
}

// === Respiration tests ===

TEST(RespirationTest, SeekIntervalAtZeroCapacity) {
  uint64_t interval = respiration_seek_interval(0.0f);
  EXPECT_EQ(interval, RESPIRATION_TAU_MIN_MS);
}

TEST(RespirationTest, SeekIntervalAtHighCapacity) {
  uint64_t interval = respiration_seek_interval(0.50f);
  EXPECT_EQ(interval, UINT64_MAX);

  interval = respiration_seek_interval(0.80f);
  EXPECT_EQ(interval, UINT64_MAX);
}

TEST(RespirationTest, SeekIntervalAtMidCapacity) {
  uint64_t interval = respiration_seek_interval(0.25f);
  EXPECT_GT(interval, RESPIRATION_TAU_MIN_MS);
  EXPECT_LT(interval, RESPIRATION_TAU_MAX_MS);
}

TEST(RespirationTest, ShouldInhale) {
  EXPECT_TRUE(respiration_should_inhale(0.3f));
  EXPECT_TRUE(respiration_should_inhale(0.49f));
  EXPECT_FALSE(respiration_should_inhale(0.50f));
  EXPECT_FALSE(respiration_should_inhale(0.80f));
}

TEST(RespirationTest, ShouldExhale) {
  EXPECT_FALSE(respiration_should_exhale(0.3f));
  EXPECT_FALSE(respiration_should_exhale(0.70f));
  EXPECT_TRUE(respiration_should_exhale(0.80f));
  EXPECT_TRUE(respiration_should_exhale(0.95f));
}

TEST(RespirationTest, BlocksToFree) {
  uint32_t to_free = respiration_blocks_to_free(0.80f, 100, 4096);
  EXPECT_EQ(to_free, 50u);  // 100 - 100*0.50 = 50
}

TEST(RespirationTest, BlocksToFreeBelowThreshold) {
  uint32_t to_free = respiration_blocks_to_free(0.70f, 100, 4096);
  EXPECT_EQ(to_free, 0u);
}

// === Rate limit tests ===

class RateLimitTest : public ::testing::Test {
protected:
  rate_limit_table_t table;

  void SetUp() override {
    rate_limit_table_init(&table, 4);
  }

  void TearDown() override {
    rate_limit_table_deinit(&table);
  }
};

TEST_F(RateLimitTest, InitDeinit) {
  EXPECT_NE(table.entries, nullptr);
  EXPECT_EQ(table.count, 0u);
  EXPECT_GE(table.capacity, 4u);
}

TEST_F(RateLimitTest, CheckAllowsFirstRequest) {
  node_id_t peer = {};
  memset(peer.hash, 0xAA, NODE_ID_HASH_SIZE);

  // First request should be allowed (buckets start full)
  bool allowed = rate_limit_check(&table, &peer, RPC_TYPE_FIND_BLOCK, 1000);
  EXPECT_TRUE(allowed);
}

TEST_F(RateLimitTest, CheckRejectsWhenEmpty) {
  node_id_t peer = {};
  memset(peer.hash, 0xBB, NODE_ID_HASH_SIZE);

  // Drain the bucket
  peer_rate_limits_t* limits = rate_limit_table_get(&table, &peer);
  ASSERT_NE(limits, nullptr);
  limits->buckets[RPC_TYPE_STORE_BLOCK].tokens = 0.0f;

  bool allowed = rate_limit_check(&table, &peer, RPC_TYPE_STORE_BLOCK, 1000);
  EXPECT_FALSE(allowed);
}

TEST_F(RateLimitTest, RefillOverTime) {
  node_id_t peer = {};
  memset(peer.hash, 0xCC, NODE_ID_HASH_SIZE);

  // Drain bucket
  peer_rate_limits_t* limits = rate_limit_table_get(&table, &peer);
  ASSERT_NE(limits, nullptr);
  limits->buckets[RPC_TYPE_FIND_BLOCK].tokens = 0.0f;
  limits->buckets[RPC_TYPE_FIND_BLOCK].last_refill = 1000;

  // After 1 second, should have refilled tokens
  bool allowed = rate_limit_check(&table, &peer, RPC_TYPE_FIND_BLOCK, 2000);
  EXPECT_TRUE(allowed);
}

TEST_F(RateLimitTest, DefaultConfigs) {
  // Verify default configs match the spec
  EXPECT_FLOAT_EQ(RATE_LIMIT_DEFAULTS[RPC_TYPE_FIND_BLOCK].base_rate, 5.0f);
  EXPECT_FLOAT_EQ(RATE_LIMIT_DEFAULTS[RPC_TYPE_STORE_BLOCK].base_rate, 0.5f);
  EXPECT_FLOAT_EQ(RATE_LIMIT_DEFAULTS[RPC_TYPE_SEEKING_BLOCKS].base_rate, 1.0f);
  EXPECT_FLOAT_EQ(RATE_LIMIT_DEFAULTS[RPC_TYPE_PING_CAPACITY].base_rate, 10.0f);
  EXPECT_FLOAT_EQ(RATE_LIMIT_DEFAULTS[RPC_TYPE_PING].base_rate, 10.0f);
}

TEST_F(RateLimitTest, CapacityMultiplier) {
  // StoreBlock at 0.80 → 0.05
  EXPECT_NEAR(rate_limit_capacity_multiplier(0.80f, RPC_TYPE_STORE_BLOCK), 0.05f, 0.001f);

  // StoreBlock at 0.50 → 1.0
  EXPECT_NEAR(rate_limit_capacity_multiplier(0.50f, RPC_TYPE_STORE_BLOCK), 1.0f, 0.001f);

  // StoreBlock at 0.65 → ~0.525 (linear taper)
  float mid = rate_limit_capacity_multiplier(0.65f, RPC_TYPE_STORE_BLOCK);
  EXPECT_GT(mid, 0.05f);
  EXPECT_LT(mid, 1.0f);

  // FindBlock always 1.0
  EXPECT_FLOAT_EQ(rate_limit_capacity_multiplier(0.80f, RPC_TYPE_FIND_BLOCK), 1.0f);
}

TEST_F(RateLimitTest, RetryAfter) {
  node_id_t peer = {};
  memset(peer.hash, 0xDD, NODE_ID_HASH_SIZE);

  // Drain bucket
  peer_rate_limits_t* limits = rate_limit_table_get(&table, &peer);
  ASSERT_NE(limits, nullptr);
  limits->buckets[RPC_TYPE_FIND_BLOCK].tokens = 0.0f;
  limits->buckets[RPC_TYPE_FIND_BLOCK].last_refill = 1000;

  // retry_after should be non-zero when bucket is empty
  uint32_t retry = rate_limit_retry_after(&table, &peer, RPC_TYPE_FIND_BLOCK, 1000);
  EXPECT_GT(retry, 0u);
}

TEST_F(RateLimitTest, RemovePeer) {
  node_id_t peer = {};
  memset(peer.hash, 0xEE, NODE_ID_HASH_SIZE);
  rate_limit_table_get(&table, &peer);
  EXPECT_EQ(table.count, 1u);

  rate_limit_table_remove(&table, &peer);
  EXPECT_EQ(table.count, 0u);
}

// === Authority persistence tests ===

class AuthorityPeerStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    config = config_default();
    authority = authority_create(&config);
    ASSERT_NE(authority, nullptr);

    // Set a temp file path for peer store
    authority->peer_store_path = strdup("/tmp/test_peer_store.cbor");

    // Construct a minimal network_t for testing persistence
    network = (network_t*)calloc(1, sizeof(network_t));
    ASSERT_NE(network, nullptr);
    network->authority = authority;
    network->rings = ring_set_create(0, 0, 0);
    ASSERT_NE(network->rings, nullptr);
    hebbian_table_init(&network->hebbian, 4);
    rate_limit_table_init(&network->rate_limits, 4);

    // Set a known local_id
    memset(authority->local_id.hash, 0xAB, NODE_ID_HASH_SIZE);
  }

  void TearDown() override {
    // Save path before destroying authority (which frees it)
    char* path = (authority && authority->peer_store_path) ? strdup(authority->peer_store_path) : nullptr;
    if (network != nullptr) {
      ring_set_clear_nodes(network->rings);
      ring_set_destroy(network->rings);
      hebbian_table_deinit(&network->hebbian);
      rate_limit_table_deinit(&network->rate_limits);
      free(network);
    }
    authority_destroy(authority);
    if (path != nullptr) {
      unlink(path);
      free(path);
    }
  }

  config_t config;
  authority_t* authority;
  network_t* network;
};

TEST_F(AuthorityPeerStoreTest, SaveAndLoadEmptyPeers) {
  // Save with no peers/hebbian entries
  int result = authority_save_peers(authority, network);
  EXPECT_EQ(result, 0);

  // Clear hebbian table and reload
  hebbian_table_deinit(&network->hebbian);
  hebbian_table_init(&network->hebbian, 4);
  EXPECT_EQ(network->hebbian.count, 0u);

  result = authority_load_peers(authority, network);
  EXPECT_EQ(result, 0);
}

TEST_F(AuthorityPeerStoreTest, SaveAndLoadHebbianWeights) {
  // Add some hebbian weights
  node_id_t peer1 = {};
  memset(peer1.hash, 0x11, NODE_ID_HASH_SIZE);
  hebbian_table_set(&network->hebbian, &peer1, 0.75f);

  node_id_t peer2 = {};
  memset(peer2.hash, 0x22, NODE_ID_HASH_SIZE);
  hebbian_table_set(&network->hebbian, &peer2, 0.42f);

  EXPECT_EQ(network->hebbian.count, 2u);

  // Save
  int result = authority_save_peers(authority, network);
  EXPECT_EQ(result, 0);

  // Reset hebbian table
  hebbian_table_deinit(&network->hebbian);
  hebbian_table_init(&network->hebbian, 4);
  EXPECT_EQ(network->hebbian.count, 0u);

  // Load
  result = authority_load_peers(authority, network);
  EXPECT_EQ(result, 0);

  // Verify weights were restored
  EXPECT_EQ(network->hebbian.count, 2u);
  float weight1 = hebbian_table_get(&network->hebbian, &peer1);
  float weight2 = hebbian_table_get(&network->hebbian, &peer2);
  EXPECT_NEAR(weight1, 0.75f, 0.001f);
  EXPECT_NEAR(weight2, 0.42f, 0.001f);
}

TEST_F(AuthorityPeerStoreTest, SaveAndLoadPeersWithMetadata) {
  // Add nodes to ring set
  node_id_t peer_id = {};
  memset(peer_id.hash, 0x33, NODE_ID_HASH_SIZE);
  net_node_t* node = net_node_create(&peer_id, htonl(INADDR_LOOPBACK), 9000);
  ASSERT_NE(node, nullptr);
  node->latency_ms = 15.5f;
  node->weight = 0.88f;
  node->capacity = 0.45f;
  node->phase = NODE_PHASE_INHALE;
  node->availability = 0.99f;

  ring_set_insert(network->rings, node, (uint32_t)(node->latency_ms * 1000));

  // Save
  int result = authority_save_peers(authority, network);
  EXPECT_EQ(result, 0);

  // Record original count
  size_t original_ring_count = ring_set_total_nodes(network->rings);

  // Destroy and recreate network for a clean load
  ring_set_clear_nodes(network->rings);
  ring_set_destroy(network->rings);
  hebbian_table_deinit(&network->hebbian);
  rate_limit_table_deinit(&network->rate_limits);
  network->rings = ring_set_create(0, 0, 0);
  ASSERT_NE(network->rings, nullptr);
  hebbian_table_init(&network->hebbian, 4);
  rate_limit_table_init(&network->rate_limits, 4);

  // Load
  result = authority_load_peers(authority, network);
  EXPECT_EQ(result, 0);

  // Verify local_id was restored
  for (int i = 0; i < NODE_ID_HASH_SIZE; i++) {
    EXPECT_EQ(authority->local_id.hash[i], 0xAB);
  }

  // Verify ring nodes were restored
  EXPECT_EQ(ring_set_total_nodes(network->rings), original_ring_count);

  // Verify the peer's metadata
  net_node_t* loaded = ring_set_find_by_id(network->rings, &peer_id);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->addr, htonl(INADDR_LOOPBACK));
  EXPECT_EQ(loaded->port, 9000u);
  EXPECT_NEAR(loaded->latency_ms, 15.5f, 0.01f);
  EXPECT_NEAR(loaded->weight, 0.88f, 0.01f);
  EXPECT_NEAR(loaded->capacity, 0.45f, 0.01f);
  EXPECT_EQ(loaded->phase, NODE_PHASE_INHALE);
  EXPECT_NEAR(loaded->availability, 0.99f, 0.01f);
}

TEST_F(AuthorityPeerStoreTest, LoadFromNonexistentFile) {
  char* original_path = authority->peer_store_path;
  authority->peer_store_path = strdup("/tmp/nonexistent_peer_store_test.cbor");
  int result = authority_load_peers(authority, network);
  EXPECT_EQ(result, -1);
  free(authority->peer_store_path);
  authority->peer_store_path = original_path;
}

TEST_F(AuthorityPeerStoreTest, SaveWithNullPathReturnsError) {
  char* original_path = authority->peer_store_path;
  authority->peer_store_path = nullptr;
  EXPECT_EQ(authority_save_peers(authority, network), -1);
  authority->peer_store_path = original_path;
}

// === TimingWheel tests ===

class TimingWheelTest : public ::testing::Test {
protected:
  timing_wheel_t wheel;
  void SetUp() override {
    timing_wheel_init(&wheel, 64, 60000);
  }
  void TearDown() override {
    timing_wheel_deinit(&wheel);
  }
};

TEST_F(TimingWheelTest, InitDeinit) {
  EXPECT_NE(wheel.slots, (timing_wheel_slot_t*)NULL);
  EXPECT_EQ(wheel.slot_count, 64u);
  EXPECT_EQ(wheel.slot_duration_ms, 60000u);
}

TEST_F(TimingWheelTest, AddEntry) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id = timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  EXPECT_NE(id, 0u);
  EXPECT_EQ(wheel.count, 1u);
}

TEST_F(TimingWheelTest, AddAndExpire) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  size_t expired_count = 0;
  timing_wheel_entry_t* expired = timing_wheel_advance(&wheel, 64, &expired_count);
  EXPECT_EQ(expired_count, 1u);
  EXPECT_EQ(wheel.count, 0u);
  free(expired);
}

TEST_F(TimingWheelTest, AddAndRefresh) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id1 = timing_wheel_add(&wheel, &peer_id, 0, 42, 7, block_hash);
  uint64_t id2 = timing_wheel_refresh(&wheel, id1, &peer_id, 0, 42, 7, block_hash);
  EXPECT_NE(id2, 0u);
  size_t expired_count = 0;
  timing_wheel_entry_t* expired = timing_wheel_advance(&wheel, 32, &expired_count);
  EXPECT_EQ(expired_count, 0u);
  free(expired);
  expired = timing_wheel_advance(&wheel, 32, &expired_count);
  EXPECT_EQ(expired_count, 1u);
  free(expired);
}

TEST_F(TimingWheelTest, RemoveById) {
  node_id_t peer_id = {};
  memset(peer_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  uint8_t block_hash[32];
  memset(block_hash, 0xBB, 32);
  uint64_t id = timing_wheel_add(&wheel, &peer_id, 1, 99, 3, block_hash);
  int result = timing_wheel_remove(&wheel, id);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(wheel.count, 0u);
}

TEST_F(TimingWheelTest, TTLForLevel) {
  uint64_t base = 3600000;
  EXPECT_EQ(timing_wheel_ttl_for_level(0, base), base);
  EXPECT_GT(timing_wheel_ttl_for_level(1, base), 0u);
  EXPECT_GT(timing_wheel_ttl_for_level(0, base), timing_wheel_ttl_for_level(1, base));
  EXPECT_GT(timing_wheel_ttl_for_level(1, base), timing_wheel_ttl_for_level(2, base));
}

// === HebbianConfig tests ===

class HebbianConfigTest : public ::testing::Test {
protected:
  hebbian_config_t config;
  void SetUp() override { hebbian_config_init(&config); }
};

TEST_F(HebbianConfigTest, Defaults) {
  EXPECT_FLOAT_EQ(config.initial_weight, 0.1f);
  EXPECT_FLOAT_EQ(config.drop_threshold, 0.01f);
  EXPECT_FLOAT_EQ(config.decay_rate, 0.001f);
  EXPECT_EQ(config.decay_tick_ms, 60000u);
  EXPECT_FLOAT_EQ(config.base_reward, 0.1f);
  EXPECT_FLOAT_EQ(config.failure_penalty, 0.2f);
  EXPECT_FLOAT_EQ(config.rate_limit_penalty, 0.1f);
  EXPECT_FLOAT_EQ(config.recall_reward, 2.0f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_FIND_BLOCK], 1.0f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_STORE_BLOCK], 1.5f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_PING_BLOCK], 0.8f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_SEEKING_BLOCKS], 0.5f);
  EXPECT_FLOAT_EQ(config.rpc_multipliers[WIRE_PING_CAPACITY], 0.3f);
}

TEST_F(HebbianConfigTest, ProductionDefaults) {
  hebbian_config_init_production(&config);
  EXPECT_FLOAT_EQ(config.decay_rate, 0.002f);
  EXPECT_FLOAT_EQ(config.drop_threshold, 0.05f);
  EXPECT_FLOAT_EQ(config.initial_weight, 0.1f);
}

// === PeerConnection tests ===

class PeerConnectionTest : public ::testing::Test {
protected:
  peer_connection_t* peer;
  node_id_t peer_id;
  void SetUp() override {
    memset(&peer_id, 0, sizeof(peer_id));
    memset(peer_id.hash, 0xCC, NODE_ID_HASH_SIZE);
    peer = peer_connection_create(&peer_id, NULL, 0.1f, NULL);
  }
  void TearDown() override {
    peer_connection_destroy(peer);
  }
};

TEST_F(PeerConnectionTest, CreateDestroy) {
  ASSERT_NE(peer, (peer_connection_t*)NULL);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.1f);
  EXPECT_TRUE(peer->connected);
  EXPECT_NE(peer->eabf, (eabf_t*)NULL);
}

TEST_F(PeerConnectionTest, HebbianUpdate) {
  peer_hebbian_update(peer, 0.5f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.6f);
  peer_hebbian_update(peer, -0.3f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.3f);
}

TEST_F(PeerConnectionTest, HebbianDecay) {
  peer_hebbian_update(peer, 0.5f);
  peer_hebbian_decay(peer, 0.1f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.5f);
}

TEST_F(PeerConnectionTest, HebbianClampZero) {
  peer_hebbian_update(peer, -1.0f);
  EXPECT_FLOAT_EQ(peer->hebbian_weight, 0.0f);
}

TEST_F(PeerConnectionTest, EABFSubscribeCheck) {
  uint8_t topic[32];
  memset(topic, 0xDD, 32);
  bool result = peer_eabf_subscribe(peer, topic, 32);
  EXPECT_TRUE(result);
  uint32_t hops = 0;
  bool found = peer_eabf_check(peer, topic, 32, &hops);
  EXPECT_TRUE(found);
  EXPECT_EQ(hops, 0u);
}

TEST_F(PeerConnectionTest, RTTUpdate) {
  peer_update_rtt(peer, 15.0);
  EXPECT_DOUBLE_EQ(peer->rtt_ewma, 15.0);
  peer_update_rtt(peer, 25.0);
  EXPECT_NEAR(peer->rtt_ewma, 16.0, 0.01);
}

TEST_F(PeerConnectionTest, MetricsSnapshot) {
  peer_hebbian_update(peer, 0.5f);
  peer_update_rtt(peer, 20.0);
  peer_metrics_snapshot_t snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  peer_get_metrics(peer, &snapshot);
  EXPECT_FLOAT_EQ(snapshot.hebbian_weight, 0.6f);
  EXPECT_DOUBLE_EQ(snapshot.rtt_ewma_ms, 20.0);
  EXPECT_TRUE(snapshot.connected);
}

TEST_F(PeerConnectionTest, EABFTickExpire) {
  uint8_t topic[32];
  memset(topic, 0xEE, 32);
  peer_eabf_subscribe(peer, topic, 32);

  // Subscribe adds to the bloom filter, but the TTL entry must be added
  // separately via timing_wheel_add. Use the peer's wheel directly.
  timing_wheel_add(&peer->eabf_wheel, &peer->remote_node_id,
                   0, 0, 0, topic);

  // Level 0 TTL = slot_count * slot_duration = 64 * 60000ms = 3840000ms.
  // slots_ahead = ttl / slot_duration = 3840000 / 60000 = 64, capped to 63.
  // So the entry lands at slot 63. We need to advance 63 slots to expire it.
  for (int i = 0; i < 63; i++) {
    peer_eabf_tick(peer);
  }

  // After 63 ticks, the entry should have expired and been removed from the EBF
  uint32_t hops = 0;
  bool found = peer_eabf_check(peer, topic, 32, &hops);
  EXPECT_FALSE(found);
}

// === ConnectionManager tests ===

class ConnectionManagerTest : public ::testing::Test {
protected:
  connection_manager_t mgr;
  hebbian_config_t config;
  void SetUp() override {
    hebbian_config_init(&config);
    connection_manager_init(&mgr, 4, &config);
  }
  void TearDown() override {
    connection_manager_deinit(&mgr);
  }
};

TEST_F(ConnectionManagerTest, InitDeinit) {
  EXPECT_NE(mgr.peers, (peer_connection_t**)NULL);
  EXPECT_EQ(mgr.peer_count, 0u);
  EXPECT_EQ(mgr.max_connections, 128u);
}

TEST_F(ConnectionManagerTest, AddLookupRemove) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  peer_connection_t* peer = connection_manager_add(&mgr, &id1, NULL, NULL);
  ASSERT_NE(peer, (peer_connection_t*)NULL);
  EXPECT_EQ(mgr.peer_count, 1u);

  peer_connection_t* found = connection_manager_lookup(&mgr, &id1);
  EXPECT_EQ(found, peer);

  int result = connection_manager_remove(&mgr, &id1);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(mgr.peer_count, 0u);
}

TEST_F(ConnectionManagerTest, AddMultiplePeers) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  node_id_t id2 = {};
  memset(id2.hash, 0xBB, NODE_ID_HASH_SIZE);
  node_id_t id3 = {};
  memset(id3.hash, 0xCC, NODE_ID_HASH_SIZE);

  connection_manager_add(&mgr, &id1, NULL, NULL);
  connection_manager_add(&mgr, &id2, NULL, NULL);
  connection_manager_add(&mgr, &id3, NULL, NULL);
  EXPECT_EQ(mgr.peer_count, 3u);

  // Remove middle one
  int result = connection_manager_remove(&mgr, &id2);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(mgr.peer_count, 2u);

  // Verify removed
  peer_connection_t* found = connection_manager_lookup(&mgr, &id2);
  EXPECT_EQ(found, (peer_connection_t*)NULL);
}

TEST_F(ConnectionManagerTest, GravityWellSearch) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  node_id_t id2 = {};
  memset(id2.hash, 0xBB, NODE_ID_HASH_SIZE);

  peer_connection_t* peer1 = connection_manager_add(&mgr, &id1, NULL, NULL);
  peer_connection_t* peer2 = connection_manager_add(&mgr, &id2, NULL, NULL);

  uint8_t topic[32];
  memset(topic, 0xDD, 32);
  peer_eabf_subscribe(peer1, topic, 32);

  size_t match_count = 0;
  peer_connection_t** matches = connection_manager_get_peers_for_topic(&mgr, topic, 32, &match_count);
  ASSERT_NE(matches, (peer_connection_t**)NULL);
  EXPECT_EQ(match_count, 1u);
  EXPECT_EQ(matches[0], peer1);
  free(matches);
}

TEST_F(ConnectionManagerTest, DecayTickRemovesLowWeight) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  peer_connection_t* peer = connection_manager_add(&mgr, &id1, NULL, NULL);
  ASSERT_NE(peer, (peer_connection_t*)NULL);
  // Initial weight is 0.1, decay_rate is 0.001, drop_threshold is 0.01
  // After 100 decay ticks: weight = 0.1 - 100*0.001 = 0.0, which is < 0.01
  size_t removed = 0;
  for (int i = 0; i < 100; i++) {
    removed = connection_manager_decay_tick(&mgr);
    if (removed > 0) break;
  }
  EXPECT_GT(removed, 0u);
  EXPECT_EQ(mgr.peer_count, 0u);
}

TEST_F(ConnectionManagerTest, CollectMetrics) {
  node_id_t id1 = {};
  memset(id1.hash, 0xAA, NODE_ID_HASH_SIZE);
  connection_manager_add(&mgr, &id1, NULL, NULL);

  peer_metrics_snapshot_t snapshots[4];
  size_t count = connection_manager_collect_metrics(&mgr, snapshots, 4);
  EXPECT_EQ(count, 1u);
  EXPECT_TRUE(snapshots[0].connected);
}

// === TopologyMetrics tests ===

class TopologyMetricsTest : public ::testing::Test {
protected:
  topology_metrics_t* metrics;
  void SetUp() override {
    metrics = topology_metrics_create(NULL);
  }
  void TearDown() override {
    topology_metrics_destroy(metrics);
  }
};

TEST_F(TopologyMetricsTest, CreateDestroy) {
  ASSERT_NE(metrics, (topology_metrics_t*)NULL);
  EXPECT_EQ(metrics->peer_snapshot_count, 0u);
  EXPECT_EQ(metrics->ring_entry_count, 0u);
}

TEST_F(TopologyMetricsTest, UpdatePeers) {
  peer_metrics_snapshot_t snapshots[2];
  memset(snapshots, 0, sizeof(snapshots));
  memset(snapshots[0].node_id.hash, 0xAA, NODE_ID_HASH_SIZE);
  snapshots[0].hebbian_weight = 0.5f;
  snapshots[0].rtt_ewma_ms = 20.0;
  snapshots[0].connected = true;
  memset(snapshots[1].node_id.hash, 0xBB, NODE_ID_HASH_SIZE);
  snapshots[1].hebbian_weight = 0.8f;
  snapshots[1].rtt_ewma_ms = 30.0;
  snapshots[1].connected = true;

  topology_metrics_update_peers(metrics, snapshots, 2);
  EXPECT_EQ(metrics->peer_snapshot_count, 2u);
  EXPECT_EQ(metrics->total_connections, 2u);
  EXPECT_FLOAT_EQ(metrics->avg_hebbian_weight, 0.65f);
}

TEST_F(TopologyMetricsTest, UpdateRings) {
  ring_topology_entry_t entries[3];
  memset(entries, 0, sizeof(entries));
  memset(entries[0].node_id.hash, 0x11, NODE_ID_HASH_SIZE);
  entries[0].ring_level = 0;
  entries[0].rtt_ms = 5.0;
  entries[0].is_active_connection = true;
  memset(entries[1].node_id.hash, 0x22, NODE_ID_HASH_SIZE);
  entries[1].ring_level = 1;
  entries[1].rtt_ms = 15.0;
  entries[1].is_active_connection = false;

  topology_metrics_update_rings(metrics, entries, 2);
  EXPECT_EQ(metrics->ring_entry_count, 2u);
  EXPECT_EQ(metrics->ring_entries[0].ring_level, 0u);
  EXPECT_EQ(metrics->ring_entries[1].ring_level, 1u);
}

TEST_F(TopologyMetricsTest, AggregateRpcCalls) {
  peer_metrics_snapshot_t snapshots[2];
  memset(snapshots, 0, sizeof(snapshots));
  snapshots[0].rpc_count[WIRE_FIND_BLOCK] = 10;
  snapshots[0].rpc_count[WIRE_STORE_BLOCK] = 5;
  snapshots[0].connected = true;
  snapshots[1].rpc_count[WIRE_FIND_BLOCK] = 20;
  snapshots[1].rpc_count[WIRE_STORE_BLOCK] = 15;
  snapshots[1].connected = true;

  topology_metrics_update_peers(metrics, snapshots, 2);
  EXPECT_EQ(metrics->total_rpc_calls[WIRE_FIND_BLOCK], 30u);
  EXPECT_EQ(metrics->total_rpc_calls[WIRE_STORE_BLOCK], 20u);
}

// === Wire protocol CBOR round-trip tests ===

// Helper: encode CBOR item to bytes, then decode back
static int wire_encode_decode_roundtrip(cbor_item_t* encoded, unsigned char** out, size_t* out_len) {
  size_t written = cbor_serialize_alloc(encoded, out, out_len);
  if (written == 0) return -1;
  return 0;
}

// --- FindBlockResponse round-trip tests ---

TEST(WireFindBlockResponseTest, EncodeDecodeWithoutBlockData) {
  wire_find_block_response_t original = {};
  original.message_id = 0xAABBCCDDEEFF0011ULL;
  memset(original.block_hash, 0xDD, 32);
  original.found = 0;
  memset(original.holder.hash, 0x11, NODE_ID_HASH_SIZE);
  strcpy(original.holder.str, "node-holder");
  original.fib = 42;
  memset(original.path[0].hash, 0x22, NODE_ID_HASH_SIZE);
  strcpy(original.path[0].str, "path-node-0");
  original.path_len = 1;
  original.latency_ms = 1500;
  original.block_data = NULL;
  original.block_data_len = 0;
  original.block_fib = 0;

  cbor_item_t* encoded = wire_find_block_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Serialize and deserialize
  unsigned char* buf = NULL;
  size_t buf_len = 0;
  int rc = wire_encode_decode_roundtrip(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(buf, nullptr);

  // Parse CBOR
  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_find_block_response_t result = {};
  rc = wire_find_block_response_decode(decoded, &result);
  cbor_decref(&decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(result.message_id, original.message_id);
  EXPECT_EQ(result.found, original.found);
  EXPECT_EQ(result.fib, original.fib);
  EXPECT_EQ(result.path_len, original.path_len);
  EXPECT_EQ(result.latency_ms, original.latency_ms);
  EXPECT_EQ(result.block_data_len, (size_t)0);
  EXPECT_EQ(result.block_data, nullptr);
  EXPECT_EQ(result.block_fib, (uint32_t)0);
  EXPECT_EQ(memcmp(result.block_hash, original.block_hash, 32), 0);
}

TEST(WireFindBlockResponseTest, EncodeDecodeWithBlockData) {
  wire_find_block_response_t original = {};
  original.message_id = 0x1122334455667788ULL;
  memset(original.block_hash, 0xAA, 32);
  original.found = 1;
  memset(original.holder.hash, 0xBB, NODE_ID_HASH_SIZE);
  strcpy(original.holder.str, "holder-node");
  original.fib = 99;
  memset(original.path[0].hash, 0xCC, NODE_ID_HASH_SIZE);
  strcpy(original.path[0].str, "path-node-0");
  memset(original.path[1].hash, 0xDD, NODE_ID_HASH_SIZE);
  strcpy(original.path[1].str, "path-node-1");
  original.path_len = 2;
  original.latency_ms = 3200;
  uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  original.block_data = test_data;
  original.block_data_len = sizeof(test_data);
  original.block_fib = 77;

  cbor_item_t* encoded = wire_find_block_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Verify the array has 12 elements (9 base + 3 block data)
  EXPECT_EQ(cbor_array_size(encoded), (size_t)12);

  unsigned char* buf = NULL;
  size_t buf_len = 0;
  int rc = wire_encode_decode_roundtrip(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_find_block_response_t result = {};
  rc = wire_find_block_response_decode(decoded, &result);
  cbor_decref(&decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(result.message_id, original.message_id);
  EXPECT_EQ(result.found, original.found);
  EXPECT_EQ(result.fib, original.fib);
  EXPECT_EQ(result.path_len, original.path_len);
  EXPECT_EQ(result.latency_ms, original.latency_ms);
  EXPECT_EQ(result.block_data_len, original.block_data_len);
  EXPECT_NE(result.block_data, nullptr);
  EXPECT_EQ(result.block_fib, original.block_fib);
  EXPECT_EQ(memcmp(result.block_data, original.block_data, original.block_data_len), 0);
  EXPECT_EQ(memcmp(result.block_hash, original.block_hash, 32), 0);

  free(result.block_data);
}

TEST(WireFindBlockResponseTest, EncodeDecodeFoundButNoBlockData) {
  wire_find_block_response_t original = {};
  original.message_id = 0x1234ULL;
  memset(original.block_hash, 0xEE, 32);
  original.found = 1;  // found=true, but block_data is NULL
  memset(original.holder.hash, 0xFF, NODE_ID_HASH_SIZE);
  strcpy(original.holder.str, "holder");
  original.fib = 10;
  original.path_len = 0;
  original.latency_ms = 500;
  original.block_data = NULL;  // No inline data even though found
  original.block_data_len = 0;
  original.block_fib = 0;

  cbor_item_t* encoded = wire_find_block_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Should produce 9 elements (no block data)
  EXPECT_EQ(cbor_array_size(encoded), (size_t)9);

  unsigned char* buf = NULL;
  size_t buf_len = 0;
  int rc = wire_encode_decode_roundtrip(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_find_block_response_t result = {};
  rc = wire_find_block_response_decode(decoded, &result);
  cbor_decref(&decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(result.found, 1);
  EXPECT_EQ(result.block_data, nullptr);
  EXPECT_EQ(result.block_data_len, (size_t)0);
  EXPECT_EQ(result.block_fib, (uint32_t)0);
}

// --- RecallAccept round-trip tests ---

TEST(WireRecallAcceptTest, EncodeDecodeWithoutBlockData) {
  wire_recall_accept_t original = {};
  original.message_id = 0xDEADBEEFCAFEBABEULL;
  memset(original.block_hash, 0x55, 32);
  original.block_data = NULL;
  original.block_data_len = 0;
  original.block_fib = 0;

  cbor_item_t* encoded = wire_recall_accept_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Should produce 4 elements (no block data)
  EXPECT_EQ(cbor_array_size(encoded), (size_t)4);

  unsigned char* buf = NULL;
  size_t buf_len = 0;
  int rc = wire_encode_decode_roundtrip(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_recall_accept_t result = {};
  rc = wire_recall_accept_decode(decoded, &result);
  cbor_decref(&decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(result.message_id, original.message_id);
  EXPECT_EQ(memcmp(result.block_hash, original.block_hash, 32), 0);
  EXPECT_EQ(result.block_data, nullptr);
  EXPECT_EQ(result.block_data_len, (size_t)0);
  EXPECT_EQ(result.block_fib, (uint32_t)0);
}

TEST(WireRecallAcceptTest, EncodeDecodeWithBlockData) {
  wire_recall_accept_t original = {};
  original.message_id = 0x0102030405060708ULL;
  memset(original.block_hash, 0x77, 32);
  uint8_t test_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
  original.block_data = test_data;
  original.block_data_len = sizeof(test_data);
  original.block_fib = 1234;

  cbor_item_t* encoded = wire_recall_accept_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Should produce 7 elements (4 base + 3 block data)
  EXPECT_EQ(cbor_array_size(encoded), (size_t)7);

  unsigned char* buf = NULL;
  size_t buf_len = 0;
  int rc = wire_encode_decode_roundtrip(encoded, &buf, &buf_len);
  cbor_decref(&encoded);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(buf, nullptr);

  struct cbor_load_result load_result;
  cbor_item_t* decoded = cbor_load(buf, buf_len, &load_result);
  free(buf);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(load_result.error.code, CBOR_ERR_NONE);

  wire_recall_accept_t result = {};
  rc = wire_recall_accept_decode(decoded, &result);
  cbor_decref(&decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(result.message_id, original.message_id);
  EXPECT_EQ(memcmp(result.block_hash, original.block_hash, 32), 0);
  EXPECT_NE(result.block_data, nullptr);
  EXPECT_EQ(result.block_data_len, original.block_data_len);
  EXPECT_EQ(memcmp(result.block_data, original.block_data, original.block_data_len), 0);
  EXPECT_EQ(result.block_fib, original.block_fib);

  free(result.block_data);
}

// --- Destroy function tests ---

TEST(WireDestroyTest, FindBlockResponseDestroyFreesBlockData) {
  wire_find_block_response_t* msg = (wire_find_block_response_t*)get_clear_memory(sizeof(wire_find_block_response_t));
  ASSERT_NE(msg, nullptr);
  uint8_t data[] = {0x01, 0x02, 0x03};
  msg->block_data = (uint8_t*)malloc(sizeof(data));
  memcpy(msg->block_data, data, sizeof(data));
  msg->block_data_len = sizeof(data);

  wire_find_block_response_destroy(msg);
  // No use-after-free crash means success
}

TEST(WireDestroyTest, FindBlockResponseDestroyNullBlockData) {
  wire_find_block_response_t* msg = (wire_find_block_response_t*)get_clear_memory(sizeof(wire_find_block_response_t));
  ASSERT_NE(msg, nullptr);
  msg->block_data = NULL;
  msg->block_data_len = 0;

  wire_find_block_response_destroy(msg);
}

TEST(WireDestroyTest, RecallAcceptDestroyFreesBlockData) {
  wire_recall_accept_t* msg = (wire_recall_accept_t*)get_clear_memory(sizeof(wire_recall_accept_t));
  ASSERT_NE(msg, nullptr);
  uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
  msg->block_data = (uint8_t*)malloc(sizeof(data));
  memcpy(msg->block_data, data, sizeof(data));
  msg->block_data_len = sizeof(data);
  msg->block_fib = 42;

  wire_recall_accept_destroy(msg);
}

TEST(WireDestroyTest, RecallAcceptDestroyNullBlockData) {
  wire_recall_accept_t* msg = (wire_recall_accept_t*)get_clear_memory(sizeof(wire_recall_accept_t));
  ASSERT_NE(msg, nullptr);
  msg->block_data = NULL;
  msg->block_data_len = 0;

  wire_recall_accept_destroy(msg);
}

TEST(WireDestroyTest, DestroyNullPointerIsSafe) {
  wire_find_block_response_destroy(NULL);
  wire_recall_accept_destroy(NULL);
}