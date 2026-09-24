// Peer-book actor: mutation round-trips, snapshots, config seeding before
// start, and a concurrent-mutation race regression. Fixture mirrors the
// minimal-network style of test_authority_bootstrap.cpp: a heap authority
// over a stack config plus a dedicated scheduler pool for the actor.
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <string>

extern "C" {
#include "../src/Configuration/config.h"
#include "../src/Network/peer_book.h"
#include "../src/Network/authority.h"
#include "../src/Network/endpoint.h"
#include "../src/Network/node_id.h"
#include "../src/Util/allocator.h"
#include "../src/Util/base58.h"
}

namespace {

// Builds a well-formed peer_info_t (public_key is required by
// peer_info_encode) with one HOST candidate address.
static peer_info_t* make_peer_info(uint8_t id_suffix) {
  peer_info_t* info = (peer_info_t*)get_clear_memory(sizeof(peer_info_t));
  node_id_generate(&info->node_id);
  info->node_id.hash[NODE_ID_HASH_SIZE - 1] = id_suffix;
  base58_encode(info->node_id.hash, NODE_ID_HASH_SIZE, info->node_id.str,
                NODE_ID_STRING_SIZE);
  info->public_key = (uint8_t*)get_clear_memory(32);
  for (size_t index = 0; index < 32; index++) {
    info->public_key[index] = (uint8_t)(index + id_suffix);
  }
  info->public_key_len = 32;
  info->addresses =
      (peer_address_t*)get_clear_memory(sizeof(peer_address_t));
  info->addresses[0].type = PEER_ADDR_HOST;
  info->addresses[0].host = strdup("192.168.1.5");
  info->addresses[0].port = (uint16_t)(7000 + id_suffix);
  info->address_count = 1;
  return info;
}

class PeerBook : public testing::Test {
public:
  config_t config;
  authority_t* authority;
  scheduler_pool_t* pool;
  peer_book_t* peer_book;

  void SetUp() override {
    memset(&config, 0, sizeof(config));
    authority = authority_create(&config);
    ASSERT_NE(authority, nullptr);
    pool = scheduler_pool_create(4);
    ASSERT_NE(pool, nullptr);
    scheduler_pool_start(pool);
    peer_book = peer_book_create(authority, NULL, NULL, pool);
    ASSERT_NE(peer_book, nullptr);
    ASSERT_EQ(peer_book_start(peer_book), 0);
  }

  void TearDown() override {
    peer_book_destroy(peer_book);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    authority_destroy(authority);
  }
};

TEST_F(PeerBook, BootstrapMutationRoundTrip) {
  EXPECT_EQ(0, peer_book_bootstrap_add(peer_book, "10.0.0.1:8080",
                                       PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(-2, peer_book_bootstrap_add(peer_book, "10.0.0.1:8080",
                                        PEER_BOOK_TIMEOUT_MS));  // dup
  EXPECT_EQ(-1, peer_book_bootstrap_add(peer_book, "not-an-endpoint",
                                        PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(0, peer_book_bootstrap_add(peer_book, "[2001:db8::1]:9090",
                                       PEER_BOOK_TIMEOUT_MS));

  // Snapshot: config list empty, managed list holds both normalized entries.
  char** config_endpoints = NULL;
  size_t config_count = 0;
  char** managed_endpoints = NULL;
  size_t managed_count = 0;
  ASSERT_EQ(0, peer_book_snapshot_bootstrap(peer_book, &config_endpoints,
                                            &config_count, &managed_endpoints,
                                            &managed_count,
                                            PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(0u, config_count);
  ASSERT_EQ(2u, managed_count);
  EXPECT_STREQ("10.0.0.1:8080", managed_endpoints[0]);
  EXPECT_STREQ("[2001:db8::1]:9090", managed_endpoints[1]);

  EXPECT_EQ(0, peer_book_bootstrap_remove(peer_book, "10.0.0.1:8080",
                                          PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(-1, peer_book_bootstrap_remove(peer_book, "10.0.0.1:8080",
                                           PEER_BOOK_TIMEOUT_MS));  // gone
  // The managed (operator-added) entry is removable, unlike config-seeded ones.
  EXPECT_EQ(0, peer_book_bootstrap_remove(peer_book, "[2001:db8::1]:9090",
                                          PEER_BOOK_TIMEOUT_MS));

  peer_book_free_string_array(config_endpoints, config_count);
  peer_book_free_string_array(managed_endpoints, managed_count);
}

TEST_F(PeerBook, SnapshotBootstrapReflectsState) {
  // Startup-phase direct seeding (legal before peer_book_start, exercised
  // here after start through the round-trip: the actor applies against the
  // same authority storage).
  ASSERT_EQ(0, authority_set_bootstrap_peers(authority, "10.0.0.3:7070"));
  ASSERT_EQ(0, peer_book_bootstrap_add(peer_book, "10.0.0.4:7071",
                                       PEER_BOOK_TIMEOUT_MS));

  char** config_endpoints = NULL;
  size_t config_count = 0;
  char** managed_endpoints = NULL;
  size_t managed_count = 0;
  ASSERT_EQ(0, peer_book_snapshot_bootstrap(peer_book, &config_endpoints,
                                            &config_count, &managed_endpoints,
                                            &managed_count,
                                            PEER_BOOK_TIMEOUT_MS));
  ASSERT_EQ(1u, config_count);
  EXPECT_STREQ("10.0.0.3:7070", config_endpoints[0]);
  ASSERT_EQ(1u, managed_count);
  EXPECT_STREQ("10.0.0.4:7071", managed_endpoints[0]);

  peer_book_free_string_array(config_endpoints, config_count);
  peer_book_free_string_array(managed_endpoints, managed_count);
}

TEST_F(PeerBook, FriendAddRemoveSnapshot) {
  peer_info_t* info = make_peer_info(7);

  EXPECT_EQ(0, peer_book_friend_add(peer_book, info, PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(-2, peer_book_friend_add(peer_book, info,
                                     PEER_BOOK_TIMEOUT_MS));  // dup

  // Snapshot returns a deep copy: same node id, same single address.
  peer_info_t** friends = NULL;
  size_t friend_count = 0;
  ASSERT_EQ(0, peer_book_snapshot_friends(peer_book, &friends, &friend_count,
                                          PEER_BOOK_TIMEOUT_MS));
  ASSERT_EQ(1u, friend_count);
  EXPECT_TRUE(node_id_equals(&friends[0]->node_id, &info->node_id));
  ASSERT_EQ(1u, friends[0]->address_count);
  EXPECT_STREQ("192.168.1.5", friends[0]->addresses[0].host);
  EXPECT_EQ((uint16_t)(7000 + 7), friends[0]->addresses[0].port);
  peer_book_free_peer_info_array(friends, friend_count);

  EXPECT_EQ(0, peer_book_friend_remove(peer_book, &info->node_id,
                                       PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(-1, peer_book_friend_remove(peer_book, &info->node_id,
                                        PEER_BOOK_TIMEOUT_MS));  // not found

  friends = NULL;
  friend_count = 0;
  ASSERT_EQ(0, peer_book_snapshot_friends(peer_book, &friends, &friend_count,
                                          PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(0u, friend_count);
  peer_book_free_peer_info_array(friends, friend_count);

  peer_info_destroy(info);
  free(info);
}

// Race regression: the actor serializes mutations, so concurrent adds with
// disjoint endpoints must all land without corruption.
TEST_F(PeerBook, ConcurrentBootstrapAdds) {
  const int k_threads = 4;
  const int k_ops_per_thread = 25;

  std::vector<std::thread> workers;
  for (int thread_index = 0; thread_index < k_threads; thread_index++) {
    workers.emplace_back([this, thread_index]() {
      for (int op_index = 0; op_index < k_ops_per_thread; op_index++) {
        char endpoint[64];
        snprintf(endpoint, sizeof(endpoint), "10.%d.%d.5:8000", thread_index,
                 op_index);
        EXPECT_EQ(0, peer_book_bootstrap_add(peer_book, endpoint,
                                             PEER_BOOK_TIMEOUT_MS));
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  char** config_endpoints = NULL;
  size_t config_count = 0;
  char** managed_endpoints = NULL;
  size_t managed_count = 0;
  ASSERT_EQ(0, peer_book_snapshot_bootstrap(peer_book, &config_endpoints,
                                            &config_count, &managed_endpoints,
                                            &managed_count,
                                            PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(0u, config_count);
  EXPECT_EQ((size_t)(k_threads * k_ops_per_thread), managed_count);

  peer_book_free_string_array(config_endpoints, config_count);
  peer_book_free_string_array(managed_endpoints, managed_count);
}

// After peer_book_stop the round-trips fail closed (-1) instead of blocking
// or touching the lists.
TEST_F(PeerBook, MutationsFailClosedAfterStop) {
  peer_book_stop(peer_book);
  EXPECT_EQ(-1, peer_book_bootstrap_add(peer_book, "10.0.0.9:7000",
                                        PEER_BOOK_TIMEOUT_MS));
  peer_info_t** friends = NULL;
  size_t friend_count = 99;
  EXPECT_EQ(-1, peer_book_snapshot_friends(peer_book, &friends, &friend_count,
                                           PEER_BOOK_TIMEOUT_MS));
  EXPECT_EQ(0u, friend_count);
}

}  // namespace