//
// Created by victor on 7/28/25.
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
#include "Platform/platform.h"
#include "../src/Util/atomic_compat.h"
#include <time.h>
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/BlockCache/section.h"
#include "../src/BlockCache/sections.h"
#include "../src/Util/allocator.h"
}

/* ---- Completion actor state for section async tests ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  section_write_result_payload_t write_result;
  buffer_t* read_buffer;
  int deallocate_result;
} section_completion_state_t;

static void section_completion_dispatch(void* state, message_t* msg) {
  section_completion_state_t* cs = (section_completion_state_t*)state;
  switch (msg->type) {
    case SECTION_WRITE_RESULT: {
      section_write_result_payload_t* r = (section_write_result_payload_t*)msg->payload;
      cs->write_result = *r;
      break;
    }
    case SECTION_READ_RESULT: {
      section_read_result_payload_t* r = (section_read_result_payload_t*)msg->payload;
      cs->read_buffer = r->data;
      r->data = NULL;
      break;
    }
    case SECTION_DEALLOCATE_RESULT: {
      section_deallocate_result_payload_t* r = (section_deallocate_result_payload_t*)msg->payload;
      cs->deallocate_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Helper: async section_write, returns 0 on success */
static int section_write_sync(section_t* section, buffer_t* data, size_t* out_index, uint8_t* out_full) {
  section_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, section_completion_dispatch, NULL);

  section_write(section, data, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  if (out_index) *out_index = cs.write_result.index;
  if (out_full) *out_full = cs.write_result.full;
  actor_destroy(&comp);
  return cs.write_result.result;
}

/* Helper: async section_read, returns buffer or NULL */
static buffer_t* section_read_sync(section_t* section, size_t index) {
  section_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, section_completion_dispatch, NULL);

  section_read(section, index, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  actor_destroy(&comp);
  return cs.read_buffer;
}

/* Helper: async section_deallocate, returns 0 on success */
static int section_deallocate_sync(section_t* section, size_t index) {
  section_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, section_completion_dispatch, NULL);

  section_deallocate(section, index, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  actor_destroy(&comp);
  return cs.deallocate_result;
}

/* ---- Completion actor state for sections async tests ---- */

typedef struct {
  ATOMIC(uint8_t) done;
  size_t section_id;
  size_t section_index;
  buffer_t* read_buffer;
  int write_result;
  int deallocate_result;
} sections_completion_state_t;

static void sections_completion_dispatch(void* state, message_t* msg) {
  sections_completion_state_t* cs = (sections_completion_state_t*)state;
  switch (msg->type) {
    case SECTIONS_WRITE_RESULT: {
      sections_write_result_payload_t* r = (sections_write_result_payload_t*)msg->payload;
      cs->write_result = r->result;
      cs->section_id = r->section_id;
      cs->section_index = r->section_index;
      break;
    }
    case SECTIONS_READ_RESULT: {
      sections_read_result_payload_t* r = (sections_read_result_payload_t*)msg->payload;
      cs->read_buffer = r->data;
      r->data = NULL;
      break;
    }
    case SECTIONS_DEALLOCATE_RESULT: {
      sections_deallocate_result_payload_t* r = (sections_deallocate_result_payload_t*)msg->payload;
      cs->deallocate_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Helper: async sections_write, returns 0 on success */
static int sections_write_sync(sections_t* sections, buffer_t* data, size_t* out_section_id, size_t* out_section_index) {
  sections_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, sections_completion_dispatch, NULL);

  sections_write(sections, data, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&sections->actor, ACTOR_BATCH_SIZE);
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  if (out_section_id) *out_section_id = cs.section_id;
  if (out_section_index) *out_section_index = cs.section_index;
  actor_destroy(&comp);
  return cs.write_result;
}

/* Helper: async sections_read, returns buffer or NULL */
static buffer_t* sections_read_sync(sections_t* sections, size_t section_id, size_t section_index) {
  sections_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, sections_completion_dispatch, NULL);

  sections_read(sections, section_id, section_index, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&sections->actor, ACTOR_BATCH_SIZE);
    /* sections may delegate to a section actor */
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  actor_destroy(&comp);
  return cs.read_buffer;
}

/* Helper: async sections_deallocate, returns 0 on success */
static int sections_deallocate_sync(sections_t* sections, size_t section_id, size_t section_index) {
  sections_completion_state_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, sections_completion_dispatch, NULL);

  sections_deallocate(sections, section_id, section_index, &comp);

  while (!ATOMIC_LOAD(&cs.done)) {
    actor_run(&sections->actor, ACTOR_BATCH_SIZE);
    actor_run(&comp, ACTOR_BATCH_SIZE);
  }

  actor_destroy(&comp);
  return cs.deallocate_result;
}

class TestSection : public testing::Test {
public:
  char* section_location;
  char* meta_location;
  void SetUp() override {
    section_location = path_join("/tmp", "sections");
    meta_location = path_join("/tmp", "meta");
    rm_rf(section_location);
    rm_rf(meta_location);
  }
};


TEST_F(TestSection, TestSectionFunction) {
  size_t block_count = 20;
  block_t* blocks[block_count];
  index_entry_t* entries[block_count];
  uint8_t full;
  for (size_t i = 0; i < block_count; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
  }

  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 20, 4000, mini);
  for (size_t i = 0; i < block_count; i++) {
    size_t section_index = 0;
    int result = section_write_sync(section, blocks[i]->data, &section_index, &full);
    EXPECT_EQ(result, 0);
    if (result == 0) {
      index_entry_t* entry = index_entry_create(blocks[i]->hash);
      entry->section_id = 4000;
      entry->section_index = section_index;
      entries[i] = entry;
    } else {

      section_destroy(section);
      free(meta_location);
      free(section_location);
      for (size_t i = 0; i < block_count; i++) {
        block_destroy(blocks[i]);
      }
      GTEST_SKIP();
    }
  }

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    buffer_t* buf = section_read_sync(section, entry->section_index);
    EXPECT_NE(buf, (buffer_t*) NULL);
    EXPECT_EQ(buffer_compare(buf, blocks[i]->data), 0);
    refcounter_yield((refcounter_t*) buf);
    block_t* block = block_create_existing_data(buf);
    EXPECT_EQ(buffer_compare(block->hash, blocks[i]->hash), 0);
    EXPECT_EQ(buffer_compare(block->hash, entry->hash), 0);
    block_destroy(block);
  }

  section_destroy(section);
  section = section_create(section_location, meta_location, 20, 4000, mini);

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    buffer_t* buf = section_read_sync(section, entry->section_index);
    EXPECT_NE(buf, (buffer_t*) NULL);
    EXPECT_EQ(buffer_compare(buf, blocks[i]->data), 0);
    refcounter_yield((refcounter_t*) buf);
    block_t* block = block_create_existing_data(buf);
    EXPECT_EQ(buffer_compare(block->hash, blocks[i]->hash), 0);
    EXPECT_EQ(buffer_compare(block->hash, entry->hash), 0);
    block_destroy(block);
  }

  for (size_t i = 0; i < block_count; i++) {
    index_entry_t* entry =  entries[i];
    int result = section_deallocate_sync(section, entry->section_index);
    EXPECT_EQ(result, 0);
  }

  for (size_t i = 0; i < block_count; i++) {
    size_t section_index;
    int result = section_write_sync(section, blocks[i]->data, &section_index, &full);
    EXPECT_EQ(result, 0);
  }
  int result = section_deallocate_sync(section, entries[10]->section_index);
  EXPECT_EQ(result, 0);
  result = section_deallocate_sync(section, entries[11]->section_index);
  EXPECT_EQ(result, 0);
  result = section_deallocate_sync(section, entries[12]->section_index);
  EXPECT_EQ(result, 0);

  result = section_deallocate_sync(section, entries[2]->section_index);
  EXPECT_EQ(result, 0);

  result = section_deallocate_sync(section, entries[18]->section_index);
  EXPECT_EQ(result, 0);

  result = section_deallocate_sync(section, entries[19]->section_index);
  EXPECT_EQ(result, 0);

  result = section_write_sync(section, blocks[12]->data, &entries[12]->section_index, &full);
  EXPECT_EQ(result, 0);

  EXPECT_EQ(entries[12]->section_index, entries[2]->section_index);

  result = section_write_sync(section, blocks[18]->data, &entries[18]->section_index, &full);
  EXPECT_EQ(result, 0);

  EXPECT_EQ(entries[18]->section_index, entries[10]->section_index);


  section_destroy(section);
  free(meta_location);
  free(section_location);

  for (size_t i = 0; i < block_count; i++) {
    index_entry_destroy(entries[i]);
    block_destroy(blocks[i]);
  }
}

/* ---- Async actor-based section test ---- */

TEST_F(TestSection, TestSectionAsyncWrite) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 20, 5000, mini);

  /* Create completion actor for async results */
  section_completion_state_t completion_state;
  memset(&completion_state, 0, sizeof(completion_state));
  actor_t completion_actor;
  actor_init(&completion_actor, &completion_state, section_completion_dispatch, NULL);

  /* Write a block asynchronously */
  block_t* block = block_create_random_block_by_type(mini);
  section_write_payload_t* payload = (section_write_payload_t*)get_clear_memory(sizeof(section_write_payload_t));
  payload->data = block->data;
  payload->reply_to = &completion_actor;

  message_t msg;
  msg.type = SECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&section->actor, &msg);

  /* Process both actors until completion */
  while (!ATOMIC_LOAD(&completion_state.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&completion_actor, ACTOR_BATCH_SIZE);
  }

  EXPECT_EQ(completion_state.write_result.result, 0);
  EXPECT_EQ(completion_state.write_result.full, 0);
  size_t written_index = completion_state.write_result.index;

  /* Read back the block asynchronously */
  ATOMIC_STORE(&completion_state.done, 0);
  completion_state.read_buffer = NULL;
  section_read_payload_t* read_payload = (section_read_payload_t*)get_clear_memory(sizeof(section_read_payload_t));
  read_payload->index = written_index;
  read_payload->reply_to = &completion_actor;

  message_t read_msg;
  read_msg.type = SECTION_READ;
  read_msg.payload = read_payload;
  read_msg.payload_destroy = free;

  actor_send(&section->actor, &read_msg);

  while (!ATOMIC_LOAD(&completion_state.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&completion_actor, ACTOR_BATCH_SIZE);
  }

  ASSERT_NE(completion_state.read_buffer, (buffer_t*)NULL);
  EXPECT_EQ(buffer_compare(completion_state.read_buffer, block->data), 0);

  /* Deallocate the block asynchronously */
  ATOMIC_STORE(&completion_state.done, 0);
  section_deallocate_payload_t* dealloc_payload = (section_deallocate_payload_t*)get_clear_memory(sizeof(section_deallocate_payload_t));
  dealloc_payload->index = written_index;
  dealloc_payload->reply_to = &completion_actor;

  message_t dealloc_msg;
  dealloc_msg.type = SECTION_DEALLOCATE;
  dealloc_msg.payload = dealloc_payload;
  dealloc_msg.payload_destroy = free;

  actor_send(&section->actor, &dealloc_msg);

  while (!ATOMIC_LOAD(&completion_state.done)) {
    actor_run(&section->actor, ACTOR_BATCH_SIZE);
    actor_run(&completion_actor, ACTOR_BATCH_SIZE);
  }

  EXPECT_EQ(completion_state.deallocate_result, 0);

  /* Cleanup */
  buffer_destroy(completion_state.read_buffer);
  block_destroy(block);
  actor_destroy(&completion_actor);
  section_destroy(section);
  free(meta_location);
  free(section_location);
}

class TestSectionsLRU : public testing::Test {
public:
  size_t size = 5;
  size_t overage = 3;
  section_t* sections[8];
  size_t id = 0;
  char* section_location;
  char* meta_location;
  void SetUp() override {
    section_location = path_join("/tmp", "sections");
    meta_location = path_join("/tmp", "meta");
    rm_rf(section_location);
    rm_rf(meta_location);
    mkdir_p(section_location);
    mkdir_p(meta_location);

    for (size_t i = 0; i < 8; i++) {
      sections[i] = section_create(section_location, meta_location, 20, id, mini);
      id++;
    }
  }
  void TearDown() override {
    free(meta_location);
    free(section_location);

    for (size_t i = 0; i < 8; i++) {
      section_destroy(sections[i]);
    }
  }
};

TEST_F(TestSectionsLRU, TestSectionLRUInsertionDeletion) {
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i <  overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}

TEST_F(TestSectionsLRU, TestSectionLRUSize1) {
  size = 1;
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i < overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}

TEST_F(TestSectionsLRU, TestSectionLRUSize0) {
  size = 0;
  sections_lru_cache_t* lru = sections_lru_cache_create(size);
  for (size_t i = 0; i < (size + overage); i++) {
    sections_lru_cache_put(lru, sections[i]);
  }
  for (size_t i = 0; i < overage; i++) {
    EXPECT_EQ(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_NE(sections_lru_cache_get(lru, sections[i]->id) == NULL, true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), true);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    sections_lru_cache_delete(lru, sections[i]->id);
  }
  for (size_t i = overage; i < (size + overage); i++) {
    EXPECT_EQ(sections_lru_cache_contains(lru, sections[i]->id), false);
  }

  sections_lru_cache_destroy(lru);
}

class TestRoundRobin : public testing::Test {
public:
  char* robin_location;
  timer_actor_t* timer_actor_inst;
  round_robin_t* robin;
  void SetUp() override {
    robin_location = path_join("/tmp", "robin");
    rm_rf(robin_location);
    timer_actor_inst = timer_actor_create();
    mkdir_p(robin_location);
  }
  void TearDown() override {
    round_robin_destroy(robin);
    free(robin_location);
    timer_actor_destroy(timer_actor_inst);
  }
};

TEST_F(TestRoundRobin, TestRoundRobinFunctions) {
  robin = round_robin_create(path_join(robin_location,".robin"), timer_actor_inst, NULL, 5, 5000);
  size_t size = 6;
  for (size_t i = 0; i < size; i++) {
    round_robin_add(robin, i);
  }
  EXPECT_EQ(robin->size, size);
  size_t id;
  for (size_t i= 0; i < (size * 10); i++) {
    int ok = round_robin_next(robin, &id);
    EXPECT_EQ(ok, 1);
    EXPECT_EQ(id, i % size);
  }
  for (size_t i = 0; i < size; i++) {
    round_robin_remove(robin, i);
  }
  EXPECT_EQ(robin->size, 0);
  for (size_t i= 0; i < (size * 10); i++) {
    int ok = round_robin_next(robin, &id);
    EXPECT_EQ(ok, 0);
  }
}


class TestSections : public testing::Test {
public:
  block_size_e block_type = mini;
  block_t* blocks[25];
  index_entry_t* entries[25];
  size_t cache_size = 5;
  size_t size = 5;
  size_t max_tuple_size = 5;
  size_t id = 0;
  uint64_t wait = 5;
  uint64_t max_wait = 5000;
  char* path;
  timer_actor_t* timer_actor_inst;
  sections_t* sections = NULL;
  void SetUp() override {
    path = path_join("/tmp", "sections");
    rm_rf(path);
    timer_actor_inst = timer_actor_create();
    mkdir_p(path);
    for (size_t i = 0; i < 25; i++) {
      blocks[i] = block_create_random_block_by_type(block_type);
      entries[i] = index_entry_create(blocks[i]->hash);
    }
    sections = sections_create(path, size, cache_size, max_tuple_size, block_type, timer_actor_inst, NULL, wait, max_wait);
  }
  void TearDown() override {
    timer_actor_destroy(timer_actor_inst);
    for (size_t i = 0; i < 25; i++) {
      block_destroy(blocks[i]);
      index_entry_destroy(entries[i]);
    }
    free(path);
    sections_destroy(sections);
  }
};

TEST_F(TestSections, SectionsFunctions) {
  size_t section_index;
  size_t section_id;

  for (size_t i = 0; i < 25; i++) {
    uint8_t result = sections_write_sync(sections, blocks[i]->data, &section_id, &section_index);
    EXPECT_EQ(result, 0);
    if (result == 0) {
      entries[i]->section_id = section_id;
      entries[i]->section_index = section_index;
    } else {
      GTEST_FATAL_FAILURE_("Failed to write to sections");
    }
  }
  for (size_t i = 0; i < 25; i++) {
    buffer_t* data = sections_read_sync(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_EQ(data == NULL, false);
    if (data != NULL) {
      EXPECT_EQ(buffer_compare(data, blocks[i]->data) == 0, true);
      buffer_destroy(data);
    }
  }
  for (size_t i = 0; i < 25; i++) {
    int result = sections_deallocate_sync(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_EQ(result, 0);
  }
  for (size_t i = 0; i < 25; i++) {
    int result = sections_deallocate_sync(sections, entries[i]->section_id, entries[i]->section_index);
    EXPECT_NE(result, 0);
  }
}

/* ---- Bitmap-specific tests ---- */

TEST_F(TestSection, TestBitmapAllocationOrder) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 32, 6000, mini);

  uint8_t full;
  for (size_t i = 0; i < 32; i++) {
    block_t* block = block_create_random_block_by_type(mini);
    size_t index;
    int result = section_write_sync(section, block->data, &index, &full);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(index, i);
    block_destroy(block);
  }
  EXPECT_EQ(full, 1);

  section_destroy(section);
  free(meta_location);
  free(section_location);
}

TEST_F(TestSection, TestBitmapFullDetection) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 5, 7000, mini);

  EXPECT_EQ(section_full(section), 0);

  uint8_t full;
  block_t* blocks[5];
  size_t indices[5];
  for (size_t i = 0; i < 5; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
    int result = section_write_sync(section, blocks[i]->data, &indices[i], &full);
    EXPECT_EQ(result, 0);
  }
  EXPECT_EQ(full, 1);
  EXPECT_EQ(section_full(section), 1);

  section_deallocate_sync(section, indices[2]);
  EXPECT_EQ(section_full(section), 0);

  size_t new_index;
  int result = section_write_sync(section, blocks[2]->data, &new_index, &full);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(new_index, indices[2]);
  EXPECT_EQ(full, 1);

  for (size_t i = 0; i < 5; i++) {
    block_destroy(blocks[i]);
  }
  section_destroy(section);
  free(meta_location);
  free(section_location);
}

TEST_F(TestSection, TestBitmapDeallocateDoubleFree) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 10, 8000, mini);

  uint8_t full;
  block_t* block = block_create_random_block_by_type(mini);
  size_t index;
  int result = section_write_sync(section, block->data, &index, &full);
  EXPECT_EQ(result, 0);

  result = section_deallocate_sync(section, index);
  EXPECT_EQ(result, 0);

  result = section_deallocate_sync(section, index);
  EXPECT_NE(result, 0);

  block_destroy(block);
  section_destroy(section);
  free(meta_location);
  free(section_location);
}

TEST_F(TestSection, TestBitmapPersistenceRoundTrip) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 10, 9000, mini);

  uint8_t full;
  block_t* blocks[7];
  size_t indices[7];
  for (size_t i = 0; i < 7; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
    int result = section_write_sync(section, blocks[i]->data, &indices[i], &full);
    EXPECT_EQ(result, 0);
  }

  section_deallocate_sync(section, indices[1]);
  section_deallocate_sync(section, indices[4]);
  section_save_meta(section);

  section_destroy(section);
  section = section_create(section_location, meta_location, 10, 9000, mini);

  size_t new_index1, new_index2;
  int result = section_write_sync(section, blocks[1]->data, &new_index1, &full);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(new_index1 == indices[1] || new_index1 == indices[4]);

  result = section_write_sync(section, blocks[4]->data, &new_index2, &full);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(new_index2 == indices[1] || new_index2 == indices[4]);

  for (size_t i = 0; i < 7; i++) {
    block_destroy(blocks[i]);
  }
  section_destroy(section);
  free(meta_location);
  free(section_location);
}

/* ---- Actor queue tests ---- */

TEST_F(TestSection, TestActorMultipleMessagesQueued) {
  mkdir_p(section_location);
  mkdir_p(meta_location);
  section_t* section = section_create(section_location, meta_location, 10, 10000, mini);

  section_completion_state_t completion_state;

  /* Send 5 write messages */
  block_t* blocks[5];
  for (int i = 0; i < 5; i++) {
    blocks[i] = block_create_random_block_by_type(mini);
  }

  for (int i = 0; i < 5; i++) {
    size_t index;
    uint8_t full;
    int result = section_write_sync(section, blocks[i]->data, &index, &full);
    EXPECT_EQ(result, 0);
  }

  EXPECT_EQ(section_full(section), 0);

  /* Read back the blocks to verify writes succeeded */
  for (int i = 0; i < 5; i++) {
    buffer_t* buf = section_read_sync(section, (size_t)i);
    ASSERT_NE(buf, (buffer_t*)NULL);
    EXPECT_EQ(buffer_compare(buf, blocks[i]->data), 0);
    buffer_destroy(buf);
    block_destroy(blocks[i]);
  }

  section_destroy(section);
  free(meta_location);
  free(section_location);
}