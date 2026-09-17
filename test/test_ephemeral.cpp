#include <gtest/gtest.h>
#include <string.h>
#include <cstdio>
extern "C" {
#include "../src/BlockCache/index.h"
#include "../src/BlockCache/block_cache.h"
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
   one file must go through the same wal_t: wal_write positions at the start
   of the (freshly opened) file, so a second wal_t would overwrite the first
   record instead of appending. */
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
     through one wal_t — wal_write positions at the start of a freshly opened
     file, so a second wal_t would clobber the first record. */
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
    /* Long debounce window so the 5ms timer cannot fire mid-test. The
       TearDown's explicit flush + sync handles persistence. */
    config.index_wait = 60000;
    config.index_max_wait = 60000;
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
