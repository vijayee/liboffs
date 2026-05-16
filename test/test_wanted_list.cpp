//
// Created by victor on 5/15/25.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/Network/wanted_list.h"
#include "../src/Buffer/buffer.h"
#include "../src/RefCounter/refcounter.h"
}

static buffer_t* make_hash(const uint8_t* data, size_t len) {
  return buffer_create_from_pointer_copy((uint8_t*)data, len);
}

TEST(TestWantedList, CreateDestroy) {
  wanted_list_t* wl = wanted_list_create();
  ASSERT_NE(wl, nullptr);
  wanted_list_destroy(wl);
}

TEST(TestWantedList, CheckReturnsFalseForUnknown) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  ASSERT_NE(hash, nullptr);
  EXPECT_FALSE(wanted_list_check(wl, hash));
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(TestWantedList, AddAndCheck) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t dummy_actor = {0};
  wanted_list_add(wl, hash, &dummy_actor);
  EXPECT_TRUE(wanted_list_check(wl, hash));
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(TestWantedList, AddMultipleRequesters) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor2);
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);
  /* Should have 2 requesters */
  int count = 0;
  wanted_requester_t* req = entry->requesters;
  while (req != nullptr) { count++; req = req->next; }
  EXPECT_EQ(count, 2);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(TestWantedList, RemoveReturnsRequesters) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_list_add(wl, hash, &actor2);
  wanted_requester_t* requesters = wanted_list_remove(wl, hash);
  ASSERT_NE(requesters, nullptr);
  /* Bloom should no longer contain the hash (well, bloom can't remove,
     but the entry is gone so check still returns true for bloom false-positive) */
  EXPECT_FALSE(wanted_list_find(wl, hash) != nullptr);
  /* Free requesters */
  wanted_requester_list_destroy(requesters);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(TestWantedList, ClearRequestersKeepsBloom) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1 = {0};
  wanted_list_add(wl, hash, &actor1);
  wanted_requester_t* requesters = wanted_list_clear_requesters(wl, hash);
  /* Bloom should still contain the hash */
  EXPECT_TRUE(wanted_list_check(wl, hash));
  /* But find should return NULL (no entry) */
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  EXPECT_EQ(entry, nullptr);
  /* Free requesters */
  wanted_requester_list_destroy(requesters);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

TEST(TestWantedList, RetryAfterFailure) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1 = {0};
  actor_t actor2 = {0};
  /* First request */
  wanted_list_add(wl, hash, &actor1);
  /* Fail: clear requesters but keep bloom */
  wanted_requester_t* reqs = wanted_list_clear_requesters(wl, hash);
  wanted_requester_list_destroy(reqs);
  /* Bloom hit but no entry -> fresh request */
  EXPECT_TRUE(wanted_list_check(wl, hash));
  EXPECT_EQ(wanted_list_find(wl, hash), nullptr);
  /* New request after failure */
  wanted_list_add(wl, hash, &actor2);
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}