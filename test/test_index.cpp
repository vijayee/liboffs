//
// Created by victor on 5/27/25.
//
#include <gtest/gtest.h>
extern "C" {
#include "../src/Buffer/buffer.h"
#include "../src/BlockCache/block.h"
#include "../src/BlockCache/fibonacci.h"
#include "../src/BlockCache/index.h"
#include "../src/Util/path_join.h"
#include <time.h>
#include <cbor.h>
#include "../src/Time/wheel.h"
}

namespace indexTest {
  TEST(TestPath, TestPathJoin) {
    char *location = path_join(".", "wal");
    EXPECT_EQ(strcmp(location, "./wal"), 0);
    free(location);
  }

  int littleEndian() {
    int n = 1;
    if (*(char *) &n == 1) {
      return 1;
    } else {
      return 0;
    }
  }

  TEST(TestIndex, TestBitFunctions) {
    uint8_t data[1] = {1};
    buffer_t *buf = buffer_create_from_existing_memory(data, 1);
    if (littleEndian()) {
      EXPECT_EQ(get_bit(buf, 0), 1);
      EXPECT_EQ(get_bit(buf, 1), 0);
      EXPECT_EQ(get_bit(buf, 2), 0);
      EXPECT_EQ(get_bit(buf, 3), 0);
      EXPECT_EQ(get_bit(buf, 4), 0);
      EXPECT_EQ(get_bit(buf, 5), 0);
      EXPECT_EQ(get_bit(buf, 6), 0);
      EXPECT_EQ(get_bit(buf, 7), 0);
    } else {
      EXPECT_EQ(get_bit(buf, 0), 0);
      EXPECT_EQ(get_bit(buf, 1), 0);
      EXPECT_EQ(get_bit(buf, 2), 0);
      EXPECT_EQ(get_bit(buf, 3), 0);
      EXPECT_EQ(get_bit(buf, 4), 0);
      EXPECT_EQ(get_bit(buf, 5), 0);
      EXPECT_EQ(get_bit(buf, 6), 0);
      EXPECT_EQ(get_bit(buf, 7), 1);
    }
  }

  TEST(TestIndex, TestIndexEntry) {
    block_t *block = block_create_random_block_by_type(nano);
    index_entry_t *entry = index_entry_create(block->hash);
    EXPECT_EQ(buffer_compare(entry->hash, block->hash), 0);
    EXPECT_EQ(entry->hash, block->hash);

    for (int i = 0; i < 1000; i++) {
      index_entry_increment(entry);
    }

    EXPECT_EQ(entry->counter.fib, 14);
    EXPECT_EQ(entry->counter.count, 377);
    uint64_t now = (uint64_t) time(NULL);
    index_entry_set_ejection_date(entry, now);
    EXPECT_EQ(entry->ejection_date, now);

    cbor_item_t *cbor = index_entry_to_cbor(entry);
    uint8_t *cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
    struct cbor_load_result result;
    cbor_item_t *cbor2 = cbor_load(cbor_data, cbor_size, &result);
    EXPECT_EQ(result.error.code == CBOR_ERR_NONE, true);
    EXPECT_EQ(cbor_isa_array(cbor2), true);
    index_entry_t *from_cbor = cbor_to_index_entry(cbor2);
    EXPECT_EQ(from_cbor->counter.fib, entry->counter.fib);
    EXPECT_EQ(from_cbor->counter.count, entry->counter.count);
    EXPECT_EQ(from_cbor->ejection_date, from_cbor->ejection_date);
    EXPECT_EQ(buffer_compare(from_cbor->hash, entry->hash), 0);
    EXPECT_EQ(from_cbor->section_id, entry->section_id);
    EXPECT_EQ(from_cbor->section_index, entry->section_index);

    cbor_decref(&cbor);
    cbor_decref(&cbor2);
    free(cbor_data);
    block_destroy(block);
    index_entry_destroy(entry);
    index_entry_destroy(from_cbor);
  }

  void Shutdown(void* ctx) {
    auto pool = (work_pool_t*)ctx;
    platform_signal_condition(&pool->shutdown);
  }
  void ShutdownAborted(void* ctx) {
  }


  TEST(TestIndex, TestIndexFunctions) {
    block_t* block1 = block_create_random_block_by_type(nano);
    block_t* block2 = block_create_random_block_by_type(nano);
    block_t* block3 = block_create_random_block_by_type(nano);
    block_t* block4 = block_create_random_block_by_type(nano);
    block_t* block5 = block_create_random_block_by_type(nano);
    block_t* block6 = block_create_random_block_by_type(nano);
    block_t* block7 = block_create_random_block_by_type(nano);
    block_t* block8 = block_create_random_block_by_type(nano);

    index_entry_t* entry1 = index_entry_create(block1->hash);
    index_entry_t* entry2 = index_entry_create(block2->hash);
    index_entry_t* entry3 = index_entry_create(block3->hash);
    index_entry_t* entry4 = index_entry_create(block4->hash);
    index_entry_t* entry5 = index_entry_create(block5->hash);
    index_entry_t* entry6 = index_entry_create(block6->hash);
    index_entry_t* entry7 = index_entry_create(block7->hash);
    index_entry_t* entry8 = index_entry_create(block8->hash);
    char *location = path_join(".", "block_index");
    work_pool_t* pool= work_pool_create(4);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    uint64_t wait = 200;
    uint64_t max_wait = 5000;
    index_t *index = index_create(3, location, wheel, wait, max_wait);

    index_add(index, entry1);
    index_add(index, entry2);
    index_add(index, entry3);
    index_add(index, entry4);
    index_add(index, entry5);
    index_add(index, entry6);
    index_add(index, entry7);
    index_add(index, entry8);

    index_entry_t* _entry1 = index_get(index, block1->hash);
    index_entry_t* _entry2 = index_get(index, block2->hash);
    index_entry_t* _entry3 = index_get(index, block3->hash);
    index_entry_t* _entry4 = index_get(index, block4->hash);
    index_entry_t* _entry5 = index_get(index, block5->hash);
    index_entry_t* _entry6 = index_get(index, block6->hash);
    index_entry_t* _entry7 = index_get(index, block7->hash);
    index_entry_t* _entry8 = index_get(index, block8->hash);

    EXPECT_EQ(buffer_compare(_entry1->hash, entry1->hash), 0);
    EXPECT_EQ(buffer_compare(_entry2->hash, entry2->hash), 0);
    EXPECT_EQ(buffer_compare(_entry3->hash, entry3->hash), 0);
    EXPECT_EQ(buffer_compare(_entry4->hash, entry4->hash), 0);
    EXPECT_EQ(buffer_compare(_entry5->hash, entry5->hash), 0);
    EXPECT_EQ(buffer_compare(_entry6->hash, entry6->hash), 0);
    EXPECT_EQ(buffer_compare(_entry7->hash, entry7->hash), 0);
    EXPECT_EQ(buffer_compare(_entry8->hash, entry8->hash), 0);

    _entry1 = index_find(index, block1->hash);
    _entry2 = index_find(index, block2->hash);
    _entry3 = index_find(index, block3->hash);
    _entry4 = index_find(index, block4->hash);
    _entry5 = index_find(index, block5->hash);
    _entry6 = index_find(index, block6->hash);
    _entry7 = index_find(index, block7->hash);
    _entry8 = index_find(index, block8->hash);

    EXPECT_EQ(buffer_compare(_entry1->hash, entry1->hash), 0);
    EXPECT_EQ(buffer_compare(_entry2->hash, entry2->hash), 0);
    EXPECT_EQ(buffer_compare(_entry3->hash, entry3->hash), 0);
    EXPECT_EQ(buffer_compare(_entry4->hash, entry4->hash), 0);
    EXPECT_EQ(buffer_compare(_entry5->hash, entry5->hash), 0);
    EXPECT_EQ(buffer_compare(_entry6->hash, entry6->hash), 0);
    EXPECT_EQ(buffer_compare(_entry7->hash, entry7->hash), 0);
    EXPECT_EQ(buffer_compare(_entry8->hash, entry8->hash), 0);

    index_remove(index, block1->hash);
    index_remove(index, block2->hash);
    index_remove(index, block3->hash);
    index_remove(index, block4->hash);
    index_remove(index, block5->hash);
    index_remove(index, block6->hash);
    index_remove(index, block7->hash);
    index_remove(index, block8->hash);


    _entry1 = index_get(index, block1->hash);
    _entry2 = index_get(index, block2->hash);
    _entry3 = index_get(index, block3->hash);
    _entry4 = index_get(index, block4->hash);
    _entry5 = index_get(index, block5->hash);
    _entry6 = index_get(index, block6->hash);
    _entry7 = index_get(index, block7->hash);
    _entry8 = index_get(index, block8->hash);

    EXPECT_TRUE(_entry1 == NULL);
    EXPECT_TRUE(_entry2 == NULL);
    EXPECT_TRUE(_entry3 == NULL);
    EXPECT_TRUE(_entry4 == NULL);
    EXPECT_TRUE(_entry5 == NULL);
    EXPECT_TRUE(_entry6 == NULL);
    EXPECT_TRUE(_entry7 == NULL);
    EXPECT_TRUE(_entry8 == NULL);


    cbor_item_t *cbor = index_to_cbor(index);
    uint8_t *cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
    struct cbor_load_result result;
    cbor_item_t *cbor2 = cbor_load(cbor_data, cbor_size, &result);
    EXPECT_EQ(result.error.code == CBOR_ERR_NONE, true);
    EXPECT_EQ(cbor_isa_array(cbor2), true);
    index_t* from_cbor = cbor_to_index(cbor2, location, wheel, wait, max_wait);
    EXPECT_FALSE(from_cbor == NULL);


    _entry1 = index_get(from_cbor, block1->hash);
    _entry2 = index_get(from_cbor, block2->hash);
    _entry3 = index_get(from_cbor, block3->hash);
    _entry4 = index_get(from_cbor, block4->hash);
    _entry5 = index_get(from_cbor, block5->hash);
    _entry6 = index_get(from_cbor, block6->hash);
    _entry7 = index_get(from_cbor, block7->hash);
    _entry8 = index_get(from_cbor, block8->hash);

    EXPECT_TRUE(_entry1 == NULL);
    EXPECT_TRUE(_entry2 == NULL);
    EXPECT_TRUE(_entry3 == NULL);
    EXPECT_TRUE(_entry4 == NULL);
    EXPECT_TRUE(_entry5 == NULL);
    EXPECT_TRUE(_entry6 == NULL);
    EXPECT_TRUE(_entry7 == NULL);
    EXPECT_TRUE(_entry8 == NULL);

    cbor_decref(&cbor);
    cbor_decref(&cbor2);
    free(cbor_data);

    //hierarchical_timing_wheel_stop(wheel);
    //work_pool_wait_for_idle_signal(pool);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);

    index_entry_destroy(entry1);
    index_entry_destroy(entry2);
    index_entry_destroy(entry3);
    index_entry_destroy(entry4);
    index_entry_destroy(entry5);
    index_entry_destroy(entry6);
    index_entry_destroy(entry7);
    index_entry_destroy(entry8);


    block_destroy(block1);
    block_destroy(block2);
    block_destroy(block3);
    block_destroy(block4);
    block_destroy(block5);
    block_destroy(block6);
    block_destroy(block7);
    block_destroy(block8);

    free(location);
    index_destroy(index);
    index_destroy(from_cbor);
  }
}
