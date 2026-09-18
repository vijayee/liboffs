#include <gtest/gtest.h>
#include <string.h>
#include <cstdio>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include "../src/BlockCache/index.h"
#include "../src/BlockCache/block_cache.h"
#include "../src/BlockCache/ephemeral_registry.h"
#include "../src/Bloom/elastic_bloom_filter.h"
#include "../src/Bloom/attenuated_bloom_filter.h"
#include "../src/BlockCache/sections.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Util/get_dir.h"
#include "../src/Platform/platform_file.h"
#include "../src/Configuration/config.h"
#include "../src/Timer/timer_actor.h"
#include "../src/Actor/actor.h"
#include "../src/Actor/message.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Platform/platform_time.h"
#include "../src/Util/error.h"
#include "../src/Streams/stream.h"
#include "../src/OFFStreams/tuple.h"
#include "../src/OFFStreams/tuple_cache.h"
#include "../src/OFFStreams/block_recipe.h"
#include "../src/OFFStreams/writeable_off_stream.h"
#include "../src/OFFStreams/writeable_descriptor.h"
#include "../src/OFFStreams/representation_actor.h"
#include <cbor.h>
}

/* Legacy (pre-ephemeral) snapshot CRC entry point under test — declared
   here rather than in index.h because it is an internal migration helper
   exercised directly by the legacy-acceptance test below. */
extern "C" int _index_to_crc_ex(index_t* index, uint64_t* crc, int legacy);

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
/* ---- WAL 'm' metadata record ---- */

/* Persist ephemeral/pin counts via index_write_entry_metadata ('m' WAL
   records), roll a snapshot, and verify the counts survive a full
   destroy/re-create cycle. */
TEST(TestEphemeralIndex, WalMetadataRecordRoundTrip) {
  char* location = path_join("/tmp", "ephemeral_metadata_roundtrip");
  rm_rf(location);
  mkdir_p(location);
  int error_code = 0;

  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);
  index_entry_t* entry = index_entry_create(hash);

  index_t* index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_add(index, CONSUME(entry, index_entry_t));

  index_entry_t* held = REFERENCE(index_find(index, hash), index_entry_t);
  ASSERT_NE(held, nullptr);
  held->ephemeral_count = 2;
  index_write_entry_metadata(index, held);
  held->pin_count = 1;
  index_write_entry_metadata(index, held);
  DESTROY(held, index_entry);

  index_debounce(index);
  index_sync(index);
  DESTROY(index, index);

  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(error_code, 0);
  index_entry_t* found = REFERENCE(index_find(reloaded, hash), index_entry_t);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 2u);
  EXPECT_EQ(found->pin_count, 1u);
  DESTROY(found, index_entry);
  DESTROY(reloaded, index);

  DESTROY(hash, buffer);
  rm_rf(location);
  free(location);
}

/* Replay path for the 'm' record: hand-write an 'm' record into the live
   WAL (id 2) as if a prior session had set the metadata and crashed
   before its snapshot, then verify the reloaded index applies the two
   metadata fields from the WAL. */
TEST(TestEphemeralIndex, WalMetadataRecordReplayedFromWal) {
  char* location = path_join("/tmp", "ephemeral_metadata_replay");
  rm_rf(location);
  mkdir_p(location);
  int error_code = 0;

  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);
  index_entry_t* entry = index_entry_create(hash);

  /* Phase 1: snapshot the entry with zeroed counts, rolling the live WAL
     forward to id 2 (mirrors index_destroy's debounce rollover). */
  index_t* index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_add(index, CONSUME(entry, index_entry_t));
  DESTROY(index, index);

  /* Phase 2: write an 'm' record carrying ephemeral=2/pin=1 into WAL 2
     using the same framing wal_write applies at the index write sites. */
  index_entry_t* meta = index_entry_from(hash, 0, 0, 0,
                                         fibonacci_hit_counter_create(), 2, 1);
  cbor_item_t* cbor_entry = index_entry_to_cbor(meta);
  ASSERT_NE(cbor_entry, nullptr);
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor_entry, &cbor_data, &cbor_size);
  EXPECT_EQ(cbor_size, 86u) << "'m' records carry the full 7-element entry CBOR";
  buffer_t* payload = buffer_create_from_existing_memory(cbor_data, cbor_size);
  wal_t* wal = wal_create(location, 2);
  ASSERT_NE(wal, nullptr);
  wal_write(wal, metadata, payload);
  EXPECT_EQ(wal_sync(wal), 0);
  wal_destroy(wal);
  buffer_destroy(payload);
  cbor_decref(&cbor_entry);
  index_entry_destroy(meta);

  /* Phase 3: reopen. The snapshot holds zeroed counts; the 'm' record in
     the live WAL must replay onto the entry. */
  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  index_entry_t* found = REFERENCE(index_find(reloaded, hash), index_entry_t);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 2u);
  EXPECT_EQ(found->pin_count, 1u);
  DESTROY(found, index_entry);
  DESTROY(reloaded, index);

  DESTROY(hash, buffer);
  rm_rf(location);
  free(location);
}

/* wal_write must append at the end of a lazily opened file: a second wal_t
   opening a non-empty (crash-recovered) WAL would otherwise overwrite the
   recovered records from offset 0. Writes one 'm' record per session and
   verifies both replay, in order. */
TEST(TestEphemeralIndex, WalWriteAppendsToRecoveredFile) {
  char* location = path_join("/tmp", "ephemeral_wal_append");
  rm_rf(location);
  mkdir_p(location);

  block_t* first_block = block_create_random_block_by_type(standard);
  buffer_t* first_hash = (buffer_t*)refcounter_reference((refcounter_t*)first_block->hash);
  block_destroy(first_block);
  block_t* second_block = block_create_random_block_by_type(standard);
  buffer_t* second_hash = (buffer_t*)refcounter_reference((refcounter_t*)second_block->hash);
  block_destroy(second_block);

  /* Session 1: write one 'm' record, then "crash" (destroy without sync
     state beyond wal_sync, exactly like a prior session would leave on
     disk). */
  index_entry_t* first_meta = index_entry_from(first_hash, 0, 0, 0,
                                               fibonacci_hit_counter_create(), 1, 0);
  cbor_item_t* first_cbor = index_entry_to_cbor(first_meta);
  ASSERT_NE(first_cbor, nullptr);
  uint8_t* first_data;
  size_t first_size;
  cbor_serialize_alloc(first_cbor, &first_data, &first_size);
  buffer_t* first_payload = buffer_create_from_existing_memory(first_data, first_size);
  wal_t* first_wal = wal_create(location, 2);
  ASSERT_NE(first_wal, nullptr);
  wal_write(first_wal, metadata, first_payload);
  EXPECT_EQ(wal_sync(first_wal), 0);
  wal_destroy(first_wal);
  buffer_destroy(first_payload);
  cbor_decref(&first_cbor);
  index_entry_destroy(first_meta);

  /* Session 2: a fresh wal_t lazily opens the SAME non-empty file. */
  index_entry_t* second_meta = index_entry_from(second_hash, 0, 0, 0,
                                                fibonacci_hit_counter_create(), 2, 0);
  cbor_item_t* second_cbor = index_entry_to_cbor(second_meta);
  ASSERT_NE(second_cbor, nullptr);
  uint8_t* second_data;
  size_t second_size;
  cbor_serialize_alloc(second_cbor, &second_data, &second_size);
  buffer_t* second_payload = buffer_create_from_existing_memory(second_data, second_size);
  wal_t* second_wal = wal_create(location, 2);
  ASSERT_NE(second_wal, nullptr);
  wal_write(second_wal, metadata, second_payload);
  EXPECT_EQ(wal_sync(second_wal), 0);
  wal_destroy(second_wal);
  buffer_destroy(second_payload);
  cbor_decref(&second_cbor);
  index_entry_destroy(second_meta);

  /* Replay: both records must come back, first one first, and then the
     file must end. Without the append fix the second write overwrote the
     first, so the second read would hit end-of-file. */
  wal_t* reader = wal_create(location, 2);
  ASSERT_NE(reader, nullptr);
  wal_type_e record_type = addition;
  buffer_t* record_data = NULL;
  uint64_t cursor = 0;
  int32_t wal_size = 0;

  EXPECT_EQ(wal_read(reader, &record_type, &record_data, &cursor, &wal_size), 0);
  ASSERT_NE(record_data, nullptr);
  EXPECT_EQ(record_type, metadata);
  {
    struct cbor_load_result load_result;
    cbor_item_t* record_cbor = cbor_load(record_data->data, record_data->size, &load_result);
    ASSERT_NE(record_cbor, nullptr);
    EXPECT_TRUE(load_result.read == record_data->size);
    index_entry_t* record_entry = cbor_to_index_entry(record_cbor);
    ASSERT_NE(record_entry, nullptr);
    EXPECT_EQ(buffer_compare(record_entry->hash, first_hash), 0);
    EXPECT_EQ(record_entry->ephemeral_count, 1u);
    index_entry_destroy(record_entry);
    cbor_decref(&record_cbor);
  }
  buffer_destroy(record_data);
  record_data = NULL;

  EXPECT_EQ(wal_read(reader, &record_type, &record_data, &cursor, &wal_size), 0);
  ASSERT_NE(record_data, nullptr);
  EXPECT_EQ(record_type, metadata);
  {
    struct cbor_load_result load_result;
    cbor_item_t* record_cbor = cbor_load(record_data->data, record_data->size, &load_result);
    ASSERT_NE(record_cbor, nullptr);
    index_entry_t* record_entry = cbor_to_index_entry(record_cbor);
    ASSERT_NE(record_entry, nullptr);
    EXPECT_EQ(buffer_compare(record_entry->hash, second_hash), 0);
    EXPECT_EQ(record_entry->ephemeral_count, 2u);
    index_entry_destroy(record_entry);
    cbor_decref(&record_cbor);
  }
  buffer_destroy(record_data);
  record_data = NULL;

  /* Exactly two records — the next read is end-of-file. */
  EXPECT_NE(wal_read(reader, &record_type, &record_data, &cursor, &wal_size), 0);
  EXPECT_EQ(record_data, nullptr);
  wal_destroy(reader);

  DESTROY(first_hash, buffer);
  DESTROY(second_hash, buffer);
  rm_rf(location);
  free(location);
}

/* Legacy WAL framing: an 'a' record written by an older binary has a
   78-byte payload (5-element entry CBOR). wal_read must detect the
   legacy framing (first 86-byte attempt fails, 78-byte retry verifies,
   detection is sticky) and _index_replay_wal must apply the entry with
   zeroed ephemeral/pin counts. */
TEST(TestEphemeralIndex, LegacyWalFramingReplaysOldEntryFormat) {
  char* location = path_join("/tmp", "ephemeral_legacy_wal");
  rm_rf(location);
  mkdir_p(location);
  int error_code = 0;

  block_t* seed_block = block_create_random_block_by_type(standard);
  buffer_t* seed_hash = (buffer_t*)refcounter_reference((refcounter_t*)seed_block->hash);
  block_destroy(seed_block);
  index_entry_t* seed_entry = index_entry_create(seed_hash);

  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);

  /* Phase 1: snapshot with an unrelated seed entry so reopen takes the
     happy path and replays the live WAL (id 2). */
  index_t* index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_add(index, CONSUME(seed_entry, index_entry_t));
  DESTROY(index, index);

  /* Phase 2: hand-write a legacy 78-byte 'a' record into WAL 2. The old
     format serializes [counter(16) + hash bytestring(34) + three
     uint64s(27)] under a 5-element array header(1) = 78 bytes. */
  fibonacci_hit_counter_t counter = fibonacci_hit_counter_create();
  cbor_item_t* legacy_entry = cbor_new_definite_array(5);
  (void)cbor_array_push(legacy_entry, cbor_move(fibonacci_hit_counter_to_cbor(&counter)));
  (void)cbor_array_push(legacy_entry, cbor_move(buffer_to_cbor(hash)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  uint8_t* legacy_data;
  size_t legacy_size;
  cbor_serialize_alloc(legacy_entry, &legacy_data, &legacy_size);
  ASSERT_EQ(legacy_size, 78u) << "legacy entry payload must be exactly 78 bytes";
  buffer_t* payload = buffer_create_from_existing_memory(legacy_data, legacy_size);
  wal_t* wal = wal_create(location, 2);
  ASSERT_NE(wal, nullptr);
  wal_write(wal, addition, payload);
  EXPECT_EQ(wal_sync(wal), 0);
  wal_destroy(wal);
  buffer_destroy(payload);
  cbor_decref(&legacy_entry);

  /* Phase 3: reopen — snapshot loads, the legacy record in WAL 2 must
     replay with zeroed ephemeral/pin counts. */
  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  index_entry_t* found = REFERENCE(index_find(reloaded, hash), index_entry_t);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 0u);
  EXPECT_EQ(found->pin_count, 0u);
  DESTROY(found, index_entry);
  index_entry_t* seed_found = REFERENCE(index_find(reloaded, seed_hash), index_entry_t);
  ASSERT_NE(seed_found, nullptr);
  DESTROY(seed_found, index_entry);
  DESTROY(reloaded, index);

  DESTROY(seed_hash, buffer);
  DESTROY(hash, buffer);
  rm_rf(location);
  free(location);
}

/* Legacy snapshot acceptance: a snapshot whose entries are old-format
   5-element CBOR arrays, named with the legacy (pre-ephemeral) CRC, must
   load with zeroed ephemeral/pin counts instead of being rejected. The
   legacy CRC for the filename is computed with the _index_to_crc_ex
   legacy entry point on a scratch index built from the same snapshot
   CBOR (the test target has no xxHash include path, so the CRC sequence
   cannot be replicated by hand here — this is the sanctioned
   alternative route). */
TEST(TestEphemeralIndex, LegacySnapshotLoads) {
  char* location = path_join("/tmp", "ephemeral_legacy_snapshot");
  rm_rf(location);
  mkdir_p(location);
  char* scratch_location = path_join("/tmp", "ephemeral_legacy_snapshot_scratch");
  rm_rf(scratch_location);
  char* scratch_index_dir = path_join(scratch_location, "index");
  mkdir_p(scratch_index_dir);
  free(scratch_index_dir);
  int error_code = 0;

  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);

  /* Old-format snapshot CBOR: [node, bucket_size] where node is a single
     leaf bucket holding one 5-element (legacy) entry. */
  fibonacci_hit_counter_t counter = fibonacci_hit_counter_create();
  cbor_item_t* entry_cbor = cbor_new_definite_array(5);
  (void)cbor_array_push(entry_cbor, cbor_move(fibonacci_hit_counter_to_cbor(&counter)));
  (void)cbor_array_push(entry_cbor, cbor_move(buffer_to_cbor(hash)));
  (void)cbor_array_push(entry_cbor, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(entry_cbor, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(entry_cbor, cbor_move(cbor_build_uint64(0)));
  cbor_item_t* bucket = cbor_new_definite_array(1);
  (void)cbor_array_push(bucket, cbor_move(entry_cbor));
  cbor_item_t* node = cbor_new_definite_array(1);
  (void)cbor_array_push(node, cbor_move(bucket));
  cbor_item_t* snapshot = cbor_new_definite_array(2);
  (void)cbor_array_push(snapshot, cbor_move(node));
  (void)cbor_array_push(snapshot, cbor_move(cbor_build_uint64(25)));
  uint8_t* snapshot_data;
  size_t snapshot_size;
  cbor_serialize_alloc(snapshot, &snapshot_data, &snapshot_size);

  /* Compute the legacy CRC the way an older binary would have named the
     file, via a scratch index holding the same decoded entries. The
     modern CRC must differ (it hashes the zeroed counts too), proving
     this snapshot is only loadable through the legacy-acceptance branch. */
  index_t* scratch = cbor_to_index(snapshot, scratch_location, 60000, 60000, 3, 3);
  ASSERT_NE(scratch, nullptr);
  uint64_t legacy_crc = 0;
  EXPECT_EQ(_index_to_crc_ex(scratch, &legacy_crc, 1), 0);
  uint64_t modern_crc = 0;
  EXPECT_EQ(_index_to_crc_ex(scratch, &modern_crc, 0), 0);
  EXPECT_NE(legacy_crc, modern_crc);
  DESTROY(scratch, index);

  char filename[64];
  snprintf(filename, sizeof(filename), "1-%llu", (unsigned long long)legacy_crc);
  char* index_dir = path_join(location, "index");
  mkdir_p(index_dir);
  char* file_path = path_join(index_dir, filename);
  FILE* snapshot_file = fopen(file_path, "wb");
  ASSERT_NE(snapshot_file, nullptr);
  ASSERT_EQ(fwrite(snapshot_data, 1, snapshot_size, snapshot_file), snapshot_size);
  ASSERT_EQ(fclose(snapshot_file), 0);
  free(file_path);
  free(index_dir);
  free(snapshot_data);
  cbor_decref(&snapshot);

  index_t* index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_entry_t* found = REFERENCE(index_find(index, hash), index_entry_t);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 0u);
  EXPECT_EQ(found->pin_count, 0u);
  DESTROY(found, index_entry);
  DESTROY(index, index);

  DESTROY(hash, buffer);
  rm_rf(location);
  free(location);
  rm_rf(scratch_location);
  free(scratch_location);
}

/* ---- eager rollover retires legacy-framed WALs ---- */

/* Hand-write a legacy 78-byte 'a' record (5-element entry CBOR) into an
   already-open WAL, exactly as an older binary would have. All records for
   one file go through the same wal_t (wal_write appends at the end of the
   lazily opened file). */
static void WriteLegacyAdditionRecord(wal_t* wal, buffer_t* hash) {
  fibonacci_hit_counter_t counter = fibonacci_hit_counter_create();
  cbor_item_t* legacy_entry = cbor_new_definite_array(5);
  (void)cbor_array_push(legacy_entry, cbor_move(fibonacci_hit_counter_to_cbor(&counter)));
  (void)cbor_array_push(legacy_entry, cbor_move(buffer_to_cbor(hash)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  (void)cbor_array_push(legacy_entry, cbor_move(cbor_build_uint64(0)));
  uint8_t* legacy_data;
  size_t legacy_size;
  cbor_serialize_alloc(legacy_entry, &legacy_data, &legacy_size);
  ASSERT_EQ(legacy_size, 78u) << "legacy entry payload must be exactly 78 bytes";
  buffer_t* payload = buffer_create_from_existing_memory(legacy_data, legacy_size);
  wal_write(wal, addition, payload);
  buffer_destroy(payload);
  cbor_decref(&legacy_entry);
}

/* Numeric id parsed from the live WAL's current file path ("…/wal/<id>"). */
static uint64_t LiveWalFileId(index_t* index) {
  const char* name = index->wal->current_file;
  const char* slash = strrchr(name, '/');
  const char* backslash = strrchr(name, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash)) {
    slash = backslash;
  }
  if (slash == NULL) {
    return 0;
  }
  return strtoull(slash + 1, NULL, 10);
}

/* True when the index directory holds a snapshot file named "<id>-…". */
static bool SnapshotIdExists(char* location, uint64_t snapshot_id) {
  char* index_dir = path_join(location, "index");
  vec_str_t* files = get_dir(index_dir);
  free(index_dir);
  if (files == NULL) {
    return false;
  }
  char prefix[24];
  snprintf(prefix, sizeof(prefix), "%llu-", (unsigned long long)snapshot_id);
  bool found = false;
  for (int i = 0; i < files->length; i++) {
    if (strncmp(files->data[i], prefix, strlen(prefix)) == 0) {
      found = true;
      break;
    }
  }
  destroy_files(files);
  return found;
}

/* Tear the snapshot file named "<id>-…" down to a 4-byte stub so its CBOR
   can never load — the invalid-newest-snapshot state the rebuilding branch
   of index_create recovers from. (Tearing the content rather than just
   renaming with a bogus CRC matters: a CRC-mismatched snapshot still parses
   as CBOR, and index_create's reject path calls DESTROY on the loaded index,
   whose index_destroy debounces it — writing the rejected content back out
   as a fresh junk snapshot that collides with the rollover's ids.) Returns
   false when no such file exists. */
static bool TearSnapshot(char* location, uint64_t snapshot_id) {
  char* index_dir = path_join(location, "index");
  vec_str_t* files = get_dir(index_dir);
  if (files == NULL) {
    free(index_dir);
    return false;
  }
  char prefix[24];
  snprintf(prefix, sizeof(prefix), "%llu-", (unsigned long long)snapshot_id);
  bool torn = false;
  for (int i = 0; i < files->length && !torn; i++) {
    if (strncmp(files->data[i], prefix, strlen(prefix)) == 0) {
      char* snapshot_path = path_join(index_dir, files->data[i]);
      platform_file_t* snapshot_file =
          platform_file_open(snapshot_path, PLATFORM_O_WRONLY | PLATFORM_O_TRUNC, 0644);
      if (snapshot_file != NULL) {
        ssize_t written = platform_file_write(snapshot_file, "torn", 4);
        platform_file_close(snapshot_file);
        torn = (written == 4);
      }
      free(snapshot_path);
    }
  }
  destroy_files(files);
  free(index_dir);
  return torn;
}

/* Happy path: when index_create replays a legacy-framed live WAL it becomes
   the live WAL, and wal_read's legacy classification is sticky per file —
   new-format records appended to it would misframe every record after the
   first on the next replay. index_create must retire it eagerly: roll a
   snapshot and move the live WAL to a fresh new-format file BEFORE
   returning. The test also proves the whole legacy file replays (two
   records — mirroring records back into the WAL during replay used to
   clobber the second record's header) and that a metadata write + crash
   (no snapshot) afterwards is recoverable from the fresh WAL alone. */
TEST(TestEphemeralIndex, LegacyLiveWalRetiredByEagerRollover) {
  char* location = path_join("/tmp", "ephemeral_legacy_live_rollover");
  rm_rf(location);
  mkdir_p(location);
  int error_code = 0;

  block_t* seed_block = block_create_random_block_by_type(standard);
  buffer_t* seed_hash = (buffer_t*)refcounter_reference((refcounter_t*)seed_block->hash);
  block_destroy(seed_block);
  index_entry_t* seed_entry = index_entry_create(seed_hash);

  block_t* first_block = block_create_random_block_by_type(standard);
  buffer_t* first_hash = (buffer_t*)refcounter_reference((refcounter_t*)first_block->hash);
  block_destroy(first_block);

  block_t* second_block = block_create_random_block_by_type(standard);
  buffer_t* second_hash = (buffer_t*)refcounter_reference((refcounter_t*)second_block->hash);
  block_destroy(second_block);

  /* Phase 1: snapshot with an unrelated seed entry so the reopen takes the
     happy path with live WAL id 2. */
  index_t* seed_index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(seed_index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_add(seed_index, CONSUME(seed_entry, index_entry_t));
  DESTROY(seed_index, index);

  /* Phase 2: hand-write TWO legacy 'a' records into live WAL 2, as an older
     binary that crashed before its snapshot would have left behind. Both go
     through one wal_t (wal_write appends at the end of the lazily opened
     file). */
  wal_t* legacy_wal = wal_create(location, 2);
  ASSERT_NE(legacy_wal, nullptr);
  WriteLegacyAdditionRecord(legacy_wal, first_hash);
  WriteLegacyAdditionRecord(legacy_wal, second_hash);
  EXPECT_EQ(wal_sync(legacy_wal), 0);
  wal_destroy(legacy_wal);

  /* Phase 3: reopen. The legacy WAL must fully replay AND be retired before
     index_create returns. */
  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(error_code, 0);
  index_entry_t* first_found = REFERENCE(index_find(reloaded, first_hash), index_entry_t);
  ASSERT_NE(first_found, nullptr);
  EXPECT_EQ(first_found->ephemeral_count, 0u);
  EXPECT_EQ(first_found->pin_count, 0u);
  DESTROY(first_found, index_entry);
  index_entry_t* second_found = REFERENCE(index_find(reloaded, second_hash), index_entry_t);
  ASSERT_NE(second_found, nullptr);
  EXPECT_EQ(second_found->ephemeral_count, 0u);
  EXPECT_EQ(second_found->pin_count, 0u);
  DESTROY(second_found, index_entry);
  index_entry_t* seed_found = REFERENCE(index_find(reloaded, seed_hash), index_entry_t);
  ASSERT_NE(seed_found, nullptr);
  DESTROY(seed_found, index_entry);
  /* The live WAL is no longer the legacy file (id 2): the eager rollover
     advanced it (to a fresh id — 3 with the current id arithmetic) and wrote
     the rollover's snapshot (id 2) to disk inside index_create. */
  EXPECT_GT(LiveWalFileId(reloaded), 2u);
  EXPECT_TRUE(SnapshotIdExists(location, 2));

  /* Phase 4: write metadata into the fresh live WAL, then simulate a crash —
     deliberately no index_debounce/index_destroy, so the counts exist ONLY
     as 'm' records in the live WAL (same pattern as
     TestIndex.TestWalCrashRecovery). */
  index_entry_t* held = REFERENCE(index_find(reloaded, first_hash), index_entry_t);
  ASSERT_NE(held, nullptr);
  held->ephemeral_count = 3;
  index_write_entry_metadata(reloaded, held);
  held->pin_count = 2;
  index_write_entry_metadata(reloaded, held);
  DESTROY(held, index_entry);
  EXPECT_EQ(index_sync(reloaded), 0);

  /* Phase 5: reload without a snapshot of the counts. The eager rollover's
     snapshot carries zeroed counts, so recovering 3/2 here proves the 'm'
     records are readable — i.e. they live in a new-format WAL. */
  index_t* recovered = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(recovered, nullptr);
  index_entry_t* recovered_entry = REFERENCE(index_find(recovered, first_hash), index_entry_t);
  ASSERT_NE(recovered_entry, nullptr);
  EXPECT_EQ(recovered_entry->ephemeral_count, 3u);
  EXPECT_EQ(recovered_entry->pin_count, 2u);
  DESTROY(recovered_entry, index_entry);
  index_entry_t* recovered_second = REFERENCE(index_find(recovered, second_hash), index_entry_t);
  ASSERT_NE(recovered_second, nullptr);
  DESTROY(recovered_second, index_entry);

  DESTROY(recovered, index);
  /* Cleanup of the deliberately-undestroyed handle: every assertion is done,
     so debouncing now cannot mask anything. */
  DESTROY(reloaded, index);

  DESTROY(seed_hash, buffer);
  DESTROY(first_hash, buffer);
  DESTROY(second_hash, buffer);
  rm_rf(location);
  free(location);
}

/* Rebuild path: the newest snapshot is invalid, so index_create loads an
   older one and replays the newer session's WAL through the rebuilding
   branch. When that WAL is legacy-framed, the live WAL inherited from the
   loaded snapshot is one of the replayed files and must not keep receiving
   new-format appends — the rebuild path has to roll over too. */
TEST(TestEphemeralIndex, LegacyRebuildWalRetiredByEagerRollover) {
  char* location = path_join("/tmp", "ephemeral_legacy_rebuild_rollover");
  rm_rf(location);
  mkdir_p(location);
  int error_code = 0;

  block_t* seed_block = block_create_random_block_by_type(standard);
  buffer_t* seed_hash = (buffer_t*)refcounter_reference((refcounter_t*)seed_block->hash);
  block_destroy(seed_block);
  index_entry_t* seed_entry = index_entry_create(seed_hash);

  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);

  /* Phase 1: first session — snapshot 1 with the seed entry, clean destroy
     (live WAL becomes 2, but the file is never written). */
  index_t* first_index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(first_index, nullptr);
  EXPECT_EQ(error_code, 0);
  index_add(first_index, CONSUME(seed_entry, index_entry_t));
  DESTROY(first_index, index);

  /* Phase 2: second session — no index writes, clean destroy → snapshot 2,
     live WAL 3. WAL 2 is left empty, so the legacy record below is the only
     thing in it. */
  index_t* second_index = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(second_index, nullptr);
  EXPECT_EQ(error_code, 0);
  DESTROY(second_index, index);

  /* Phase 3: hand-write a legacy 'a' record into WAL 2 — the WAL the
     rebuilding branch replays for snapshot file id 2. */
  wal_t* legacy_wal = wal_create(location, 2);
  ASSERT_NE(legacy_wal, nullptr);
  WriteLegacyAdditionRecord(legacy_wal, hash);
  EXPECT_EQ(wal_sync(legacy_wal), 0);
  wal_destroy(legacy_wal);

  /* Phase 4: tear the newest snapshot's content so the next open falls into
     the rebuilding branch. */
  ASSERT_TRUE(TearSnapshot(location, 2));

  /* Phase 5: reopen → rebuilding branch: snapshot 1 + replay of legacy WAL 2,
     then the eager rollover retires it. error_code stays at the torn file's
     CBOR-load failure (-4): the rebuild recovered successfully, but
     index_create keeps the last file's error for the caller. */
  index_t* reloaded = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(reloaded, nullptr);
  index_entry_t* found = REFERENCE(index_find(reloaded, hash), index_entry_t);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ephemeral_count, 0u);
  EXPECT_EQ(found->pin_count, 0u);
  DESTROY(found, index_entry);
  index_entry_t* seed_found = REFERENCE(index_find(reloaded, seed_hash), index_entry_t);
  ASSERT_NE(seed_found, nullptr);
  DESTROY(seed_found, index_entry);
  /* Live WAL is neither the replayed legacy WAL 2 nor the stale WAL the
     rebuild inherited — the rollover moved it past both (to a fresh id — 4
     with the current id arithmetic), and the rollover's snapshot (id 3,
     matching the rebuild's current_id) is on disk. */
  EXPECT_GT(LiveWalFileId(reloaded), 3u);
  EXPECT_TRUE(SnapshotIdExists(location, 3));

  /* Phase 6: metadata write into the fresh live WAL + crash (no snapshot). */
  index_entry_t* held = REFERENCE(index_find(reloaded, hash), index_entry_t);
  ASSERT_NE(held, nullptr);
  held->ephemeral_count = 3;
  index_write_entry_metadata(reloaded, held);
  held->pin_count = 2;
  index_write_entry_metadata(reloaded, held);
  DESTROY(held, index_entry);
  EXPECT_EQ(index_sync(reloaded), 0);

  /* Phase 7: reload — the eager rollover's snapshot (id 3) is the newest
     valid one, so this is a happy-path open that replays only the fresh
     live WAL; the counts must recover from its 'm' records. */
  index_t* recovered = index_create(25, location, 60000, 60000, 3, 3, &error_code);
  ASSERT_NE(recovered, nullptr);
  index_entry_t* recovered_entry = REFERENCE(index_find(recovered, hash), index_entry_t);
  ASSERT_NE(recovered_entry, nullptr);
  EXPECT_EQ(recovered_entry->ephemeral_count, 3u);
  EXPECT_EQ(recovered_entry->pin_count, 2u);
  DESTROY(recovered_entry, index_entry);
  index_entry_t* recovered_seed = REFERENCE(index_find(recovered, seed_hash), index_entry_t);
  ASSERT_NE(recovered_seed, nullptr);
  DESTROY(recovered_seed, index_entry);

  DESTROY(recovered, index);
  /* Cleanup of the deliberately-undestroyed handle. */
  DESTROY(reloaded, index);

  DESTROY(seed_hash, buffer);
  DESTROY(hash, buffer);
  rm_rf(location);
  free(location);
}

/* ---- block-level ephemeral/pin ops ---- */

/* Completion actor for async block_cache tests — mirrors the
   bc_completion_t pattern in test_block_cache.cpp, extended with the
   ephemeral/pin result fields. */
typedef struct {
  ATOMIC(uint8_t) done;
  int put_result;
  block_t* get_block;
  buffer_t* get_hash;
  int remove_result;
} bc_completion_t;

static void bc_completion_dispatch(void* state, message_t* msg) {
  bc_completion_t* cs = (bc_completion_t*)state;
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* r = (cache_put_result_payload_t*)msg->payload;
      cs->put_result = r->result;
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* r = (cache_get_result_payload_t*)msg->payload;
      cs->get_block = r->block;
      cs->get_hash = r->hash;
      r->block = NULL;
      r->hash = NULL;
      break;
    }
    case CACHE_REMOVE_RESULT: {
      cache_remove_result_payload_t* r = (cache_remove_result_payload_t*)msg->payload;
      cs->remove_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Helper: put a block and wait for result (mirrors test_block_cache.cpp) */
static int bc_put_sync(block_cache_t* bc, block_t* block, scheduler_pool_t* pool) {
  bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, bc_completion_dispatch, pool);

  block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)block);
  refcounter_yield((refcounter_t*)ref_block);
  block_cache_put(bc, ref_block, 0, &comp);

  /* comp is scheduled by actor_send when bc->actor delivers the result — do
     NOT pre-inject it with an empty queue (that spawns a spurious no-op run
     and a double-injection that races actor_destroy). */
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  /* Wait for every pool worker to be idle before destroying the actor: a
     worker may still be in the tail of actor_run(&comp) (between setting
     cs.done and clearing ACTOR_FLAG_RUNNING), and actor_destroy must not
     free the queue out from under it. */
  scheduler_pool_wait_for_idle(pool);

  actor_destroy(&comp);
  return cs.put_result;
}

typedef struct {
  ATOMIC(uint8_t) done;
  int put_result;
  int ephemeral_result;
  uint16_t ephemeral_previous;
  uint16_t ephemeral_new;
  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
  int remove_result;
} eph_completion_t;

static void eph_completion_dispatch(void* state, message_t* msg) {
  eph_completion_t* cs = (eph_completion_t*)state;
  switch (msg->type) {
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* r = (cache_put_result_payload_t*)msg->payload;
      cs->put_result = r->result;
      break;
    }
    case CACHE_EPHEMERAL_RESULT: {
      cache_ephemeral_result_payload_t* r = (cache_ephemeral_result_payload_t*)msg->payload;
      cs->ephemeral_result = r->result;
      cs->ephemeral_previous = r->previous_count;
      cs->ephemeral_new = r->new_count;
      break;
    }
    case CACHE_PIN_RESULT: {
      cache_pin_result_payload_t* r = (cache_pin_result_payload_t*)msg->payload;
      cs->pin_result = r->result;
      cs->pin_previous = r->previous_count;
      cs->pin_new = r->new_count;
      break;
    }
    case CACHE_REMOVE_RESULT: {
      cache_remove_result_payload_t* r = (cache_remove_result_payload_t*)msg->payload;
      cs->remove_result = r->result;
      break;
    }
    default:
      break;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Helper: run one op against the cache actor and wait for its result using
   the completion-actor polling pattern (poll done, barrier on pool idle,
   destroy the completion actor). */
static void eph_wait(eph_completion_t* cs, actor_t* comp, scheduler_pool_t* pool) {
  /* comp is scheduled by actor_send when the cache actor delivers the
     result — do NOT pre-inject it with an empty queue. */
  while (!ATOMIC_LOAD(&cs->done)) { platform_sleep_ms(1); }
  /* Barrier before actor_destroy: a worker may still be in the tail of
     actor_run(&comp). */
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(comp);
}

static void eph_ephemeral_sync(block_cache_t* bc, buffer_t* hash, cache_ephemeral_op_e op,
                               int* result, uint16_t* previous, uint16_t* new_count,
                               scheduler_pool_t* pool) {
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_ephemeral(bc, hash, op, &comp);
  eph_wait(&cs, &comp, pool);
  *result = cs.ephemeral_result;
  *previous = cs.ephemeral_previous;
  *new_count = cs.ephemeral_new;
}

static void eph_pin_sync(block_cache_t* bc, buffer_t* hash,
                         int* result, uint32_t* previous, uint32_t* new_count,
                         scheduler_pool_t* pool) {
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_pin(bc, hash, &comp);
  eph_wait(&cs, &comp, pool);
  *result = cs.pin_result;
  *previous = cs.pin_previous;
  *new_count = cs.pin_new;
}

static int eph_remove_ex_sync(block_cache_t* bc, buffer_t* hash, uint8_t force,
                              scheduler_pool_t* pool) {
  eph_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, eph_completion_dispatch, pool);
  block_cache_remove_ex(bc, hash, force, &comp);
  eph_wait(&cs, &comp, pool);
  return cs.remove_result;
}

#define EPH_BLOCK_COUNT 4

class TestEphemeralCache : public testing::Test {
public:
  block_size_e type = standard;
  char* location;
  timer_actor_t* timer_actor;
  scheduler_pool_t* pool;
  block_cache_t* block_cache;
  block_t* blocks[EPH_BLOCK_COUNT];
  config_t config;
  void SetUp() override {
    location = path_join("/tmp", "EphemeralCacheTest");
    rm_rf(location);
    pool = scheduler_pool_create(4);
    scheduler_pool_start(pool);
    timer_actor = timer_actor_create(pool);
    mkdir_p(location);
    block_cache = NULL;
    config = config_default();
    /* Short debounce window: a mid-test snapshot fire is harmless (production
       debounces constantly) and the TearDown still flushes + syncs explicitly.
       Long windows (60s) armed a timer per mutation that made valgrind runs
       take ~48 minutes waiting them out. */
    config.index_wait = 100;
    config.index_max_wait = 100;
    for (size_t i = 0; i < EPH_BLOCK_COUNT; i++) {
      blocks[i] = block_create_random_block_by_type(type);
    }
  }
  void TearDown() override {
    scheduler_pool_wait_for_idle(pool);
    if (block_cache != NULL) {
      block_cache_sync(block_cache);
      block_cache_destroy(block_cache);
    }
    timer_actor_destroy(timer_actor);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    free(location);
    for (size_t i = 0; i < EPH_BLOCK_COUNT; i++) {
      block_destroy(blocks[i]);
    }
  }
};

/* ACQUIRE counts up, RELEASE counts down, and the last RELEASE deletes the
   block — even when a pin is held (pins do not protect ephemeral blocks).
   CLEAR zeroes the count without deleting the block (commit semantics:
   the block stays as a permanent block until something removes it). */
TEST_F(TestEphemeralCache, EphemeralAcquireReleaseClear) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);
  buffer_t* hash = blocks[0]->hash;

  int result;
  uint16_t previous;
  uint16_t new_count;

  eph_ephemeral_sync(block_cache, hash, CACHE_EPHEMERAL_ACQUIRE,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 0u);
  EXPECT_EQ(new_count, 1u);

  eph_ephemeral_sync(block_cache, hash, CACHE_EPHEMERAL_ACQUIRE,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 1u);
  EXPECT_EQ(new_count, 2u);

  /* Release one claim — the block stays while a claim is held. */
  eph_ephemeral_sync(block_cache, hash, CACHE_EPHEMERAL_RELEASE,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 2u);
  EXPECT_EQ(new_count, 1u);
  EXPECT_GT(block_cache_count(block_cache), 0u);

  /* Pin the block, then release the last claim — the block is deleted
     regardless of the pin: ephemeral claims win over pins. */
  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
  eph_pin_sync(block_cache, hash, &pin_result, &pin_previous, &pin_new, pool);
  EXPECT_EQ(pin_result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(pin_previous, 0u);
  EXPECT_EQ(pin_new, 1u);

  eph_ephemeral_sync(block_cache, hash, CACHE_EPHEMERAL_RELEASE,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 1u);
  EXPECT_EQ(new_count, 0u);
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  /* CLEAR: count zeroed, metadata committed, block kept in place. */
  ASSERT_EQ(bc_put_sync(block_cache, blocks[1], pool), CACHE_PUT_NEW);
  eph_ephemeral_sync(block_cache, blocks[1]->hash, CACHE_EPHEMERAL_ACQUIRE,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(new_count, 1u);
  eph_ephemeral_sync(block_cache, blocks[1]->hash, CACHE_EPHEMERAL_CLEAR,
                     &result, &previous, &new_count, pool);
  EXPECT_EQ(result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(previous, 1u);
  EXPECT_EQ(new_count, 0u);
  EXPECT_EQ(block_cache_count(block_cache), 1u);
}

/* Pinned permanent blocks resist a plain remove and yield
   CACHE_REMOVE_PINNED; a forced remove clears the pin and deletes. */
TEST_F(TestEphemeralCache, PinnedPermanentRemoveRejectedAndForced) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);
  buffer_t* hash = blocks[0]->hash;

  int pin_result;
  uint32_t pin_previous;
  uint32_t pin_new;
  eph_pin_sync(block_cache, hash, &pin_result, &pin_previous, &pin_new, pool);
  EXPECT_EQ(pin_result, CACHE_EPHEMERAL_OK);
  EXPECT_EQ(pin_previous, 0u);
  EXPECT_EQ(pin_new, 1u);

  EXPECT_EQ(eph_remove_ex_sync(block_cache, hash, /*force=*/0, pool),
            CACHE_REMOVE_PINNED);
  EXPECT_EQ(block_cache_count(block_cache), 1u);

  EXPECT_EQ(eph_remove_ex_sync(block_cache, hash, /*force=*/1, pool), 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);
}

/* A claimed (ephemeral_count > 0) block resists a plain remove with
   CACHE_REMOVE_EPHEMERAL_CLAIMED. */
TEST_F(TestEphemeralCache, ClaimedEphemeralRemoveRejected) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  ASSERT_EQ(bc_put_sync(block_cache, blocks[0], pool), CACHE_PUT_NEW);
  buffer_t* hash = blocks[0]->hash;

  int result;
  uint16_t previous;
  uint16_t new_count;
  eph_ephemeral_sync(block_cache, hash, CACHE_EPHEMERAL_ACQUIRE,
                     &result, &previous, &new_count, pool);
  ASSERT_EQ(result, CACHE_EPHEMERAL_OK);
  ASSERT_EQ(new_count, 1u);

  EXPECT_EQ(eph_remove_ex_sync(block_cache, hash, /*force=*/0, pool),
            CACHE_REMOVE_EPHEMERAL_CLAIMED);
  EXPECT_EQ(block_cache_count(block_cache), 1u);
}

/* ---- sheddable / servable helpers ---- */

/* The respiration exhale victim filter and the peer find-block filter are
   both pure functions of the two count fields — this pins the decision
   table without needing an authority/network. */
TEST(TestEphemeralHelpers, SheddableAndServable) {
  block_t* block = block_create_random_block_by_type(standard);
  buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)block->hash);
  block_destroy(block);
  index_entry_t* entry = index_entry_create(hash);

  /* Freshly put block: sheddable, servable. */
  EXPECT_TRUE(block_cache_entry_is_sheddable(entry));
  EXPECT_TRUE(index_entry_is_servable(entry));

  /* Pinned permanent: not sheddable, servable. */
  entry->pin_count = 1;
  EXPECT_FALSE(block_cache_entry_is_sheddable(entry));
  EXPECT_TRUE(index_entry_is_servable(entry));

  /* Ephemeral: not sheddable, not servable to peers. */
  entry->pin_count = 0;
  entry->ephemeral_count = 1;
  EXPECT_FALSE(block_cache_entry_is_sheddable(entry));
  EXPECT_FALSE(index_entry_is_servable(entry));

  index_entry_destroy(entry);
  DESTROY(hash, buffer);
}

/* Helper: synchronous CACHE_GET via a completion actor (mirrors bc_get_sync
   in test_block_cache.cpp; local copy because that helper is static there). */
static block_t* eph_get_sync(block_cache_t* bc, buffer_t* hash, scheduler_pool_t* pool) {
  bc_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, bc_completion_dispatch, pool);

  block_cache_get(bc, hash, &comp);

  /* comp is scheduled by actor_send when bc->actor delivers the result — do
     NOT pre-inject it with an empty queue. */
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  /* Barrier before actor_destroy: a worker may still be in the tail of
     actor_run(&comp). */
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);

  if (cs.get_hash != NULL) {
    DESTROY(cs.get_hash, buffer);
  }
  return cs.get_block;
}

/* block_cache_put_ephemeral stores the block with ephemeral_count == 1 and
   the block stays readable through the local CACHE_GET path while claimed
   (the peer-facing find-block filter is a separate, network-side concern). */
TEST_F(TestEphemeralCache, EphemeralPutAcquiresClaim) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);

  block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)blocks[0]);
  refcounter_yield((refcounter_t*)ref_block);
  block_cache_put_ephemeral(block_cache, ref_block, NULL);
  scheduler_pool_wait_for_idle(pool);

  index_entry_t* entry = index_peek(block_cache->index, blocks[0]->hash);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->ephemeral_count, 1u);

  /* Local reads still work while ephemeral. */
  block_t* fetched = eph_get_sync(block_cache, blocks[0]->hash, pool);
  EXPECT_NE(fetched, nullptr);
  if (fetched != NULL) {
    EXPECT_EQ(buffer_compare(fetched->hash, blocks[0]->hash), 0);
    block_destroy(fetched);
  }
}

/* Completion actor receiving the mirrored CACHE_EPHEMERAL_LIST message.
   Exercises the documented consumer contract: steal the arrays, destroy the
   stolen referenced hash buffers, free the arrays, and empty the shell
   (NULL arrays, count = 0) while leaving msg->payload intact — this
   actor_run's payload_destroy (cache_ephemeral_list_payload_destroy, via
   its void* adapter) then frees the emptied shell exactly once, with no
   double-destroy of the stolen contents. */
typedef struct {
  ATOMIC(uint8_t) done;
  size_t count;
  uint16_t claims[4];
  uint32_t pins[4];
  buffer_t* hashes[4];
} list_completion_t;

static void list_completion_dispatch(void* state, message_t* msg) {
  list_completion_t* cs = (list_completion_t*)state;
  if (msg->type != CACHE_EPHEMERAL_LIST) {
    return;
  }
  cache_ephemeral_list_payload_t* payload = (cache_ephemeral_list_payload_t*)msg->payload;
  if (payload == NULL) {
    ATOMIC_STORE(&cs->done, 1);
    return;
  }

  /* Copy out what the consumer needs before emptying the shell. Take a
     reference on each hash so the test can pin the payload content, not just
     the shape, before the stolen originals are destroyed below. */
  size_t copy_count = payload->count < 4 ? payload->count : 4;
  for (size_t idx = 0; idx < copy_count; idx++) {
    cs->claims[idx] = payload->ephemeral_counts[idx];
    cs->pins[idx] = payload->pin_counts[idx];
    if (payload->hashes != NULL && payload->hashes[idx] != NULL) {
      cs->hashes[idx] = (buffer_t*)refcounter_reference((refcounter_t*)payload->hashes[idx]);
    }
  }
  cs->count = payload->count;

  /* Steal the arrays: each hashes[i] is a referenced buffer — destroy it;
     the three arrays are plain allocations — free them. */
  if (payload->hashes != NULL) {
    for (size_t idx = 0; idx < payload->count; idx++) {
      if (payload->hashes[idx] != NULL) {
        DESTROY(payload->hashes[idx], buffer);
      }
    }
    free(payload->hashes);
    payload->hashes = NULL;
  }
  if (payload->ephemeral_counts != NULL) {
    free(payload->ephemeral_counts);
    payload->ephemeral_counts = NULL;
  }
  if (payload->pin_counts != NULL) {
    free(payload->pin_counts);
    payload->pin_counts = NULL;
  }
  payload->count = 0;
  /* msg->payload deliberately left non-NULL: actor_run's
     payload_destroy(node->msg.payload) frees only the emptied shell. */
  ATOMIC_STORE(&cs->done, 1);
}

/* The mirrored CACHE_EPHEMERAL_LIST reply hands the payload ownership to the
   consumer; the contract above must leave exactly one shell free and zero
   double-frees (valgrind verifies the second half). */
TEST_F(TestEphemeralCache, ListEphemeralMirrorsPayload) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);

  /* put + acquire two blocks */
  for (int idx = 0; idx < 2; idx++) {
    block_t* ref_block = (block_t*)refcounter_reference((refcounter_t*)blocks[idx]);
    refcounter_yield((refcounter_t*)ref_block);
    block_cache_put_ephemeral(block_cache, ref_block, NULL);
  }
  scheduler_pool_wait_for_idle(pool);

  list_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, list_completion_dispatch, pool);
  block_cache_list_ephemeral(block_cache, &comp);

  /* comp is scheduled by actor_send when the cache actor mirrors the list —
     do NOT pre-inject it with an empty queue. */
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  /* Barrier before actor_destroy: a worker may still be in the tail of
     actor_run(&comp). */
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);

  ASSERT_EQ(cs.count, 2u);
  EXPECT_EQ(cs.claims[0], 1u);
  EXPECT_EQ(cs.claims[1], 1u);
  /* Pin the payload content, not just the shape: the mirrored hashes must be
     exactly the two put blocks' hashes. Order follows the index tree walk, so
     accept either assignment. */
  ASSERT_NE(cs.hashes[0], nullptr);
  ASSERT_NE(cs.hashes[1], nullptr);
  if (cs.hashes[0] != NULL && cs.hashes[1] != NULL) {
    bool forward = buffer_compare(cs.hashes[0], blocks[0]->hash) == 0 &&
                   buffer_compare(cs.hashes[1], blocks[1]->hash) == 0;
    bool swapped = buffer_compare(cs.hashes[0], blocks[1]->hash) == 0 &&
                   buffer_compare(cs.hashes[1], blocks[0]->hash) == 0;
    EXPECT_TRUE(forward || swapped);
    DESTROY(cs.hashes[0], buffer);
    DESTROY(cs.hashes[1], buffer);
  }
}

/* ---- ephemeral registry actor ---- */

/* Completion actor for the registry CHECK round-trip — records the advisory
   present bit from the CHECK_RESULT reply. */
typedef struct {
  ATOMIC(uint8_t) done;
  uint8_t present;
} registry_completion_t;

static void registry_completion_dispatch(void* state, message_t* msg) {
  registry_completion_t* cs = (registry_completion_t*)state;
  if (msg->type == EPHEMERAL_REGISTRY_CHECK_RESULT) {
    ephemeral_registry_check_result_payload_t* result =
        (ephemeral_registry_check_result_payload_t*)msg->payload;
    cs->present = result->present;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Synchronous advisory CHECK: send the request through a completion actor,
   poll done, barrier on pool idle, destroy the completion actor. */
static void _registry_check_sync(ephemeral_registry_t* registry, buffer_t* hash,
                                 scheduler_pool_t* pool, uint8_t* present) {
  registry_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, registry_completion_dispatch, pool);
  ephemeral_registry_check(registry, hash, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  *present = cs.present;
}

/* Add → present, native remove → absent (no rebuild), two-file rotation
   persistence on disk, reload round-trip, and backup-fallback when the
   current filter file is corrupt. */
TEST(TestEphemeralRegistry, AddCheckRemovePersist) {
  char* location = path_join("/tmp", "EphemeralRegistryTest");
  rm_rf(location);
  mkdir_p(location);
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);
  config_t config = config_default();
  block_t* descriptor_block = block_create_random_block_by_type(standard);
  buffer_t* descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)descriptor_block->hash);
  block_destroy(descriptor_block);

  ephemeral_registry_t* registry = ephemeral_registry_create(location, config, pool);
  ASSERT_NE(registry, nullptr);

  uint8_t present = 2;
  _registry_check_sync(registry, descriptor_hash, pool, &present);
  EXPECT_EQ(present, 0u);

  ephemeral_registry_add(registry, descriptor_hash);
  scheduler_pool_wait_for_idle(pool);  /* flush happens in the actor's dispatch */
  present = 2;
  _registry_check_sync(registry, descriptor_hash, pool, &present);
  EXPECT_EQ(present, 1u);

  ephemeral_registry_remove(registry, descriptor_hash);
  scheduler_pool_wait_for_idle(pool);
  present = 2;
  _registry_check_sync(registry, descriptor_hash, pool, &present);
  EXPECT_EQ(present, 0u);  /* native elastic BF deletion — no rebuild needed */

  /* Persistence round-trip + .last backup rotation on disk. */
  ephemeral_registry_add(registry, descriptor_hash);
  scheduler_pool_wait_for_idle(pool);
  /* The current + backup files exist after two flushes (the remove-then-add
     pair rotates the first current into the .last backup). */
  char* current_path = path_join(location, "ephemeral_registry.bf");
  char* backup_path = path_join(location, "ephemeral_registry.bf.last");
  struct stat file_info;
  EXPECT_EQ(stat(current_path, &file_info), 0);
  EXPECT_EQ(stat(backup_path, &file_info), 0);
  free(current_path);
  free(backup_path);
  ephemeral_registry_destroy(registry);

  ephemeral_registry_t* reloaded = ephemeral_registry_create(location, config, pool);
  ASSERT_NE(reloaded, nullptr);
  present = 2;
  _registry_check_sync(reloaded, descriptor_hash, pool, &present);
  EXPECT_EQ(present, 1u);

  /* Backup-fallback: corrupt the current file → load falls back to .last.
     One more ADD first so a flush rotates the hash-containing current into
     the .last backup — the remove-flush had left an empty filter there. The
     ADD must be a real mutation: flushes fire only on actual change now
     (a self-heal re-ADD of the already-registered hash is a no-op). */
  block_t* mutation_block = block_create_random_block_by_type(standard);
  buffer_t* mutation_hash = (buffer_t*)refcounter_reference((refcounter_t*)mutation_block->hash);
  block_destroy(mutation_block);
  ephemeral_registry_add(reloaded, mutation_hash);
  scheduler_pool_wait_for_idle(pool);
  DESTROY(mutation_hash, buffer);
  ephemeral_registry_destroy(reloaded);
  char* corrupt_path = path_join(location, "ephemeral_registry.bf");
  FILE* corrupt = fopen(corrupt_path, "wb");
  ASSERT_NE(corrupt, nullptr);
  fputs("garbage", corrupt);
  fclose(corrupt);
  free(corrupt_path);
  ephemeral_registry_t* recovered = ephemeral_registry_create(location, config, pool);
  ASSERT_NE(recovered, nullptr);
  present = 2;
  _registry_check_sync(recovered, descriptor_hash, pool, &present);
  EXPECT_EQ(present, 1u);  /* served from the backup */

  ephemeral_registry_destroy(recovered);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  DESTROY(descriptor_hash, buffer);
  rm_rf(location);
  free(location);
}

/* ---- decoded-filter validation (corrupt CBOR must never crash) ---- */

/* Build a filter CBOR array shaped like elastic_bloom_filter_encode's
   output: [size, hash_count, fp_bits, seed_a, seed_b, bitset, num_occupied].
   bitset_len bytes are pushed from a zeroed buffer (the corrupt-parameter
   tests never reach the bitset contents). */
static cbor_item_t* BuildFilterCbor(uint64_t size, uint32_t hash_count,
                                    uint32_t fp_bits, size_t bitset_len) {
  /* Largest bitset_len any caller passes is 32; cbor_build_bytestring reads
     bitset_len bytes from this buffer, so it must be at least that large. */
  static const uint8_t zero_bits[32] = {0};
  cbor_item_t* filter = cbor_new_definite_array(7);
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint64(size)));
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint32(hash_count)));
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint32(fp_bits)));
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint64(1)));  /* seed_a */
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint64(2)));  /* seed_b */
  (void)cbor_array_push(filter, cbor_move(cbor_build_bytestring(zero_bits, bitset_len)));
  (void)cbor_array_push(filter, cbor_move(cbor_build_uint64(0)));  /* num_occupied */
  return filter;
}

/* elastic_bloom_filter_decode is fed arbitrary bytes (filter files on disk,
   network gossip): a corrupt size used to divide by zero on the first bucket
   access, and an oversized one drove a giant aborting allocation. Every
   variant below must return NULL without crashing. */
TEST(TestEphemeralBloomDecode, RejectsCorruptParameters) {
  /* size = 0 → division-by-zero risk. */
  cbor_item_t* filter = BuildFilterCbor(0, 4, 8, 1);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);

  /* Absurd bucket count → would abort the process via the allocator. */
  filter = BuildFilterCbor((uint64_t)1 << 40, 4, 8, 1);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);

  /* hash_count = 0 (contains() would vacuously return true) and oversized. */
  filter = BuildFilterCbor(256, 0, 8, 32);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);
  filter = BuildFilterCbor(256, 65, 8, 32);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);

  /* fp_bits = 0 and oversized. */
  filter = BuildFilterCbor(256, 4, 0, 32);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);
  filter = BuildFilterCbor(256, 4, 33, 32);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);

  /* Bitset byte length not matching (size + 7) / 8. */
  filter = BuildFilterCbor(256, 4, 8, 5);
  EXPECT_EQ(elastic_bloom_filter_decode(filter), nullptr);
  cbor_decref(&filter);
}

/* Positive control: the bounds checks must not reject anything the encoder
   actually produces. */
TEST(TestEphemeralBloomDecode, ValidEncodeDecodeRoundTrip) {
  elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 4, 0.75f, 8);
  ASSERT_NE(ebf, nullptr);
  const uint8_t data[] = "descriptor hash bytes";
  EXPECT_TRUE(elastic_bloom_filter_add(ebf, data, sizeof(data)));

  cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
  ASSERT_NE(encoded, nullptr);
  elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(encoded);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(elastic_bloom_filter_contains(decoded, data, sizeof(data)));
  const uint8_t other[] = "something else";
  EXPECT_FALSE(elastic_bloom_filter_contains(decoded, other, sizeof(other)));

  elastic_bloom_filter_destroy(decoded);
  cbor_decref(&encoded);
  elastic_bloom_filter_destroy(ebf);
}

/* attenuated_bloom_filter_decode has the same untrusted-input exposure: a
   corrupt level count must be rejected instead of driving a giant levels
   allocation (the allocator aborts on failure). */
TEST(TestEphemeralBloomDecode, AttenuatedRejectsCorruptLevelCount) {
  /* level_count = 0 — never produced by the encoder. */
  cbor_item_t* root = cbor_new_definite_array(2);
  (void)cbor_array_push(root, cbor_move(cbor_build_uint32(0)));
  (void)cbor_array_push(root, cbor_move(cbor_new_definite_array(0)));
  EXPECT_EQ(attenuated_bloom_filter_decode(root), nullptr);
  cbor_decref(&root);

  /* Absurd claimed level count clamped down to the actual (empty) array →
     zero levels → still NULL. */
  root = cbor_new_definite_array(2);
  (void)cbor_array_push(root, cbor_move(cbor_build_uint32(0xFFFFFFFFu)));
  (void)cbor_array_push(root, cbor_move(cbor_new_definite_array(0)));
  EXPECT_EQ(attenuated_bloom_filter_decode(root), nullptr);
  cbor_decref(&root);
}

/* Positive control for the attenuated path: a normal encode/decode round
   trip of a two-level filter still decodes. */
TEST(TestEphemeralBloomDecode, AttenuatedValidRoundTrip) {
  attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(2, 256, 4, 0.75f, 8);
  ASSERT_NE(abf, nullptr);
  const uint8_t topic[] = "topic bytes";
  EXPECT_TRUE(attenuated_bloom_filter_subscribe(abf, topic, sizeof(topic)));

  cbor_item_t* encoded = attenuated_bloom_filter_encode(abf);
  ASSERT_NE(encoded, nullptr);
  attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(encoded);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(attenuated_bloom_filter_level_count(decoded), 2u);
  uint32_t hops = 99;
  EXPECT_TRUE(attenuated_bloom_filter_check(decoded, topic, sizeof(topic), &hops));
  EXPECT_EQ(hops, 0u);

  attenuated_bloom_filter_destroy(decoded);
  cbor_decref(&encoded);
  attenuated_bloom_filter_destroy(abf);
}

/* ---- ephemeral put mode in writeable_off_stream / writeable_descriptor ---- */

/* Wiring for a local put pipeline, mirroring the off_routes.c put flow:
   tuples produced by the writeable off-stream feed the descriptor (the
   32-byte finalize payload is the file hash, not a tuple), and the
   off-stream's close_event closes the descriptor so it builds and stores
   its descriptor blocks. */
typedef struct {
  writeable_descriptor_t* desc;
} ephemeral_put_pipeline_t;

static void ephemeral_put_on_stream_data(void* ctx, void* data) {
  ephemeral_put_pipeline_t* pipeline = (ephemeral_put_pipeline_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (payload->size == 32) {
    /* WRITEABLE_FINALIZE emits the 32-byte file hash on data_event — not a
       tuple (tuple payloads reach here as a tuple_t whose cast "size" field
       is the tuple hash count). */
    return;
  }
  tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)payload);
  writeable_descriptor_write(pipeline->desc, tuple);
  tuple_destroy(tuple);
}

static void ephemeral_put_on_stream_close(void* ctx, void* unused) {
  (void)unused;
  ephemeral_put_pipeline_t* pipeline = (ephemeral_put_pipeline_t*)ctx;
  writeable_descriptor_close(pipeline->desc);
}

/* Completion for the descriptor stream's close_event — set from the handler
   (invoked on the descriptor's actor thread), polled with platform_sleep_ms
   like the bc_completion_t pattern above. */
typedef struct {
  ATOMIC(uint8_t) done;
} ephemeral_put_completion_t;

static void ephemeral_put_on_descriptor_close(void* ctx, void* unused) {
  (void)unused;
  ephemeral_put_completion_t* cs = (ephemeral_put_completion_t*)ctx;
  ATOMIC_STORE(&cs->done, 1);
}

/* An ephemeral put marks every block it creates with exactly one ephemeral
   claim: new_blocks_recipe claims the fresh random blocks it produces, the
   stream claims the off blocks it creates, and the descriptor stream claims
   the descriptor blocks it builds. No block is double-claimed (a random
   block re-put by the stream goes through the plain path — the recipe
   already holds its claim) and none is left unclaimed. */
TEST_F(TestEphemeralCache, EphemeralPutMarksCreatedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  tuple_cache_t* tc = tuple_cache_create(16, pool);
  ASSERT_NE(tc, nullptr);

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, block_cache, type);
  ASSERT_NE(recipe, nullptr);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* ws = writeable_off_stream_create(
      pool, block_cache, tc, type, /*tuple_size=*/3, /*digest_size=*/32, recipes, NULL);
  ASSERT_NE(ws, nullptr);
  writeable_off_stream_set_ephemeral(ws, 1);

  writeable_descriptor_t* desc = writeable_descriptor_create(
      pool, block_cache, type, /*descriptor_pad=*/32, /*tuple_size=*/3,
      /*data_length=*/type, NULL);
  ASSERT_NE(desc, nullptr);
  writeable_descriptor_set_ephemeral(desc, 1);

  ephemeral_put_pipeline_t pipeline;
  pipeline.desc = desc;
  stream_subscribe((stream_t*)ws, data_event, &pipeline,
                   ephemeral_put_on_stream_data, NULL);
  stream_subscribe((stream_t*)ws, close_event, &pipeline,
                   ephemeral_put_on_stream_close, NULL);

  ephemeral_put_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  stream_once((stream_t*)desc, close_event, &cs,
              ephemeral_put_on_descriptor_close, NULL);

  buffer_t* upload = buffer_create(type);
  upload->size = type;
  writeable_off_stream_write(ws, upload);
  writeable_off_stream_finalize(ws);
  buffer_destroy(upload);

  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);

  /* One 128000-byte upload = one tuple = two random blocks + one off block,
     plus one descriptor block. Each carries exactly one claim. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  EXPECT_EQ(entries->length, 4u);
  for (int idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
  }
  for (int idx = 0; idx < entries->length; idx++) {
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  /* Release the recipe's creation reference, deferring its destructor so it
     runs LAST (the pending list is LIFO) — after the stream's destructor has
     dropped its recipe refs. Mirrors the completion order in off_routes.c. */
  refcounter_dereference((refcounter_t*)recipe);
  scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)ws);
  stream_deferred_deref((stream_t*)desc);
  scheduler_pool_wait_for_idle(pool);
  tuple_cache_destroy(tc);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;  /* TearDown must not double-destroy */
}

/* ---- recycler enforcement (fetch-time exact check, modes, self-heal) ---- */

/* Shared helper context for a representation put pipeline: captures the
   descriptor hash of the completed put (used again by Task 10's tests). */
typedef struct {
  buffer_t* descriptor_hash;
  void* desc_handle;
  ATOMIC(uint8_t) done;
} rep_put_context_t;

static void _test_capture_descriptor_hash(void* ctx, void* data) {
  rep_put_context_t* put_ctx = (rep_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (put_ctx->descriptor_hash != NULL) buffer_destroy(put_ctx->descriptor_hash);
  put_ctx->descriptor_hash = (buffer_t*)refcounter_reference((refcounter_t*)payload);
}

static void _test_capture_tuple(void* ctx, void* data) {
  rep_put_context_t* put_ctx = (rep_put_context_t*)ctx;
  buffer_t* payload = (buffer_t*)data;
  if (payload->size == 32) {
    /* WRITEABLE_FINALIZE emits the 32-byte file hash on data_event — not a
       tuple (tuple payloads reach here as a tuple_t whose cast "size" field
       is the tuple hash count). */
    return;
  }
  tuple_t* tuple = (tuple_t*)refcounter_reference((refcounter_t*)data);
  writeable_descriptor_write((writeable_descriptor_t*)put_ctx->desc_handle, tuple);
  tuple_destroy(tuple);
}

static void _test_rep_put_ws_close(void* ctx, void* unused) {
  (void)unused;
  rep_put_context_t* put_ctx = (rep_put_context_t*)ctx;
  writeable_descriptor_close((writeable_descriptor_t*)put_ctx->desc_handle);
}

static void _test_rep_put_close(void* ctx, void* unused) {
  (void)unused;
  ATOMIC_STORE(&((rep_put_context_t*)ctx)->done, 1);
}

/* Run one complete ephemeral put of data_size bytes and return a referenced
   descriptor hash of the finished representation (NULL when the pipeline
   failed). Mirrors the off_routes.c put flow: tuples feed the descriptor,
   the off-stream's close_event closes it, and the descriptor's close_event
   marks completion. */
static buffer_t* _test_put_ephemeral(block_cache_t* bc, scheduler_pool_t* pool, size_t data_size) {
  rep_put_context_t put_ctx;
  memset(&put_ctx, 0, sizeof(put_ctx));
  tuple_cache_t* tc = tuple_cache_create(16, pool);
  if (tc == NULL) {
    return NULL;
  }
  writeable_descriptor_t* desc = writeable_descriptor_create(pool, bc, standard, 32, 3, data_size, NULL);
  if (desc == NULL) {
    tuple_cache_destroy(tc);
    return NULL;
  }
  writeable_descriptor_set_ephemeral(desc, 1);
  put_ctx.desc_handle = desc;
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, bc, standard);
  if (recipe == NULL) {
    stream_deferred_deref((stream_t*)desc);
    scheduler_pool_wait_for_idle(pool);
    tuple_cache_destroy(tc);
    return NULL;
  }
  vec_push(&recipes, (block_recipe_t*)recipe);
  writeable_off_stream_t* ws = writeable_off_stream_create(pool, bc, tc, standard, 3, 32, recipes, NULL);
  if (ws == NULL) {
    refcounter_dereference((refcounter_t*)recipe);
    scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
    stream_deferred_deref((stream_t*)desc);
    scheduler_pool_wait_for_idle(pool);
    tuple_cache_destroy(tc);
    return NULL;
  }
  writeable_off_stream_set_ephemeral(ws, 1);
  stream_subscribe((stream_t*)ws, data_event, &put_ctx, _test_capture_tuple, NULL);
  stream_subscribe((stream_t*)ws, close_event, &put_ctx, _test_rep_put_ws_close, NULL);
  stream_subscribe((stream_t*)desc, data_event, &put_ctx, _test_capture_descriptor_hash, NULL);
  stream_once((stream_t*)desc, close_event, &put_ctx, _test_rep_put_close, NULL);
  buffer_t* upload = buffer_create(data_size);
  upload->size = data_size;
  writeable_off_stream_write(ws, upload);
  writeable_off_stream_finalize(ws);
  buffer_destroy(upload);
  while (!ATOMIC_LOAD(&put_ctx.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  /* Release the recipe's creation reference, deferring its destructor so it
     runs LAST (the pending list is LIFO). Mirrors off_routes.c's order. */
  refcounter_dereference((refcounter_t*)recipe);
  scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)ws);
  stream_deferred_deref((stream_t*)desc);
  scheduler_pool_wait_for_idle(pool);
  tuple_cache_destroy(tc);
  return put_ctx.descriptor_hash;
}

typedef struct {
  ATOMIC(uint8_t) done;
  ATOMIC(uint8_t) errored;
  char error_message[128];
} recipe_watch_t;

static void recipe_error_watch(void* ctx, void* error) {
  recipe_watch_t* watch = (recipe_watch_t*)ctx;
  if (error != NULL) {
    async_error_t* async_error = (async_error_t*)error;
    if (async_error->message != NULL) {
      snprintf(watch->error_message, sizeof(watch->error_message), "%s",
               async_error->message);
    }
  }
  ATOMIC_STORE(&watch->errored, 1);
  ATOMIC_STORE(&watch->done, 1);
}

static void recipe_close_watch(void* ctx, void* unused) {
  (void)unused;
  ATOMIC_STORE(&((recipe_watch_t*)ctx)->done, 1);
}

static ori_t* _test_ori_for(buffer_t* descriptor_hash) {
  ori_t* source_ori = ori_create(standard);
  source_ori->descriptor_hash = buffer_copy(descriptor_hash);
  source_ori->block_type = standard;
  source_ori->tuple_size = 3;
  return source_ori;
}

TEST(TestRecyclerModes, ModeEnumDefaults) {
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_NONE, 0);
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_COMMIT, 1);
  EXPECT_EQ((int)RECYCLE_EPHEMERAL_PROPAGATE, 2);
}

/* Default mode: a permanent put recycling an ephemeral source must hard-error
   at data-block fetch time (the exact index walk), not deliver the block. */
TEST_F(TestEphemeralCache, RecyclerRejectsEphemeralSourceOnPermanentPut) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  vec_ori_t oris;
  vec_init(&oris);
  ori_t* source_ori = _test_ori_for(descriptor_hash);
  vec_push(&oris, source_ori);
  recycler_recipe_t* recycler = recycler_recipe_create(pool, block_cache, standard, oris, NULL,
                                                        /*put_is_ephemeral=*/0, RECYCLE_EPHEMERAL_NONE);
  ASSERT_NE(recycler, nullptr);
  /* recycler_recipe_create references every ori itself — drop the creation
     reference held by the test. */
  DESTROY(source_ori, ori);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);
  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 1);
  /* The error must be the fetch-time enforcement rejection, not a fetch
     failure — the propagate test proves the same blocks are readable. */
  EXPECT_STREQ(watch.error_message, "recycle source is ephemeral/unverified");

  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* An ephemeral put (put_is_ephemeral) behaves as propagate regardless of the
   configured mode: no error, and every recycled source block gains the
   consuming representation's claim (ephemeral_count goes 1 -> 2). */
TEST_F(TestEphemeralCache, RecyclerPropagatesForEphemeralPut) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  vec_ori_t oris;
  vec_init(&oris);
  ori_t* source_ori = _test_ori_for(descriptor_hash);
  vec_push(&oris, source_ori);
  recycler_recipe_t* recycler = recycler_recipe_create(pool, block_cache, standard, oris, NULL,
                                                        /*put_is_ephemeral=*/1, RECYCLE_EPHEMERAL_NONE);
  ASSERT_NE(recycler, nullptr);
  /* recycler_recipe_create references every ori itself — drop the creation
     reference held by the test. */
  DESTROY(source_ori, ori);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);
  /* Three pulls: two serve the descriptor's data blocks (acquiring the
     propagated claims), the third exhausts the ori and closes the recipe. */
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 0);

  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);

  /* Both recycled source blocks carry the propagated claim on top of the
     source put's own claim — and the claims must SURVIVE the recycler's
     destroy: after a successful put they are the consuming representation's
     only reference protection on the shared source blocks. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  size_t propagated = 0;
  for (int idx = 0; idx < entries->length; idx++) {
    if (entries->data[idx]->ephemeral_count >= 2) {
      propagated++;
    }
  }
  for (int idx = 0; idx < entries->length; idx++) {
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  EXPECT_GE(propagated, 2u);

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* Failure rollback: recycler_recipe_release_acquired drops every propagated
   claim the recipe acquired, restoring each recycled source block to the
   source put's own single claim. This is the consuming put's abort path —
   after a successful put the claims must instead be kept. */
TEST_F(TestEphemeralCache, RecyclerReleaseAcquiredRollsBackClaims) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  vec_ori_t oris;
  vec_init(&oris);
  ori_t* source_ori = _test_ori_for(descriptor_hash);
  vec_push(&oris, source_ori);
  recycler_recipe_t* recycler = recycler_recipe_create(pool, block_cache, standard, oris, NULL,
                                                        /*put_is_ephemeral=*/1, RECYCLE_EPHEMERAL_NONE);
  ASSERT_NE(recycler, nullptr);
  /* recycler_recipe_create references every ori itself — drop the creation
     reference held by the test. */
  DESTROY(source_ori, ori);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);
  /* Three pulls: two serve the descriptor's data blocks (acquiring the
     propagated claims), the third exhausts the ori and closes the recipe. */
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 0);

  /* The propagated claims are in place (ephemeral_count == 2 on the recycled
     source blocks). Record which entries carry them. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  std::vector<buffer_t*> claimed_hashes;
  for (int idx = 0; idx < entries->length; idx++) {
    if (entries->data[idx]->ephemeral_count >= 2) {
      claimed_hashes.push_back(
          (buffer_t*)refcounter_reference((refcounter_t*)entries->data[idx]->hash));
    }
  }
  for (int idx = 0; idx < entries->length; idx++) {
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  ASSERT_GE(claimed_hashes.size(), 2u) << "recycled source blocks must be claimed first";

  /* Rollback: the consuming put aborted, so the recipe releases its claims. */
  recycler_recipe_release_acquired(recycler);
  scheduler_pool_wait_for_idle(pool);

  /* Every previously-double-claimed block is back to the source put's own
     single claim — the rollback must neither delete the block (source A's
     claim remains) nor leave the propagated claim behind. */
  for (buffer_t* claimed_hash : claimed_hashes) {
    index_entry_t* entry = index_peek(block_cache->index, claimed_hash);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ephemeral_count, 1u);
    DESTROY(claimed_hash, buffer);
  }

  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* ---- Task 9: failure cleanup of created blocks + acquired claims ---- */

/* Poll the index until it holds expected_entries entries of which exactly
 * expected_double_claimed carry an ephemeral_count >= 2. Returns false on
 * timeout (30s) instead of hanging forever. */
static bool WaitForClaimState(block_cache_t* bc, size_t expected_entries,
                              size_t expected_double_claimed) {
  for (int attempt = 0; attempt < 15000; attempt++) {
    index_entry_vec_t* entries = index_to_array(bc->index);
    if (entries != NULL) {
      size_t length = (size_t)entries->length;
      size_t double_claimed = 0;
      for (int idx = 0; idx < entries->length; idx++) {
        if (entries->data[idx]->ephemeral_count >= 2) {
          double_claimed++;
        }
      }
      for (int idx = 0; idx < entries->length; idx++) {
        index_entry_destroy(entries->data[idx]);
      }
      vec_deinit(entries);
      free(entries);
      if (length == expected_entries && double_claimed == expected_double_claimed) {
        return true;
      }
    }
    platform_sleep_ms(2);
  }
  return false;
}

/* A failed ephemeral put must release the claim on every block it created.
 * Capacity is capped at two blocks (2 x standard): the new_blocks_recipe
 * claims and stores the first tuple's two random blocks (both fit — the
 * second lands exactly at the cap), and the off block's put — the first
 * tuple's third NEW block — fails with CACHE_PUT_FULL. The error branch must
 * release both random-block claims (each block deleted at count 0) and skip
 * the off block's never-applied claim harmlessly. No finalize is sent: the
 * put dies mid-stream (the client-disconnect failure mode). A finalize could
 * otherwise complete the single-tuple stream via _maybe_finalize before the
 * async FULL result arrives, after which the deactivate guard would drop the
 * error — so omitting it keeps the failure deterministic. */
TEST_F(TestEphemeralCache, FailedEphemeralPutCleansUpCreatedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL,
                                   /*max_capacity_bytes=*/ 2 * (size_t)standard);
  ASSERT_NE(block_cache, nullptr);
  tuple_cache_t* tc = tuple_cache_create(16, pool);
  ASSERT_NE(tc, nullptr);

  vec_block_recipe_t recipes;
  vec_init(&recipes);
  new_blocks_recipe_t* recipe = new_blocks_recipe_create(pool, block_cache, type);
  ASSERT_NE(recipe, nullptr);
  vec_push(&recipes, (block_recipe_t*)recipe);

  writeable_off_stream_t* ws = writeable_off_stream_create(
      pool, block_cache, tc, type, /*tuple_size=*/3, /*digest_size=*/32, recipes, NULL);
  ASSERT_NE(ws, nullptr);
  writeable_off_stream_set_ephemeral(ws, 1);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)ws, error_event, &watch, recipe_error_watch, NULL);

  buffer_t* upload = buffer_create(type);
  upload->size = type;
  writeable_off_stream_write(ws, upload);
  buffer_destroy(upload);

  /* The off block's put is the third NEW block against a two-block cap —
   * a real CACHE_PUT_FULL must fire and reach the error subscriber. */
  bool errored = false;
  for (int attempt = 0; attempt < 15000 && !errored; attempt++) {
    errored = ATOMIC_LOAD(&watch.errored) != 0;
    if (!errored) {
      platform_sleep_ms(2);
    }
  }
  ASSERT_TRUE(errored) << "expected CACHE_PUT_FULL to error the stream";
  scheduler_pool_wait_for_idle(pool);
  EXPECT_STREQ(watch.error_message, "cache full during put: configure larger max_capacity_bytes");

  /* Destroy the pipeline; the destroy-path cleanup backstop must be a no-op
   * after the error branch already released everything (idempotence). */
  refcounter_dereference((refcounter_t*)recipe);
  scheduler_pool_defer_cleanup(pool, recipe, (void (*)(void*))new_blocks_recipe_destroy);
  stream_deferred_deref((stream_t*)ws);
  scheduler_pool_wait_for_idle(pool);
  tuple_cache_destroy(tc);

  /* Both created random blocks were claimed then released — deleted at
   * count 0. The off block never claimed (its put was the FULL one), so its
   * release is a benign no-op. Nothing remains. */
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
}

/* Failure cleanup must roll back the claims a recycler recipe acquired on
 * recycled source blocks. Route: drive B's recycled put to its stable
 * mid-stream state — the recycler has delivered both of A's random blocks
 * (acquiring the propagated claim on each: ephemeral_count 1 -> 2) and the
 * stream has created and claimed B's own off block — then destroy B's stream
 * WITHOUT finalize/completion (the client-disconnect abort). The
 * destroy-path cleanup must release B's off block (deleted at count 0 — its
 * only claim was this put's) and call recycler_recipe_release_acquired (A's
 * random blocks back to count 1). The mid-stream state is directly observed
 * before the abort, so both the acquisition and the rollback are pinned
 * deterministically — a capacity-FULL failure would run the rollback inside
 * the error branch before any observation window could see count 2. */
TEST_F(TestEphemeralCache, FailedEphemeralPutReleasesAcquiredRecyclerClaims) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  ASSERT_NE(block_cache, nullptr);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);

  /* A's full footprint: two random blocks + off block + descriptor block,
   * each carrying A's own single claim. */
  ASSERT_EQ(block_cache_count(block_cache), 4u);
  index_entry_vec_t* a_entries = index_to_array(block_cache->index);
  ASSERT_NE(a_entries, nullptr);
  std::vector<buffer_t*> a_hashes;
  for (int idx = 0; idx < a_entries->length; idx++) {
    EXPECT_EQ(a_entries->data[idx]->ephemeral_count, 1u);
    a_hashes.push_back(
        (buffer_t*)refcounter_reference((refcounter_t*)a_entries->data[idx]->hash));
    index_entry_destroy(a_entries->data[idx]);
  }
  vec_deinit(a_entries);
  free(a_entries);
  ASSERT_EQ(a_hashes.size(), 4u);

  tuple_cache_t* tc = tuple_cache_create(16, pool);
  ASSERT_NE(tc, nullptr);

  vec_ori_t oris;
  vec_init(&oris);
  ori_t* source_ori = _test_ori_for(descriptor_hash);
  vec_push(&oris, source_ori);
  vec_block_recipe_t recipes;
  vec_init(&recipes);
  recycler_recipe_t* recycler = recycler_recipe_create(pool, block_cache, standard, oris, NULL,
                                                        /*put_is_ephemeral=*/1, RECYCLE_EPHEMERAL_NONE);
  ASSERT_NE(recycler, nullptr);
  vec_push(&recipes, (block_recipe_t*)recycler);
  /* recycler_recipe_create references every ori itself — drop the creation
     reference held by the test. */
  DESTROY(source_ori, ori);

  writeable_off_stream_t* ws = writeable_off_stream_create(
      pool, block_cache, tc, standard, /*tuple_size=*/3, /*digest_size=*/32, recipes, NULL);
  ASSERT_NE(ws, nullptr);
  writeable_off_stream_set_ephemeral(ws, 1);

  buffer_t* upload = buffer_create(standard);
  upload->size = standard;
  /* Distinct content: A's upload was a zeroed buffer, and B recycles A's
   * random blocks — with identical origin bytes B's off block would hash
   * equal to A's, land as CACHE_PUT_EXISTS, and claim A's off block instead
   * of creating a fifth entry. */
  memset(upload->data, 0x5A, standard);
  writeable_off_stream_write(ws, upload);
  buffer_destroy(upload);

  /* Terminal mid-stream state: the recycler acquired the propagated claim on
   * both recycled source blocks and the stream claimed B's off block — five
   * entries, exactly two of them double-claimed. No finalize is ever sent,
   * so the stream cannot complete on its own. */
  ASSERT_TRUE(WaitForClaimState(block_cache, /*expected_entries=*/5,
                                /*expected_double_claimed=*/2))
      << "recycled put never reached its double-claimed mid-stream state";

  /* Abort without completion: the destroy-path cleanup releases B's off
   * block and rolls the recycler's acquired claims back. */
  refcounter_dereference((refcounter_t*)recycler);
  stream_deferred_deref((stream_t*)ws);
  scheduler_pool_wait_for_idle(pool);
  /* The destroy-path cleanup above runs during the first wait's final
     pending-deref drain — AFTER that wait's idle predicate — so the
     CACHE_EPHEMERAL_RELEASE messages it enqueues are still in the cache
     actor's mailbox here. A second wait drains them before the assertions
     below observe the cache. */
  scheduler_pool_wait_for_idle(pool);
  tuple_cache_destroy(tc);

  /* B's off block is gone (released at count 0); every one of A's blocks is
   * still present, restored to A's own single claim. */
  EXPECT_EQ(block_cache_count(block_cache), 4u);
  for (buffer_t* a_hash : a_hashes) {
    index_entry_t* entry = index_peek(block_cache->index, a_hash);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ephemeral_count, 1u);
    DESTROY(a_hash, buffer);
  }

  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* ---- Task 10: representation actor ops (mark-permanent / delete / pin / unpin) ---- */

/* Completion actor for the representation actor's summary reply. */
typedef struct {
  ATOMIC(uint8_t) done;
  int result;
  size_t blocks_touched;
} rep_completion_t;

static void rep_completion_dispatch(void* state, message_t* msg) {
  rep_completion_t* cs = (rep_completion_t*)state;
  if (msg->type == REPRESENTATION_OP_RESULT) {
    representation_op_result_payload_t* result =
        (representation_op_result_payload_t*)msg->payload;
    cs->result = result->result;
    cs->blocks_touched = result->blocks_touched;
  }
  ATOMIC_STORE(&cs->done, 1);
}

/* Run one representation op to completion and return the (still-live) actor so
   the caller can destroy it once it has asserted on the cache state. */
static representation_actor_t* _test_rep_op(block_cache_t* bc, scheduler_pool_t* pool,
                                            buffer_t* descriptor_hash, representation_op_e op,
                                            int* out_result, size_t* out_blocks) {
  rep_completion_t cs;
  memset(&cs, 0, sizeof(cs));
  actor_t comp;
  actor_init(&comp, &cs, rep_completion_dispatch, pool);
  representation_actor_t* rep = representation_actor_create(bc, NULL, descriptor_hash, op, &comp);
  while (!ATOMIC_LOAD(&cs.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  actor_destroy(&comp);
  *out_result = cs.result;
  *out_blocks = cs.blocks_touched;
  return rep;
}

/* MARK_PERMANENT walks the whole chain, CLEARs every claim (commit), and keeps
   every block — the blocks become ordinary permanent blocks. */
TEST_F(TestEphemeralCache, MarkPermanentClearsClaimsAndKeepsBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  size_t entries_before = block_cache_count(block_cache);
  EXPECT_GT(entries_before, 0u);

  int result; size_t blocks;
  representation_actor_t* rep = _test_rep_op(block_cache, pool, descriptor_hash,
                                             REPRESENTATION_OP_MARK_PERMANENT, &result, &blocks);
  EXPECT_EQ(result, 0);
  EXPECT_GT(blocks, 0u);
  EXPECT_EQ(block_cache_count(block_cache), entries_before);   /* all blocks survive */

  index_entry_vec_t* entries = index_to_array(block_cache->index);
  for (int idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 0u) << "block " << idx;
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);

  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* DELETE_EPHEMERAL releases every claim of a wholly-owned representation —
   each block's count drops to zero and the block is deleted. */
TEST_F(TestEphemeralCache, DeleteEphemeralRemovesOnlyClaimedBlocks) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  int result; size_t blocks;
  representation_actor_t* rep = _test_rep_op(block_cache, pool, descriptor_hash,
                                             REPRESENTATION_OP_DELETE_EPHEMERAL, &result, &blocks);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(block_cache_count(block_cache), 0u);   /* exclusive claims → all deleted */
  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}

/* Referential integrity: a block shared with another representation survives
   a delete-ephemeral of this one. Route (the accepted alternative): the other
   representation's claims are represented by a recycler's acquired propagated
   claims (Task 8) over A's ori — after that recycle, A's blocks carry count 2.
   delete-ephemeral(A) drops A's own claim: shared blocks go 2 → 1 and SURVIVE,
   while A's exclusively-owned blocks (count 1) are deleted. Releasing the
   surviving claims afterwards (the other representation's own delete) removes
   those too. */
TEST_F(TestEphemeralCache, DeleteEphemeralSparesBlocksSharedWithOtherRepresentation) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_a = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_a, nullptr);
  ASSERT_EQ(block_cache_count(block_cache), 4u);

  /* Recycle A into an ephemeral put: the recycler acquires a second claim on
     each source block it serves (RecyclerPropagatesForEphemeralPut mechanics). */
  vec_ori_t oris;
  vec_init(&oris);
  ori_t* source_ori = _test_ori_for(descriptor_a);
  vec_push(&oris, source_ori);
  recycler_recipe_t* recycler = recycler_recipe_create(pool, block_cache, standard, oris, NULL,
                                                        /*put_is_ephemeral=*/1, RECYCLE_EPHEMERAL_NONE);
  ASSERT_NE(recycler, nullptr);
  /* recycler_recipe_create references every ori itself — drop the creation
     reference held by the test. */
  DESTROY(source_ori, ori);

  recipe_watch_t watch;
  memset(&watch, 0, sizeof(watch));
  stream_subscribe((stream_t*)recycler, error_event, &watch, recipe_error_watch, NULL);
  stream_once((stream_t*)recycler, close_event, &watch, recipe_close_watch, NULL);
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  recycler_recipe_pull(recycler);
  while (!ATOMIC_LOAD(&watch.done)) { platform_sleep_ms(1); }
  scheduler_pool_wait_for_idle(pool);
  EXPECT_EQ(ATOMIC_LOAD(&watch.errored), 0);
  stream_deferred_deref((stream_t*)recycler);
  scheduler_pool_wait_for_idle(pool);

  /* Classify A's blocks by claim count: shared (count 2, referenced by the
     recycler's propagated claims) vs exclusively owned (count 1). */
  std::vector<buffer_t*> shared_hashes;
  std::vector<buffer_t*> exclusive_hashes;
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  for (int idx = 0; idx < entries->length; idx++) {
    buffer_t* hash = (buffer_t*)refcounter_reference((refcounter_t*)entries->data[idx]->hash);
    if (entries->data[idx]->ephemeral_count >= 2) {
      shared_hashes.push_back(hash);
    } else {
      exclusive_hashes.push_back(hash);
    }
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  ASSERT_EQ(shared_hashes.size(), 2u) << "the recycler must share exactly two blocks";
  ASSERT_EQ(exclusive_hashes.size(), 2u);

  /* Delete A: its own claim goes away on every block in the walk. */
  int result; size_t blocks;
  representation_actor_t* rep = _test_rep_op(block_cache, pool, descriptor_a,
                                             REPRESENTATION_OP_DELETE_EPHEMERAL, &result, &blocks);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(blocks, 4u);   /* three data hashes + the descriptor block */

  /* Exclusively-owned blocks are deleted; shared blocks drop 2 → 1 and stay. */
  for (buffer_t* exclusive_hash : exclusive_hashes) {
    EXPECT_EQ(index_peek(block_cache->index, exclusive_hash), nullptr);
    DESTROY(exclusive_hash, buffer);
  }
  for (buffer_t* shared_hash : shared_hashes) {
    index_entry_t* entry = index_peek(block_cache->index, shared_hash);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ephemeral_count, 1u);
    DESTROY(shared_hash, buffer);
  }
  EXPECT_EQ(block_cache_count(block_cache), 2u);

  /* The other representation's own delete (its remaining claim on each shared
     block) removes those blocks too. Re-snapshot the survivors, then release
     each one's remaining claim. */
  int release_result;
  uint16_t release_previous;
  uint16_t release_new;
  index_entry_vec_t* survivors = index_to_array(block_cache->index);
  ASSERT_NE(survivors, nullptr);
  ASSERT_EQ(survivors->length, 2);
  std::vector<buffer_t*> survivor_hashes;
  for (int idx = 0; idx < survivors->length; idx++) {
    survivor_hashes.push_back(
        (buffer_t*)refcounter_reference((refcounter_t*)survivors->data[idx]->hash));
    index_entry_destroy(survivors->data[idx]);
  }
  vec_deinit(survivors);
  free(survivors);
  for (buffer_t* survivor_hash : survivor_hashes) {
    eph_ephemeral_sync(block_cache, survivor_hash, CACHE_EPHEMERAL_RELEASE,
                       &release_result, &release_previous, &release_new, pool);
    EXPECT_EQ(release_result, CACHE_EPHEMERAL_OK);
    DESTROY(survivor_hash, buffer);
  }
  EXPECT_EQ(block_cache_count(block_cache), 0u);

  representation_actor_destroy(rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_a, buffer);
}

/* PIN walks the whole chain and pins every block; UNPIN walks it again and
   takes every pin back. Pins coexist with ephemeral claims: the blocks stay
   ephemeral (ephemeral_count 1) while pinned, and unpinning an ephemeral
   representation is legal — pin_count merely returns to 0. */
TEST_F(TestEphemeralCache, PinAndUnpinWholeRepresentation) {
  block_cache = block_cache_create(config, location, type, timer_actor, pool, NULL, 0);
  buffer_t* descriptor_hash = _test_put_ephemeral(block_cache, pool, standard);
  ASSERT_NE(descriptor_hash, nullptr);
  size_t entries_before = block_cache_count(block_cache);
  EXPECT_GT(entries_before, 0u);

  int result; size_t blocks;
  representation_actor_t* pin_rep = _test_rep_op(block_cache, pool, descriptor_hash,
                                                  REPRESENTATION_OP_PIN, &result, &blocks);
  EXPECT_EQ(result, 0);
  EXPECT_GT(blocks, 0u);

  /* Every block is pinned and still carries its single ephemeral claim. */
  index_entry_vec_t* entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  for (int idx = 0; idx < entries->length; idx++) {
    EXPECT_GE(entries->data[idx]->pin_count, 1u) << "block " << idx;
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  EXPECT_EQ(block_cache_count(block_cache), entries_before);

  size_t pin_blocks = blocks;
  representation_actor_t* unpin_rep = _test_rep_op(block_cache, pool, descriptor_hash,
                                                    REPRESENTATION_OP_UNPIN, &result, &blocks);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(blocks, pin_blocks);   /* the second walk sees the same chain */

  /* Every pin is gone; the blocks remain ephemeral (never committed). */
  entries = index_to_array(block_cache->index);
  ASSERT_NE(entries, nullptr);
  for (int idx = 0; idx < entries->length; idx++) {
    EXPECT_EQ(entries->data[idx]->pin_count, 0u) << "block " << idx;
    EXPECT_EQ(entries->data[idx]->ephemeral_count, 1u) << "block " << idx;
    index_entry_destroy(entries->data[idx]);
  }
  vec_deinit(entries);
  free(entries);
  EXPECT_EQ(block_cache_count(block_cache), entries_before);

  representation_actor_destroy(pin_rep);
  representation_actor_destroy(unpin_rep);
  block_cache_sync(block_cache);
  block_cache_destroy(block_cache);
  block_cache = NULL;
  DESTROY(descriptor_hash, buffer);
}
