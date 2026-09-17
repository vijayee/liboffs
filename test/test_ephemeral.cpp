#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "../src/BlockCache/index.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/sections.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Platform/platform_time.h"
#include <cbor.h>
}

/* ---- index entry CBOR round-trip with the new fields ---- */

TEST(TestEphemeralIndex, EntryCborRoundTripNewFields) {
  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);
  index_entry_t* entry = index_entry_from(hash, 7, 3, 12345,
                                          fibonacci_hit_counter_create(),
                                          /*ephemeral_count=*/2, /*pin_count=*/5);
  cbor_item_t* cbor = index_entry_to_cbor(entry);
  ASSERT_NE(cbor, nullptr);
  EXPECT_EQ(cbor_array_size(cbor), 7u);
  index_entry_t* decoded = cbor_to_index_entry(cbor);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->ephemeral_count, 2u);
  EXPECT_EQ(decoded->pin_count, 5u);
  EXPECT_EQ(decoded->section_id, 7u);
  EXPECT_EQ(decoded->section_index, 3u);
  EXPECT_EQ(decoded->ejection_date, 12345u);
  EXPECT_EQ(buffer_compare(decoded->hash, hash), 0);
  cbor_decref(&cbor);
  index_entry_destroy(decoded);
  index_entry_destroy(entry);
  DESTROY(hash, buffer);
}

TEST(TestEphemeralIndex, EntryCborDecodeOldFiveElementArray) {
  /* Old-format 5-element array must decode with ephemeral=0, pin=0. */
  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);
  fibonacci_hit_counter_t counter = fibonacci_hit_counter_create();
  cbor_item_t* array = cbor_new_definite_array(5);
  (void)cbor_array_push(array, cbor_move(fibonacci_hit_counter_to_cbor(&counter)));
  (void)cbor_array_push(array, cbor_move(buffer_to_cbor(hash)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(3)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(7)));
  (void)cbor_array_push(array, cbor_move(cbor_build_uint64(12345)));
  index_entry_t* decoded = cbor_to_index_entry(array);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->ephemeral_count, 0u);
  EXPECT_EQ(decoded->pin_count, 0u);
  cbor_decref(&array);
  index_entry_destroy(decoded);
  DESTROY(hash, buffer);
}