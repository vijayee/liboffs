//
// Created by victor on 5/15/25.
//
#include <gtest/gtest.h>
#include <cstring>
#include <ctime>
#include <vector>
extern "C" {
#include "../src/Network/wanted_list.h"
#include "../src/Buffer/buffer.h"
#include "../src/RefCounter/refcounter.h"
#include "../src/Platform/platform_time.h"
}

static buffer_t* make_hash(const uint8_t* data, size_t len) {
  return buffer_create_from_pointer_copy((uint8_t*)data, len);
}

/* Monotonic clock in ms — used for the timeout tests, which need sub-second
   resolution. The production sweep handler uses time(NULL) * 1000 (second
   granularity), but the sweep function itself is clock-agnostic: it just
   compares deadline_ms against the now_ms the caller passes. So the test
   uses the monotonic clock for both the deadline and the sweep_now, which
   makes the 100ms deadline reliable without depending on a second boundary
   crossing. */
static uint64_t now_ms_monotonic(void) {
  return platform_monotonic_ns() / 1000000ULL;
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
  actor_t dummy_actor;
  memset(&dummy_actor, 0, sizeof(dummy_actor));
  wanted_list_add(wl, hash, &dummy_actor, 0);
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
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));
  wanted_list_add(wl, hash, &actor1, 0);
  wanted_list_add(wl, hash, &actor2, 0);
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
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));
  wanted_list_add(wl, hash, &actor1, 0);
  wanted_list_add(wl, hash, &actor2, 0);
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
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  wanted_list_add(wl, hash, &actor1, 0);
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
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));
  /* First request */
  wanted_list_add(wl, hash, &actor1, 0);
  /* Fail: clear requesters but keep bloom */
  wanted_requester_t* reqs = wanted_list_clear_requesters(wl, hash);
  wanted_requester_list_destroy(reqs);
  /* Bloom hit but no entry -> fresh request */
  EXPECT_TRUE(wanted_list_check(wl, hash));
  EXPECT_EQ(wanted_list_find(wl, hash), nullptr);
  /* New request after failure */
  wanted_list_add(wl, hash, &actor2, 0);
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  ASSERT_NE(entry, nullptr);
  wanted_list_destroy(wl);
  buffer_destroy(hash);
}

// === Timeout sweep tests (audit #5, #9) ===
//
// wanted_list entries had no deadline: a lost response or dead hop left the
// origin's wanted_list entry and the requesting stream actor alive for the
// process lifetime. The fix is a per-entry deadline_ms (0 = no deadline, back
// compat) and a wanted_list_sweep that walks the list, removes expired
// entries, and hands each expired entry's (hash, requesters) to a callback.
// The callback owns the requesters list (must free each requester) and may
// REFERENCE the hash (the sweep destroys entry->hash after the callback, so
// REFERENCE bumps the refcount and the sweep's buffer_destroy decrements it
// — net +1 for the callback's result). See audit #5/#9.

namespace {
struct SweepCapture {
  buffer_t* hash;
  wanted_requester_t* requesters;
};

static void capture_expired_cb(buffer_t* hash, wanted_requester_t* requesters,
                                void* user_data) {
  auto* captures = static_cast<std::vector<SweepCapture>*>(user_data);
  SweepCapture cap;
  cap.hash = (hash != NULL) ? REFERENCE(hash, buffer_t) : NULL;
  cap.requesters = requesters;
  captures->push_back(cap);
}

static size_t requester_list_length(wanted_requester_t* requesters) {
  size_t count = 0;
  while (requesters != NULL) {
    count++;
    requesters = requesters->next;
  }
  return count;
}
}  // namespace

TEST(WantedListTimeout, SweepExpiresEntriesAndReturnsRequesters) {
  wanted_list_t* wl = wanted_list_create();
  ASSERT_NE(wl, nullptr);
  uint8_t hash_data[32];
  memset(hash_data, 0xAB, 32);
  buffer_t* hash = make_hash(hash_data, 32);
  ASSERT_NE(hash, nullptr);

  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  actor_t actor2;
  memset(&actor2, 0, sizeof(actor2));

  uint64_t base = now_ms_monotonic();
  /* Add an entry with a deadline 100ms in the future, plus a second requester
     so we can verify the entire requester list is handed to the callback. */
  wanted_list_add(wl, hash, &actor1, base + 100);
  wanted_list_add(wl, hash, &actor2, base + 100);

  /* Wait past the deadline. */
  platform_sleep_ms(150);

  std::vector<SweepCapture> captures;
  wanted_list_sweep(wl, now_ms_monotonic(), capture_expired_cb, &captures);

  ASSERT_EQ(captures.size(), 1u);
  EXPECT_EQ(requester_list_length(captures[0].requesters), (size_t)2);
  EXPECT_NE(captures[0].hash, nullptr);
  /* The entry is gone. */
  EXPECT_EQ(wanted_list_find(wl, hash), nullptr);

  /* Cleanup: free the captured requesters and the hash reference. */
  wanted_requester_list_destroy(captures[0].requesters);
  buffer_destroy(captures[0].hash);
  buffer_destroy(hash);
  wanted_list_destroy(wl);
}

TEST(WantedListTimeout, SweepKeepsNonExpiredEntries) {
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[32];
  memset(hash_data, 0xCD, 32);
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  uint64_t base = now_ms_monotonic();
  /* 60s deadline — not expired. */
  wanted_list_add(wl, hash, &actor1, base + 60000);

  std::vector<SweepCapture> captures;
  wanted_list_sweep(wl, base, capture_expired_cb, &captures);
  EXPECT_EQ(captures.size(), 0u);                  /* nothing expired */
  EXPECT_NE(wanted_list_find(wl, hash), nullptr);  /* entry still present */

  buffer_destroy(hash);
  wanted_list_destroy(wl);
}

TEST(WantedListTimeout, SweepSkipsEntriesWithNoDeadline) {
  /* deadline_ms == 0 means "no deadline" (back-compat for callers that don't
     opt into the timeout). The sweep must never remove such entries. */
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[32];
  memset(hash_data, 0xEF, 32);
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  wanted_list_add(wl, hash, &actor1, 0);  /* no deadline */

  std::vector<SweepCapture> captures;
  wanted_list_sweep(wl, now_ms_monotonic(), capture_expired_cb, &captures);
  EXPECT_EQ(captures.size(), 0u);
  EXPECT_NE(wanted_list_find(wl, hash), nullptr);

  buffer_destroy(hash);
  wanted_list_destroy(wl);
}

TEST(WantedListTimeout, GetReturnsEntryForAccounting) {
  /* wanted_list_get is the non-removing lookup for fanout/not_found
     accounting (Task 4). It must return the same entry as wanted_list_find. */
  wanted_list_t* wl = wanted_list_create();
  uint8_t hash_data[32];
  memset(hash_data, 0x77, 32);
  buffer_t* hash = make_hash(hash_data, 32);
  actor_t actor1;
  memset(&actor1, 0, sizeof(actor1));
  wanted_list_add(wl, hash, &actor1, 0);

  wanted_entry_t* entry = wanted_list_get(wl, hash);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry, wanted_list_find(wl, hash));
  EXPECT_EQ(entry->fanout_count, (uint8_t)0);
  EXPECT_EQ(entry->not_found_count, (uint8_t)0);

  buffer_destroy(hash);
  wanted_list_destroy(wl);
}