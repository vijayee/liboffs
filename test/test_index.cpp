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
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include <time.h>
#include <cbor.h>
#include "../src/Time/wheel.h"
#include "../src/BlockCache/frand.h"
#include "../src/Util/get_dir.h"
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

  TEST(TestBit, TestBitFunctions) {
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

  TEST(TestIndexEntry, TestIndexEntry) {
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
    uint8_t* cbor_data;
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

  class TestIndex : public testing::Test {
  public:
    block_t* blocks[8];
    index_entry_t* entries[8];
    char* location;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
    uint64_t wait = 200;
    uint64_t max_wait = 5000;
    void SetUp() override {
      for (size_t i = 0; i < 8; i++) {
        buffer_t* buf = buffer_create_from_existing_memory(frand(nano), nano);
        blocks[i] = block_create_by_type(CONSUME(buf, buffer_t), nano);
        entries[i] = index_entry_create(blocks[i]->hash);
      }
      location = path_join(".", "block_index");
      rm_rf(location);
      mkdir_p(location);
      pool = work_pool_create(4);
      work_pool_launch(pool);
      wheel = hierarchical_timing_wheel_create(8, pool);
      hierarchical_timing_wheel_run(wheel);
    }
    void TearDown() override {
      hierarchical_timing_wheel_wait_for_idle_signal(wheel);
      hierarchical_timing_wheel_stop(wheel);
      work_pool_shutdown(pool);
      work_pool_join_all(pool);
      work_pool_destroy(pool);
      hierarchical_timing_wheel_destroy(wheel);
      for (size_t i = 0; i < 8; i++) {
        DESTROY(blocks[i], block);
        DESTROY(entries[i], index_entry);
      }
      free(location);
    }
    void CorruptCRC(int count) {
      char* index_location = path_join(location,"index");
      vec_str_t* files = get_dir(index_location);
      if (files->length > 0) {
        vec_sort(files, _sort_indexes);
        for (size_t i = files->length - 1; ((i >= 0) && (count >= 0)); i--) {
          char* last = files->data[i];
          char* index_file_location = path_join(index_location, last);
          char delims[] = "-";
          char* last_id_str = strtok(last,delims);
          uint64_t last_id = strtoull(last_id_str, NULL, 10);

          char* last_crc_str = strtok(NULL, delims);
          uint64_t last_crc = strtoull(last_crc_str, NULL, 10);
          last_crc++;
          char file[41];
          sprintf(file, "%lu-%lu", last_id, last_crc);
          char* new_location = path_join(index_location, file);
          int result = rename(index_file_location, new_location);
          free(index_file_location);
          free(new_location);
          if (result != 0) {
            throw result;
          }
          count--;
        }
      }
      free(index_location);
      destroy_files(files);
    }
    void CorruptCBOR() {

    }
    void CorruptOrder() {

    }
  };

  TEST_F(TestIndex, TestIndexFunctions) {
    int error_code;
    index_t* index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 0; i < 8; i++) {
      index_add(index, entries[i]);
    }
    index_entry_t* _entries[8];

    for (size_t i = 0; i < 8; i++) {
      _entries[i] = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
      EXPECT_EQ(buffer_compare(_entries[i]->hash, entries[i]->hash), 0);
      DESTROY(_entries[i], index_entry);
    }

    for (size_t i = 0; i < 8; i++) {
      _entries[i] = REFERENCE(index_find(index, blocks[i]->hash), index_entry_t);
      EXPECT_EQ(buffer_compare(_entries[i]->hash, entries[i]->hash), 0);
      DESTROY(_entries[i], index_entry);
    }


    cbor_item_t *cbor = index_to_cbor(index);
    uint8_t *cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
    struct cbor_load_result result;
    cbor_item_t *cbor2 = cbor_load(cbor_data, cbor_size, &result);
    EXPECT_EQ(result.error.code == CBOR_ERR_NONE, true);
    EXPECT_EQ(cbor_isa_array(cbor2), true);
    index_destroy(index);
    index_t* from_cbor = cbor_to_index(cbor2, location, wheel, wait, max_wait);
    EXPECT_FALSE(from_cbor == NULL);

    for (size_t i = 0; i < 8; i++) {
      _entries[i] = REFERENCE(index_find(from_cbor, blocks[i]->hash), index_entry_t);
      EXPECT_EQ(buffer_compare(_entries[i]->hash, entries[i]->hash), 0);
      DESTROY(_entries[i], index_entry);
    }


    for (size_t i = 0; i < 8; i++) {
      index_remove(from_cbor, blocks[i]->hash);
      _entries[i] = REFERENCE(index_get(from_cbor, blocks[i]->hash), index_entry_t);
      EXPECT_TRUE(_entries[i] == NULL);
    }

    cbor_decref(&cbor);
    cbor_decref(&cbor2);
    free(cbor_data);
    DESTROY(from_cbor, index);

    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    DESTROY(index, index);

  }
  TEST_F(TestIndex, TestIndexRecovery) {
    int error_code;
    index_t* index;
    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wheel, wait, max_wait, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_add(index, entries[i]);
      DESTROY(index, index);
    }
    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_add(index, entries[i]);
    }
    DESTROY(index, index);

    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wheel, wait, max_wait, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_entry_t* entry = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
      DESTROY(entry, index_entry);
      DESTROY(index, index);
    }
    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_entry_t* entry = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
      DESTROY(entry, index_entry);
    }
    DESTROY(index, index);

    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    cbor_item_t *cbor = index_to_cbor(index);
    DESTROY(index, index);

    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wheel, wait, max_wait, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_remove(index, entries[i]->hash);
      DESTROY(index, index);
    }
    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_remove(index, entries[i]->hash);
    }
    DESTROY(index, index);


    CorruptCRC(5);

    index = index_create(25, location, wheel, wait, max_wait, &error_code);
    uint64_t index_crc;
    EXPECT_EQ(index_to_crc(index, &index_crc), 0);
    DESTROY(index, index);
    index_t* from_cbor = cbor_to_index(cbor, location, wheel, wait, max_wait);
    cbor_decref(&cbor);
    uint64_t cbor_crc;
    EXPECT_EQ(index_to_crc(from_cbor, &cbor_crc), 0);
    DESTROY(from_cbor, index);
    EXPECT_EQ(cbor_crc, index_crc);
  }
}
