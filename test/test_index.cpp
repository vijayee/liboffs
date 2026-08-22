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
#include "../src/BlockCache/frand.h"
#include "../src/BlockCache/wal.h"
#include "../src/Platform/platform_file.h"
#include "../src/Util/get_dir.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
}

namespace indexTest {
  TEST(TestPath, TestPathJoin) {
    // path_join inserts the platform's path separator ("/" on POSIX, "\\" on
    // Windows) between dir and file, so assert against the platform-specific
    // expected result rather than a hard-coded POSIX literal.
    char *location = path_join(".", "wal");
#ifdef _WIN32
    EXPECT_EQ(strcmp(location, ".\\wal"), 0);
#else
    EXPECT_EQ(strcmp(location, "./wal"), 0);
#endif
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
    free(buf);
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

  TEST(CborToIndexEntry, RejectsMalformedArray) {
    // Too few elements — must return NULL, not crash.
    cbor_item_t* short_array = cbor_new_definite_array(2);
    (void)cbor_array_push(short_array, cbor_move(cbor_build_uint8(1)));
    (void)cbor_array_push(short_array, cbor_move(cbor_build_uint8(2)));
    EXPECT_EQ(cbor_to_index_entry(short_array), nullptr);
    cbor_decref(&short_array);

    // Non-array item — must return NULL.
    cbor_item_t* not_array = cbor_build_uint8(42);
    EXPECT_EQ(cbor_to_index_entry(not_array), nullptr);
    cbor_decref(&not_array);

    // NULL input — must return NULL.
    EXPECT_EQ(cbor_to_index_entry(NULL), nullptr);
  }

  class TestIndex : public testing::Test {
  public:
    block_t* blocks[8];
    index_entry_t* entries[8];
    char* location;
    uint64_t wait = 200;
    uint64_t max_wait = 5000;
    void SetUp() override {
      for (size_t i = 0; i < 8; i++) {
        buffer_t* buf = buffer_create_from_existing_memory(frand(nano), nano);
        blocks[i] = block_create_by_type(CONSUME(buf, buffer_t), nano);
        entries[i] = index_entry_create(blocks[i]->hash);
      }
      location = path_join("/tmp", "block_index");
      rm_rf(location);
      mkdir_p(location);
    }
    void TearDown() override {
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
    /* Rename ONLY the newest snapshot file so its stored CRC no longer
       matches the computed CRC — forces index_create to skip it and fall
       back to an older snapshot (exercising the rebuilding branch). Unlike
       CorruptCRC (which has an off-by-one and corrupts count+1 files), this
       corrupts exactly one file: the newest. */
    void CorruptNewestSnapshotCRC() {
      char* index_location = path_join(location, "index");
      vec_str_t* files = get_dir(index_location);
      ASSERT_NE(files, nullptr);
      ASSERT_GE(files->length, 1u);
      vec_sort(files, _sort_indexes);
      char* newest = files->data[files->length - 1];
      char* newest_copy = strdup(newest);
      char* id_token = strtok(newest_copy, "-");
      ASSERT_NE(id_token, nullptr);
      uint64_t id = strtoull(id_token, nullptr, 10);
      char* crc_token = strtok(nullptr, "-");
      ASSERT_NE(crc_token, nullptr);
      uint64_t crc = strtoull(crc_token, nullptr, 10);
      free(newest_copy);
      crc++;  // make the stored CRC wrong by 1
      char new_name[64];
      snprintf(new_name, sizeof(new_name), "%lu-%lu", id, crc);
      char* old_path = path_join(index_location, newest);
      char* new_path = path_join(index_location, new_name);
      ASSERT_EQ(rename(old_path, new_path), 0) << "rename " << old_path << " -> " << new_path << " failed";
      free(old_path);
      free(new_path);
      destroy_files(files);
      free(index_location);
    }
  };

  TEST_F(TestIndex, TestIndexFunctions) {
    int error_code;
    index_t* index = index_create(25, location, wait, max_wait, 3, 3, &error_code);
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
    index_t* from_cbor = cbor_to_index(cbor2, location, wait, max_wait, 3, 3);
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

    index = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    EXPECT_TRUE(error_code == 0);
    DESTROY(index, index);

  }
  TEST_F(TestIndex, TestIndexRecovery) {
    int error_code;
    index_t* index;
    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_add(index, entries[i]);
      DESTROY(index, index);
    }
    index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_add(index, entries[i]);
    }
    DESTROY(index, index);

    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_entry_t* entry = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
      DESTROY(entry, index_entry);
      DESTROY(index, index);
    }
    index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_entry_t* entry = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
      DESTROY(entry, index_entry);
    }
    DESTROY(index, index);

    for (size_t i = 0; i < 4; i++) {
      index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
      EXPECT_TRUE(error_code == 0);
      index_remove(index, entries[i]->hash);
      DESTROY(index, index);
    }
    index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
    EXPECT_TRUE(error_code == 0);
    for (size_t i = 4; i < 8; i++) {
      index_remove(index, entries[i]->hash);
    }
    DESTROY(index, index);

    index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
    EXPECT_TRUE(error_code == 0);
    cbor_item_t *cbor = index_to_cbor(index);
    DESTROY(index, index);

    CorruptCRC(5);

    index = index_create(25, location, wait, max_wait, 0, 0, &error_code);
    uint64_t index_crc;
    EXPECT_EQ(index_to_crc(index, &index_crc), 0);
    DESTROY(index, index);
    index_t* from_cbor = cbor_to_index(cbor, location, wait, max_wait, 0, 0);
    cbor_decref(&cbor);
    uint64_t cbor_crc;
    EXPECT_EQ(index_to_crc(from_cbor, &cbor_crc), 0);
    DESTROY(from_cbor, index);
    EXPECT_EQ(cbor_crc, index_crc);
  }

  /* Crash-recovery regression test for the WAL replay path (index.c:255-289
     wal_load + replay). The existing TestIndexRecovery cycles
     index_create/destroy with max_wals=0, max_snapshots=0, so it exercises
     snapshot recovery only — it never leaves an index un-destroyed with
     pending WAL entries. This test simulates a SIGKILL mid-run: write a
     clean snapshot, then add more entries and drop the handle WITHOUT
     index_destroy (no flush, no snapshot of the new entries), then reopen
     and assert the post-snapshot entries survived via WAL replay.
     DISABLED: this test currently FAILS — entries added after the last
     snapshot are not restored on reopen, which means the WAL replay path
     is not covering post-snapshot WAL entries. Tracked in OFFS-184. Remove
     the DISABLED_ prefix once OFFS-184 lands. */
  TEST_F(TestIndex, TestWalCrashRecovery) {
    int error_code = 0;
    const size_t snapshot_count = 4;
    const size_t crash_count = 4;

    /* Phase 1: clean shutdown — writes a snapshot with the first
       snapshot_count entries. */
    {
      index_t* index = index_create(25, location, wait, max_wait, 3, 3, &error_code);
      ASSERT_TRUE(index != NULL);
      EXPECT_EQ(error_code, 0);
      for (size_t i = 0; i < snapshot_count; i++) {
        index_add(index, REFERENCE(entries[i], index_entry_t));
      }
      /* Verify they're live before the clean close. */
      for (size_t i = 0; i < snapshot_count; i++) {
        index_entry_t* got = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
        EXPECT_NE(got, nullptr);
        DESTROY(got, index_entry);
      }
      DESTROY(index, index);
    }

    /* Phase 2: reopen (loads snapshot), add crash_count MORE entries, then
       drop the handle without index_destroy — this is the crash. The new
       entries exist ONLY in the WAL; no snapshot was written for them. */
    {
      index_t* index = index_create(25, location, wait, max_wait, 3, 3, &error_code);
      ASSERT_TRUE(index != NULL);
      EXPECT_EQ(error_code, 0);
      for (size_t i = snapshot_count; i < snapshot_count + crash_count; i++) {
        index_add(index, REFERENCE(entries[i], index_entry_t));
      }
      /* Confirm the snapshot entries are still present after reopen. */
      for (size_t i = 0; i < snapshot_count; i++) {
        index_entry_t* got = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
        EXPECT_NE(got, nullptr);
        DESTROY(got, index_entry);
      }
      /* Deliberately do NOT call index_destroy — simulate a crash. The
         platform_file_t handle leaks for the rest of the test, which is
         fine on POSIX (the next index_create opens the file independently). */
    }

    /* Phase 3: reopen after "crash". The snapshot has the first
       snapshot_count entries; the WAL must replay to restore the
       crash_count entries added in phase 2. Assert ALL entries are
       present — if WAL replay is broken, the phase-2 entries are lost. */
    {
      index_t* index = index_create(25, location, wait, max_wait, 3, 3, &error_code);
      ASSERT_TRUE(index != NULL);
      EXPECT_EQ(error_code, 0);
      for (size_t i = 0; i < snapshot_count + crash_count; i++) {
        index_entry_t* got = REFERENCE(index_get(index, blocks[i]->hash), index_entry_t);
        EXPECT_NE(got, nullptr) << "entry " << i << " lost after crash recovery";
        if (got != NULL) {
          EXPECT_EQ(buffer_compare(got->hash, entries[i]->hash), 0);
          DESTROY(got, index_entry);
        }
      }
      DESTROY(index, index);
    }
  }

  /* Rebuilding-branch regression test for stop-and-keep-prefix WAL recovery.
     The setup forces index_create to fall back to an OLDER snapshot and
     replay a NEWER session's WAL via the rebuilding branch (the
     i != files->length-1 case in index_create). The newer snapshot is
     corrupted (CRC mismatch via rename), and the WAL that the rebuilding
     branch replays is given a torn tail (truncate). Pre-fix, the rebuilding
     branch bailed to _index_new_empty on any non-clean-EOF — losing the
     older snapshot's entries too. Post-fix, stop-and-keep-prefix preserves
     the older snapshot + the intact prefix of the torn WAL. */
  TEST_F(TestIndex, IndexRecoveryTornTailKeepsPrefix) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX-only test (uses truncate(2)/stat(2))";
#else
    int error_code = 0;

    /* Phase 1: create index, add 4 entries, clean destroy -> snapshot A. */
    index_t* index1 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index1, nullptr);
    for (int i = 0; i < 4; i++) {
      index_add(index1, REFERENCE(entries[i], index_entry_t));
    }
    DESTROY(index1, index);

    /* Phase 2: reopen, add 4 more entries, clean destroy -> snapshot B.
       The session-2 WAL (id == snapshot B's id) holds the 4 addition records. */
    index_t* index2 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index2, nullptr);
    for (int i = 4; i < 8; i++) {
      index_add(index2, REFERENCE(entries[i], index_entry_t));
    }
    DESTROY(index2, index);

    /* Phase 3: locate the session-2 WAL (the newest snapshot's id == its WAL
       id) and truncate its tail to simulate a torn-tail storage fault. */
    char* index_dir = path_join(location, "index");
    vec_str_t* snap_files = get_dir(index_dir);
    ASSERT_NE(snap_files, nullptr);
    ASSERT_GE(snap_files->length, 2u);
    vec_sort(snap_files, _sort_indexes);
    char* newest_snap = snap_files->data[snap_files->length - 1];
    /* Parse "{id}-{crc}" — strtok mutates its input, so work on a copy. */
    char* snap_copy = strdup(newest_snap);
    char* id_token = strtok(snap_copy, "-");
    ASSERT_NE(id_token, nullptr);
    uint64_t newest_id = strtoull(id_token, nullptr, 10);
    free(snap_copy);
    destroy_files(snap_files);
    free(index_dir);

    char* wal_dir = path_join(location, "wal");
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lu", (unsigned long)newest_id);
    char* wal_path = path_join(wal_dir, id_str);
    free(wal_dir);
    struct stat st;
    ASSERT_EQ(stat(wal_path, &st), 0) << "WAL " << wal_path << " must exist";
    ASSERT_GT(st.st_size, 10) << "WAL must have records to truncate";
    ASSERT_EQ(truncate(wal_path, st.st_size - 10), 0) << "truncate failed";
    free(wal_path);

    /* Phase 4: corrupt snapshot B's CRC so recovery falls back to snapshot A
       and exercises the rebuilding branch (which replays the now-torn WAL). */
    CorruptNewestSnapshotCRC();

    /* Phase 5: reopen after the simulated storage fault. Stop-and-keep-prefix
       must preserve snapshot A's 4 entries + the intact prefix of the torn
       WAL (at minimum the first post-snapshot entry). Pre-fix, the bail-to-
       empty would lose everything. */
    index_t* index3 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index3, nullptr);
    for (int i = 0; i < 4; i++) {
      index_entry_t* found = index_find(index3, entries[i]->hash);
      EXPECT_NE(found, nullptr) << "snapshot A entry " << i << " lost (stop-and-keep-prefix must keep the older snapshot)";
      if (found) DESTROY(found, index_entry);
    }
    /* Entry 4 is the first record in the torn WAL; the truncate cut into a
       later record, so entry 4 must survive. */
    index_entry_t* found4 = index_find(index3, entries[4]->hash);
    EXPECT_NE(found4, nullptr) << "first post-snapshot entry lost (torn tail should keep prefix)";
    if (found4) DESTROY(found4, index_entry);
    DESTROY(index3, index);
#endif
  }

  /* Rebuilding-branch regression test for stop-and-keep-prefix under a CRC
     mismatch in a replayed WAL (vs. torn tail above). Same setup as the
     torn-tail test, but instead of truncating the WAL, we flip a byte in
     the middle of one record's payload so its CRC check fails. The
     rebuilding branch must stop at the corrupt record and keep the prefix. */
  TEST_F(TestIndex, IndexRecoveryCrcCorruptionKeepsPrefix) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX-only test (uses open(2)/stat(2))";
#else
    int error_code = 0;

    /* Phase 1: snapshot A with 4 entries. */
    index_t* index1 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index1, nullptr);
    for (int i = 0; i < 4; i++) {
      index_add(index1, REFERENCE(entries[i], index_entry_t));
    }
    DESTROY(index1, index);

    /* Phase 2: snapshot B with 4 more entries; session-2 WAL has 4 records. */
    index_t* index2 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index2, nullptr);
    for (int i = 4; i < 8; i++) {
      index_add(index2, REFERENCE(entries[i], index_entry_t));
    }
    DESTROY(index2, index);

    /* Phase 3: flip a byte in the middle of the session-2 WAL payload. Each
       'a' record is 1 (type) + 4 (crc) + 78 (payload) = 83 bytes. Offset 50
       lands inside the first record's payload, so its CRC fails and replay
       stops at record 0 — losing all 4 session-2 entries but keeping
       snapshot A. This is the strongest test of stop-and-keep-prefix: the
       very first replayed record is corrupt, yet the older snapshot is kept. */
    char* index_dir = path_join(location, "index");
    vec_str_t* snap_files = get_dir(index_dir);
    ASSERT_NE(snap_files, nullptr);
    ASSERT_GE(snap_files->length, 2u);
    vec_sort(snap_files, _sort_indexes);
    char* newest_snap = snap_files->data[snap_files->length - 1];
    /* Parse "{id}-{crc}" — strtok mutates its input, so work on a copy. */
    char* snap_copy = strdup(newest_snap);
    char* id_token = strtok(snap_copy, "-");
    ASSERT_NE(id_token, nullptr);
    uint64_t newest_id = strtoull(id_token, nullptr, 10);
    free(snap_copy);
    destroy_files(snap_files);
    free(index_dir);

    char* wal_dir = path_join(location, "wal");
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lu", (unsigned long)newest_id);
    char* wal_path = path_join(wal_dir, id_str);
    free(wal_dir);
    struct stat st;
    ASSERT_EQ(stat(wal_path, &st), 0) << "WAL must exist";
    ASSERT_GT(st.st_size, 50) << "WAL must have a record to corrupt";

    int fd = open(wal_path, O_RDWR);
    ASSERT_GE(fd, 0) << "open WAL failed";
    uint8_t byte;
    ASSERT_EQ(pread(fd, &byte, 1, 50), 1) << "pread at offset 50 failed";
    byte = (uint8_t)(byte ^ 0xFF);
    ASSERT_EQ(pwrite(fd, &byte, 1, 50), 1) << "pwrite at offset 50 failed";
    ASSERT_EQ(close(fd), 0) << "close WAL failed";
    free(wal_path);

    /* Phase 4: corrupt snapshot B's CRC so recovery uses the rebuilding branch. */
    CorruptNewestSnapshotCRC();

    /* Phase 5: reopen. Snapshot A's 4 entries must survive (stop-and-keep-prefix).
       The session-2 WAL's first record is corrupt, so entries 4-7 are lost —
       but the older snapshot is NOT lost. Pre-fix, bail-to-empty lost everything. */
    index_t* index3 = index_create(25, location, wait, max_wait, 3, 3, &error_code);
    ASSERT_NE(index3, nullptr);
    for (int i = 0; i < 4; i++) {
      index_entry_t* found = index_find(index3, entries[i]->hash);
      EXPECT_NE(found, nullptr) << "snapshot A entry " << i << " lost (stop-and-keep-prefix must keep the older snapshot)";
      if (found) DESTROY(found, index_entry);
    }
    /* The corrupt first record means entry 4 (and the rest of session 2) is
       lost — assert this to confirm the corruption had effect. */
    index_entry_t* found4 = index_find(index3, entries[4]->hash);
    EXPECT_EQ(found4, nullptr) << "corrupt first record should not be applied";
    if (found4) DESTROY(found4, index_entry);
    DESTROY(index3, index);
#endif
  }

  TEST(TestWal, WalSyncOpenWalReturnsZero) {
    char* location = path_join("/tmp", "wal_sync_test");
    mkdir_p(location);

    wal_t* wal = wal_create(location, 1);
    ASSERT_NE(wal, nullptr);

    buffer_t* data = buffer_create(78);
    memset(data->data, 0xAB, 78);
    wal_write(wal, addition, data);

    int result = wal_sync(wal);
    EXPECT_EQ(result, 0);

    DESTROY(data, buffer);
    wal_destroy(wal);
    rm_rf(location);
    free(location);
  }

  TEST(TestWal, WalSyncNullWalReturnsNegative) {
    int result = wal_sync(NULL);
    EXPECT_EQ(result, -1);
  }

  TEST(TestWal, WalSyncNullLogReturnsNegative) {
    char* location = path_join("/tmp", "wal_sync_null_log");
    mkdir_p(location);

    wal_t* wal = wal_create(location, 1);
    ASSERT_NE(wal, nullptr);
    /* Don't write -- log is still NULL */
    int result = wal_sync(wal);
    EXPECT_EQ(result, -1);

    wal_destroy(wal);
    rm_rf(location);
    free(location);
  }

  /* Regression test for wal_read's type switch: an unknown type byte must
     return WAL_ERR_UNKNOWN_TYPE rather than leaving `size` uninitialized
     and calling get_memory(garbage). Writes a valid 'addition' record,
     then corrupts the type byte in-place before reopening. */
  TEST(WalRead, UnknownTypeByteReturnsErrorNotCrash) {
    char* dir = path_join("/tmp", "wal_unknown_type_test");
    rm_rf(dir);
    mkdir_p(dir);
    uint64_t id = 1;
    wal_t* wal = wal_create(dir, id);
    ASSERT_NE(wal, nullptr);

    buffer_t* payload = buffer_create(78);
    for (size_t index = 0; index < 78; index++) {
      payload->data[index] = (uint8_t)index;
    }
    wal_write(wal, addition, payload);
    buffer_destroy(payload);

    /* Corrupt the type byte of that record in-place: seek to offset 0,
       write 'x'. wal_write lazily opened wal->log above. */
    ASSERT_NE(wal->log, nullptr);
    platform_file_seek(wal->log, 0, PLATFORM_SEEK_SET);
    uint8_t bad_type = 'x';
    platform_file_write(wal->log, &bad_type, 1);
    platform_file_sync(wal->log);
    wal_destroy(wal);

    /* Re-load and read — must return WAL_ERR_UNKNOWN_TYPE, not crash. */
    wal_t* wal2 = wal_load(dir, id);
    ASSERT_NE(wal2, nullptr);
    wal_type_e type;
    buffer_t* data = nullptr;
    uint64_t cursor = 0;
    int32_t wal_size = 0;
    int rc = wal_read(wal2, &type, &data, &cursor, &wal_size);
    EXPECT_EQ(rc, WAL_ERR_UNKNOWN_TYPE);
    wal_destroy(wal2);

    rm_rf(dir);
    free(dir);
  }
}
