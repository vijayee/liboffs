# Storage Durability Implementation Plan (Stage 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the BlockCache durable across crashes: replay the live WAL on the happy path, fsync data→WAL→snapshot in the right order, verify WAL CRCs with stop-and-keep-prefix error handling, and validate recovery-side CBOR — so a crash loses at most the half-written final WAL record, never the whole index.

**Architecture:** The WAL record format `[type:1][crc:4][payload:size]` is already self-framing and CRC'd; `wal_read` already returns 4 on CRC mismatch. The gaps are: (1) the caller (`index_create`) treats any non-EOF read as a hard failure and bails to an empty index; (2) the happy path doesn't replay the live WAL at all; (3) no fsync on section data, snapshot, or meta files; (4) no `default` in the type switch (UB on unknown type); (5) no CBOR shape validation in `cbor_to_index_entry`. Fix each in place, plus an `fsync_data` config flag (default true, off in test builds for speed) plumbed through the section/index layer chain.

**Tech Stack:** C11, libcbor, XXH32, BLAKE3, GoogleTest, `platform_file_sync` (POSIX `fsync` / Windows `FlushFileBuffers`).

**Spec:** `docs/superpowers/specs/2026-08-22-production-readiness-fixes-design.md` (Section 5).

---

## File Structure

**Modify:**
- `src/BlockCache/wal.h` — add `WAL_ERR_*` named error constants.
- `src/BlockCache/wal.c` — `wal_read`: `default` in type switch (init `size`, return `WAL_ERR_UNKNOWN_TYPE`); replace magic return codes with named constants (where the caller cares).
- `src/BlockCache/index.c` — `cbor_to_index_entry` shape validation; `index_create` happy-path live-WAL replay + stop-and-keep-prefix error handling (both branches); `index_debounce` write-return check + snapshot fsync + old-WAL fsync; `index_t` gains `bool fsync_data`; `index_create`/`_index_new_empty`/`index_create_from` plumb `fsync_data`.
- `src/BlockCache/index.h` — `index_t` `fsync_data` field; updated `index_create`/`_index_new_empty` signatures if needed (prefer adding a field set after create rather than changing signatures — see Task 6).
- `src/BlockCache/section.c` — `SECTION_WRITE` fsync after pwrite (gated by `fsync_data`); `section_save_meta` fsync.
- `src/BlockCache/section.h` — `section_t` `fsync_data` field.
- `src/BlockCache/sections.c` — `round_robin_save` fwrite-return check + fsync (or refactor to `platform_file_atomic_write`); plumb `fsync_data` in `sections_create`/`section_create`.
- `src/BlockCache/sections.h` — `sections_t` `fsync_data` field.
- `src/BlockCache/block_cache.c` — `block_cache_create` plumb `fsync_data` from `config` into `sections` + `index`; `block_cache_sync` fsync section data files + snapshot at shutdown.
- `src/BlockCache/block_cache.h` — `block_cache_t` `fsync_data` field.
- `src/Configuration/config.h`, `src/Configuration/config.c` — `bool fsync_data` (default true) + default + (no validation needed for bool, but add a `config_json.c` parser entry if the JSON parser exists).
- `test/test_index.cpp` — re-enable `DISABLED_TestWalCrashRecovery` → `TestWalCrashRecovery`; add torn-tail, CRC-corruption, unknown-type, CBOR-parse-failure, ENOSPC, total-loss tests.
- `test/test_section.cpp` / `test/test_block_cache.cpp` — fsync + meta tests as needed.

---

### Task 1: WAL named error constants + `default` in type switch

**Files:**
- Modify: `src/BlockCache/wal.h` (add constants)
- Modify: `src/BlockCache/wal.c` (`wal_read` switch + return codes)
- Test: `test/test_index.cpp` (or a new `test_wal.cpp` if one exists — check)

- [ ] **Step 1: Write the failing tests**

Add to `test/test_index.cpp` (it already includes `wal.h`):

```cpp
TEST(WalRead, UnknownTypeByteReturnsErrorNotCrash) {
  // Build a WAL file with a single record whose type byte is unknown ('x').
  char* dir = path_join("/tmp", "wal_unknown_type_test");
  rm_rf(dir); mkdir_p(dir);
  uint64_t id = 1;
  wal_t* wal = wal_create(dir, id);
  // Manually write a bad record: type='x', crc=0, payload of 78 bytes (so the
  // read gets past the type+crc reads and into the payload, then the switch
  // hits the unknown type — OR the switch default fires before allocating).
  // Actually wal_read reads type first, then switch determines size. With an
  // unknown type and no default, size is uninitialized. We want: with the fix,
  // wal_read returns WAL_ERR_UNKNOWN_TYPE and does NOT allocate garbage.
  uint8_t bad_type = 'x';
  uint32_t crc = 0;
  wal_write(wal, addition, buffer_create(78));  // write a valid record first
  // Overwrite the type byte of that record in-place via the file.
  // (wal_write opened wal->log; seek to 0 and write the bad type byte.)
  platform_file_seek(wal->log, 0, PLATFORM_SEEK_SET);
  platform_file_write(wal->log, &bad_type, 1);
  platform_file_sync(wal->log);
  // Re-load and read.
  wal_destroy(wal);
  wal_t* wal2 = wal_load(dir, id);
  wal_type_e type;
  buffer_t* data;
  uint64_t cursor;
  int32_t wal_size;
  int rc = wal_read(wal2, &type, &data, &cursor, &wal_size);
  EXPECT_EQ(rc, WAL_ERR_UNKNOWN_TYPE);
  wal_destroy(wal2);
  rm_rf(dir);
  free(dir);
}
```

(Confirm `platform_file_seek`/`platform_file_write`/`platform_file_sync` are accessible from the test — they're in `Platform/platform_file.h`; add the include if needed. If driving the file directly is awkward, alternatively write a raw file with `FILE*` containing `[type='x'][crc=0][78 zero bytes]` and `wal_load` it.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=WalRead.*`
Expected: FAIL — `WAL_ERR_UNKNOWN_TYPE` not defined / UB (possible crash).

- [ ] **Step 3: Add the error constants to wal.h**

In `src/BlockCache/wal.h`, after the `wal_type_e` enum (line 18):

```c
// wal_read return codes. -3 is clean EOF; 0 is a complete valid record.
// Positive codes are recoverable errors (the caller should stop replay at
// the last complete record and keep the prefix, NOT bail to an empty index).
#define WAL_ERR_SHORT_TYPE      1
#define WAL_ERR_SHORT_CRC       2
#define WAL_ERR_SHORT_PAYLOAD   3
#define WAL_ERR_CRC             4
#define WAL_ERR_UNKNOWN_TYPE    5
```

(The existing magic numbers 1/2/3/4 keep their values; `WAL_ERR_UNKNOWN_TYPE` = 5 is new.)

- [ ] **Step 4: Add the `default` to `wal_read`'s switch and init `size`**

In `src/BlockCache/wal.c` `wal_read` (lines 111-123), change:

```c
  uint64_t size = 0;  // init — unknown type must not allocate garbage
  switch (*type) {
    case 'a':
    case 'i':
      size = 78;
      break;
    case 'e':
      size = 44;
      break;
    case 'r':
      size = 34;
      break;
    default:
      return WAL_ERR_UNKNOWN_TYPE;
  }
```

Also replace the bare `return 1/2/3/4` with the named constants (`return WAL_ERR_SHORT_TYPE;` etc.) for clarity — optional but consistent. The `return -3` (EOF) stays as-is (or add `#define WAL_ERR_EOF -3` and use it — your call, but keep `-3` working since `index_create` checks `read_result != -3`).

- [ ] **Step 5: Run test to verify it passes**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=WalRead.*`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/BlockCache/wal.h src/BlockCache/wal.c test/test_index.cpp
git commit -m "fix(wal): named error codes + default in type switch (no UB on unknown type)"
```

---

### Task 2: `cbor_to_index_entry` shape validation

**Files:**
- Modify: `src/BlockCache/index.c` (`cbor_to_index_entry`, lines 96-114)
- Test: `test/test_index.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test/test_index.cpp`:

```cpp
TEST(CborToIndexEntry, RejectsMalformedArray) {
  // A CBOR array with too few elements must return NULL, not crash.
  cbor_item_t* short_array = cbor_new_definite_array(2);
  cbor_array_push(short_array, cbor_move(cbor_build_uint8(1)));
  cbor_array_push(short_array, cbor_move(cbor_build_uint8(2)));
  EXPECT_EQ(cbor_to_index_entry(short_array), nullptr);
  cbor_decref(&short_array);

  // A non-array item must return NULL.
  cbor_item_t* not_array = cbor_build_uint8(42);
  EXPECT_EQ(cbor_to_index_entry(not_array), nullptr);
  cbor_decref(&not_array);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=CborToIndexEntry.*`
Expected: FAIL — likely a crash (unconditional `cbor_array_get` on a 2-element array reads past the end / returns NULL → crash in `cbor_to_fibonacci_hit_counter(NULL)`).

- [ ] **Step 3: Add shape validation to `cbor_to_index_entry`**

In `src/BlockCache/index.c`, replace `cbor_to_index_entry` (lines 96-114):

```c
index_entry_t* cbor_to_index_entry(cbor_item_t* cbor) {
  if (cbor == NULL || !cbor_isa_array(cbor) || cbor_array_size(cbor) < 5) {
    return NULL;
  }
  cbor_item_t* item0 = cbor_array_get(cbor, 0);
  cbor_item_t* item1 = cbor_array_get(cbor, 1);
  cbor_item_t* item2 = cbor_array_get(cbor, 2);
  cbor_item_t* item3 = cbor_array_get(cbor, 3);
  cbor_item_t* item4 = cbor_array_get(cbor, 4);
  // Validate types before use. item0 is a fibonacci counter (array),
  // item1 is a bytestring (hash), items 2/3/4 are uints.
  if (item0 == NULL || item1 == NULL || item2 == NULL || item3 == NULL || item4 == NULL) {
    goto cleanup_null;
  }
  if (!cbor_isa_array(item0) || !cbor_isa_bytestring(item1) ||
      !cbor_isa_uint(item2) || !cbor_isa_uint(item3) || !cbor_isa_uint(item4)) {
    goto cleanup_null;
  }
  fibonacci_hit_counter_t counter = cbor_to_fibonacci_hit_counter(item0);
  buffer_t* hash = cbor_to_buffer(item1);
  size_t section_index = (size_t)cbor_get_int(item2);
  size_t section_id = (size_t)cbor_get_int(item3);
  uint64_t ejection_date = cbor_get_int(item4);
  cbor_decref(&item0);
  cbor_decref(&item1);
  cbor_decref(&item2);
  cbor_decref(&item3);
  cbor_decref(&item4);
  refcounter_yield((refcounter_t*)hash);
  return index_entry_from(hash, section_id, section_index, ejection_date, counter);

cleanup_null:
  cbor_decref(&item0);
  cbor_decref(&item1);
  cbor_decref(&item2);
  cbor_decref(&item3);
  cbor_decref(&item4);
  return NULL;
}
```

Note: `cbor_decref` is safe on NULL (libcbor checks). `cbor_isa_array`/`cbor_isa_bytestring`/`cbor_isa_uint` exist in libcbor. Confirm `cbor_to_fibonacci_hit_counter` and `cbor_to_buffer` handle the validated items (they should, given the type checks). The `goto cleanup_null` keeps the decref bookkeeping centralized.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=CborToIndexEntry.*`
Expected: 2 PASS.

- [ ] **Step 5: Run the existing index tests to confirm no regression**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*`
Expected: existing tests still pass (a valid 5-element array still produces a valid entry).

- [ ] **Step 6: Commit**

```bash
git add src/BlockCache/index.c test/test_index.cpp
git commit -m "fix(index): validate CBOR shape in cbor_to_index_entry (no crash on malformed)"
```

---

### Task 3: `index_create` happy-path live-WAL replay + re-enable the crash-recovery test

**Files:**
- Modify: `src/BlockCache/index.c` (`index_create` `else` branch, lines 436-444)
- Modify: `test/test_index.cpp` (rename `DISABLED_TestWalCrashRecovery` → `TestWalCrashRecovery`)

- [ ] **Step 1: Re-enable the test (rename) and confirm it fails**

In `test/test_index.cpp` line 302, rename `DISABLED_TestWalCrashRecovery` → `TestWalCrashRecovery`. Run:

```bash
cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestWalCrashRecovery
```
Expected: FAIL — the test asserts all 8 entries are present after the simulated crash, but the happy-path `else` branch doesn't replay the live WAL, so only the 4 snapshotted entries are present.

- [ ] **Step 2: Add live-WAL replay to the happy-path branch**

In `src/BlockCache/index.c` `index_create`, the `else` branch (lines 436-444) currently:
```c
        } else {
          index->max_snapshots = max_snapshots;
          index->max_wals = max_wals;
          uint64_t first_kept_id_b = _index_prune_old_snapshots(index);
          _index_prune_old_wals(index, first_kept_id_b);
          free(index_location);
          free(parent_location);
          destroy_files(files);
          return index;
        }
```

The live WAL is the one paired with the valid snapshot — its id is `last_id + 1` (matching the one-id gap convention used in the rebuilding branch at line 289: `wal_load(parent_location, next_id)` where `next_id` comes from the snapshot file id). For the happy path, the valid snapshot is at index `i == files->length-1`, the newest file. Its id is `last_id`. The live WAL id is `last_id + 1`.

Replace the `else` branch with a replay of the live WAL before returning. Extract the replay into a helper to share with the rebuilding branch (Task 4 will harden the error handling in the same helper). Add a static helper above `index_create`:

```c
// Replay a WAL (by id) into the index. Returns 0 on clean EOF, or the
// wal_read error code on a recoverable error (caller decides stop-vs-bail).
// Stops at the last complete record on any error (stop-and-keep-prefix).
static int _index_replay_wal(index_t* index, const char* parent_location,
                             uint64_t wal_id) {
  wal_t* wal = wal_load(parent_location, wal_id);
  if (wal == NULL) return 0;  // no WAL file — nothing to replay
  wal_type_e type = 'r';
  buffer_t* data;
  uint64_t cursor;
  int32_t wal_size;
  int read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
  while (read_result == 0 && cursor <= (uint64_t)wal_size) {
    struct cbor_load_result result;
    cbor_item_t* cbor;
    switch (type) {
      case 'a':
        cbor = cbor_load(data->data, data->size, &result);
        if (result.error.code == CBOR_ERR_NONE) {
          index_entry_t* entry = cbor_to_index_entry(cbor);
          if (entry != NULL) {
            index_add(index, CONSUME(entry, index_entry_t));
          }
          cbor_decref(&cbor);
        } else {
          cbor_decref(&cbor);
          buffer_destroy(data);
          read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
          continue;  // CBOR parse failure on intact framing — skip, keep going
        }
        break;
      case 'i':
        cbor = cbor_load(data->data, data->size, &result);
        if (result.error.code == CBOR_ERR_NONE) {
          index_entry_t* entry = cbor_to_index_entry(cbor);
          if (entry != NULL) {
            index_entry_t* from_index = REFERENCE(index_find(index, entry->hash), index_entry_t);
            if (from_index != NULL) index_increment(index, from_index);
            DESTROY(entry, index_entry);
            DESTROY(from_index, index_entry);
          }
          cbor_decref(&cbor);
        } else {
          cbor_decref(&cbor);
        }
        break;
      case 'e':
        cbor = cbor_load(data->data, data->size, &result);
        if (result.error.code == CBOR_ERR_NONE && cbor_isa_array(cbor)) {
          cbor_item_t* cbor_hash = cbor_array_get(cbor, 0);
          cbor_item_t* cbor_date = cbor_array_get(cbor, 1);
          if (cbor_isa_bytestring(cbor_hash) && cbor_isa_uint(cbor_date)) {
            buffer_t* hash = cbor_to_buffer(cbor_hash);
            index_entry_t* entry = index_find(index, hash);
            index_entry_set_ejection_date(entry, cbor_get_int(cbor_date));
            DESTROY(hash, buffer);
          }
          cbor_decref(&cbor_hash);
          cbor_decref(&cbor_date);
        }
        cbor_decref(&cbor);
        break;
      case 'r':
        cbor = cbor_load(data->data, data->size, &result);
        if (result.error.code == CBOR_ERR_NONE && cbor_isa_bytestring(cbor)) {
          buffer_t* hash = cbor_to_buffer(cbor);
          index_remove(index, hash);
          DESTROY(hash, buffer);
        }
        cbor_decref(&cbor);
        break;
      default:
        // Unknown type — stop replay here, keep the prefix.
        buffer_destroy(data);
        DESTROY(wal, wal);
        return WAL_ERR_UNKNOWN_TYPE;
    }
    buffer_destroy(data);
    read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
  }
  DESTROY(wal, wal);
  // read_result == -3 (clean EOF) or a short read / CRC mismatch at the tail.
  // Stop-and-keep-prefix: return the code; the caller treats -3 as success
  // and anything else as "stop here, keep what we have" (NOT bail to empty).
  return read_result;
}
```

Then the happy-path `else` branch becomes:

```c
        } else {
          // Happy path: the newest snapshot is valid. Replay the live WAL
          // (id == last_id + 1) to recover writes since the last snapshot.
          // Stop-and-keep-prefix: a torn tail or CRC error loses only the
          // bad record, not the whole index.
          _index_replay_wal(index, parent_location, last_id + 1);
          index->max_snapshots = max_snapshots;
          index->max_wals = max_wals;
          uint64_t first_kept_id_b = _index_prune_old_snapshots(index);
          _index_prune_old_wals(index, first_kept_id_b);
          free(index_location);
          free(parent_location);
          destroy_files(files);
          return index;
        }
```

Note: `last_id` is the id of the valid snapshot file at index `i` — confirm it's in scope at the `else` branch (it's computed earlier in the iteration via `_index_get_id_crc`; read the surrounding code to get the exact variable name). The rebuilding branch uses `next_id` from `files->data[j]`'s id; for the happy path, the live WAL id is `last_id + 1` per the one-id-gap convention.

IMPORTANT: the helper above handles CBOR parse failures by SKIPPING the record (the `continue` in `case 'a'`), and unknown types by STOPPING. This is the stop-and-keep-prefix policy from the spec. Task 4 will refactor the rebuilding branch (lines 282-412) to use the same helper so both paths share the error handling.

- [ ] **Step 3: Run the re-enabled test**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=TestWalCrashRecovery`
Expected: PASS — all 8 entries present after the simulated crash.

- [ ] **Step 4: Run the full index suite for regressions**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*`
Expected: no regression.

- [ ] **Step 5: Commit**

```bash
git add src/BlockCache/index.c test/test_index.cpp
git commit -m "fix(index): replay live WAL on the happy path; re-enable TestWalCrashRecovery"
```

---

### Task 4: Stop-and-keep-prefix error handling in the rebuilding branch

**Files:**
- Modify: `src/BlockCache/index.c` (refactor the rebuilding branch lines 282-412 to use `_index_replay_wal`; replace the bail-to-empty at 404-411)
- Test: `test/test_index.cpp` (torn-tail, CRC-corruption, unknown-type, CBOR-parse-failure, total-loss tests)

- [ ] **Step 1: Write the failing tests**

Add to `test/test_index.cpp`. These build a WAL with a known corruption and assert recovery keeps the prefix.

```cpp
TEST(IndexRecovery, TornTailKeepsPrefix) {
  // Snapshot has entries 0..3; live WAL has a valid entry 4 then a torn
  // (truncated) record. Reopen asserts entries 0..4 present, 5..7 absent.
  // (Build the WAL by writing entries 4..7, then truncate the file mid-record
  //  for entry 5. Use platform_file_truncate or fopen+ftruncate.)
  // ... fixture setup like TestWalCrashRecovery ...
  // Assert index_find for entries 0..4 != NULL, 5..7 == NULL (or absent).
  GTEST_SKIP() << "Implement once _index_replay_wal is shared — needs WAL truncation helper.";
}

TEST(IndexRecovery, CrcCorruptionKeepsPrefix) {
  // Snapshot 0..3; WAL with valid entry 4 then a record whose payload byte
  // is flipped (CRC mismatch). Reopen asserts 0..4 present, the corrupt
  // record and everything after absent.
  GTEST_SKIP() << "Implement with _index_replay_wal — needs in-place WAL byte flip.";
}

TEST(IndexRecovery, UnknownTypeStopsReplay) {
  // Snapshot 0..3; WAL with valid entry 4 then a record with type='x'.
  // Reopen asserts 0..4 present, the rest absent.
  GTEST_SKIP() << "Implement with _index_replay_wal + Task 1's unknown-type support.";
}

TEST(IndexRecovery, TotalLossReturnsEmpty) {
  // No valid snapshot + unreadable WAL → fresh empty index (not a crash).
  GTEST_SKIP() << "Implement — corrupt all snapshots, assert _index_new_empty returned.";
}
```

These tests need helpers to write a snapshot + a WAL with specific corruption. The `GTEST_SKIP` placeholders are acceptable for the first pass IF the underlying `_index_replay_wal` behavior is covered by the re-enabled `TestWalCrashRecovery` (clean case) — but the corruption cases are the real value. Implement at least `TornTailKeepsPrefix` and `CrcCorruptionKeepsPrefix` for real (non-skip) coverage; the helper to truncate/flip WAL bytes is a small `fseek`/`ftruncate`/`fwrite` on the WAL file path. If time-boxed, ship the skips with a follow-up note and ensure the re-enabled `TestWalCrashRecovery` + the `WalRead.UnknownTypeByteReturnsErrorNotCrash` test cover the core paths.

- [ ] **Step 2: Refactor the rebuilding branch to use `_index_replay_wal`**

In `src/BlockCache/index.c`, the rebuilding branch (lines 282-412) has its own inline `wal_read` loop. Replace the inner WAL-replay loop (lines 289-401) and the bail-to-empty error response (404-411) with calls to `_index_replay_wal`. For each `j = i+1 .. files->length-1`, call `_index_replay_wal(index, parent_location, next_id)` where `next_id` is the id of `files->data[j]`. On any return that isn't `-3` (clean EOF) — i.e. a torn tail, CRC mismatch, unknown type — STOP the rebuild loop (break out of the `for j` loop), keep the index as-is (snapshot + whatever earlier WALs replayed), and proceed to return the index (NOT `_index_new_empty`). Only if the FIRST valid snapshot's replay fails catastrophically (and no earlier snapshot exists) fall back to `_index_new_empty`.

Concretely, replace lines 289-411 with:

```c
          for (int j = i + 1; j < files->length; j++) {
            char* next = files->data[j];
            uint64_t next_id = 0;
            uint64_t next_crc = 0;
            _index_get_id_crc(next, &next_id, &next_crc);
            int replay_result = _index_replay_wal(index, parent_location, next_id);
            // Stop-and-keep-prefix: any non-clean-EOF result stops the rebuild
            // here. The snapshot + earlier WALs are kept; later WALs are not
            // replayed (they may depend on the lost record). Do NOT bail to
            // _index_new_empty — that would discard the valid snapshot.
            if (replay_result != -3) {
              log_warn("index_create: WAL %lu replay stopped at code %d (stop-and-keep-prefix)",
                       (unsigned long)next_id, replay_result);
              break;
            }
          }
          // (the existing index->is_rebuilding = 0; current_id; next_id;
          //  prune; return index; block at 414-435 stays as-is)
```

This removes the `DESTROY(index, index); return _index_new_empty(...)` bail. The index built so far (valid snapshot + replayed prefix) is returned. `_index_new_empty` is now only reached when ALL snapshots are invalid (the fallthrough at the end of the loop, line 448+) — true total loss.

- [ ] **Step 3: Run the re-enabled test + the new tests**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=IndexRecovery.*:TestWalCrashRecovery:*Index*`
Expected: `TestWalCrashRecovery` PASS; the implemented (non-skip) `IndexRecovery.*` tests PASS; no regression.

- [ ] **Step 4: Commit**

```bash
git add src/BlockCache/index.c test/test_index.cpp
git commit -m "fix(index): stop-and-keep-prefix WAL recovery (no more bail-to-empty)"
```

---

### Task 5: `index_debounce` write-return check + snapshot fsync + old-WAL fsync

**Files:**
- Modify: `src/BlockCache/index.c` (`index_debounce`, lines 1133-1141)
- Modify: `src/BlockCache/index.h` — `index_t` gains `bool fsync_data` (Task 6 plumbs it; here just read it).
- Test: `test/test_index.cpp`

- [ ] **Step 1: Write the failing test**

A direct test of `index_debounce`'s fsync is hard (fsync is a no-op-ish syscall to observe). Instead test the write-return-check path: mock an ENOSPC by filling the disk is impractical, so test at the level of "index_debounce with fsync_data=false does not crash and produces a valid snapshot" + "index_debounce checks the write return" via a test that sets `index->fsync_data` and calls `index_debounce` then reopens. The re-enabled `TestWalCrashRecovery` already exercises debounce-then-reopen. Add a targeted test only if cheap; otherwise rely on the existing suite + a valgrind check.

- [ ] **Step 2: Fix `index_debounce`**

In `src/BlockCache/index.c` `index_debounce` (lines 1133-1141), replace:

```c
  uint8_t *cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  platform_file_t* index_file = platform_file_open(file, PLATFORM_O_WRONLY | PLATFORM_O_CREAT, 0644);
  platform_file_write(index_file, cbor_data, cbor_size);
  platform_file_close(index_file);
  free(cbor_data);
  free(file);
  wal_destroy(wal);
```

with:

```c
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  platform_file_t* index_file = platform_file_open(file, PLATFORM_O_WRONLY | PLATFORM_O_CREAT, 0644);
  ssize_t written = platform_file_write(index_file, cbor_data, cbor_size);
  int sync_rc = 0;
  if (written == (ssize_t)cbor_size && index->fsync_data) {
    sync_rc = platform_file_sync(index_file);
  }
  platform_file_close(index_file);
  free(cbor_data);
  free(file);
  if (written != (ssize_t)cbor_size || sync_rc != 0) {
    // Snapshot write failed. Do NOT destroy the old WAL — it still holds the
    // entries that the (now-missing/empty) snapshot was supposed to capture.
    // Leave index->wal as the OLD wal (revert the rollover) and return.
    log_error("index_debounce: snapshot write/sync failed (written=%zd sync=%d) — keeping old WAL",
              written, sync_rc);
    wal_destroy(index->wal);       // destroy the just-created empty new WAL
    index->wal = wal;              // revert to the old WAL
    index->current_file = index->last_file;  // revert current_file (best-effort)
    index->last_file = NULL;
    cbor_intermediate_decref(cbor);
    return;
  }
  // Snapshot is durable. fsync the old WAL (its entries are now in the
  // snapshot) before destroying it, so a crash here doesn't lose them.
  if (index->fsync_data) {
    wal_sync(wal);
  }
  wal_destroy(wal);
```

Note: the revert path is best-effort — `index->current_file` was reassigned at line 1128; reverting it to `index->last_file` (which was just set to the old current_file at line 1124) restores the pre-debounce state approximately. Read lines 1121-1131 carefully to get the revert right; the key invariant is: on snapshot-write failure, the old WAL must survive (not be destroyed) so its entries can be replayed on the next reopen. Confirm `wal_sync` is declared (wal.h:32).

- [ ] **Step 3: Run the index suite + valgrind**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*` + valgrind on `TestWalCrashRecovery`.
Expected: no regression, 0 leaks.

- [ ] **Step 4: Commit**

```bash
git add src/BlockCache/index.c
git commit -m "fix(index): fsync snapshot + old WAL in index_debounce, check write return"
```

---

### Task 6: `fsync_data` config flag + plumbing through the layer chain

**Files:**
- Modify: `src/Configuration/config.h`, `config.c` (add `bool fsync_data`)
- Modify: `src/BlockCache/block_cache.h` (`block_cache_t` field), `block_cache.c` (`block_cache_create` plumb)
- Modify: `src/BlockCache/sections.h` (`sections_t` field), `sections.c` (`sections_create` plumb + `section_create`)
- Modify: `src/BlockCache/section.h` (`section_t` field), `section.c` (`section_create` set)
- Modify: `src/BlockCache/index.h` (`index_t` field), `index.c` (`index_create`/`_index_new_empty`/`index_create_from` set)

- [ ] **Step 1: Add the config field**

In `src/Configuration/config.h`, after `peer_state_save_interval_ms` (line 36):
```c
  bool fsync_data;                    // fsync block data + WAL + snapshot for crash durability (default true; false for fast test builds)
```

In `src/Configuration/config.c` `config_default`, after `peer_state_save_interval_ms` (line 39):
```c
  config.fsync_data = true;
```

No `config_validate` clause needed (bool). If `config_json.c` parses config from JSON, add a `cJSON_IsTrue`/`cJSON_IsNumber` entry for `fsync_data` — check whether that file exists and follow its pattern.

- [ ] **Step 2: Add `fsync_data` to the structs and plumb**

Add `bool fsync_data;` to:
- `block_cache_t` (block_cache.h, after `type` or near the end of the struct).
- `sections_t` (sections.h).
- `section_t` (section.h).
- `index_t` (index.h).

In `block_cache_create` (block_cache.c:672, which takes `config_t config` by value), set `block_cache->fsync_data = config.fsync_data;` and pass it to `sections_create(...)` and to the index after `index_create` (`block_cache->index->fsync_data = config.fsync_data;`). Read `block_cache_create` to find the `sections_create` and `index_create` call sites.

In `sections_create`, accept `bool fsync_data` and set `sections->fsync_data = fsync_data;`, and pass it to each `section_create` call (set `section->fsync_data = sections->fsync_data`).

In `index_create` / `_index_new_empty` / `index_create_from` — rather than changing their signatures (they take scalar params), set `index->fsync_data` from `block_cache_create` after the index is created: `block_cache->index->fsync_data = config.fsync_data;`. This avoids a wide signature change. Confirm `_index_new_empty` doesn't reset it (it creates a fresh index — set it there too if `_index_new_empty` is reached in production recovery, OR set it from `block_cache` after `index_create` returns and rely on the fact that `_index_new_empty` is only called inside `index_create` which returns to `block_cache_create`). Simplest: `block_cache->index->fsync_data = config.fsync_data;` right after `index_create` in `block_cache_create`.

- [ ] **Step 3: Default `fsync_data = true` in tests that don't go through `block_cache_create`**

The `TestIndex` fixture in `test/test_index.cpp` calls `index_create` directly (no `block_cache_create`), so `index->fsync_data` would be 0 (false) by default (zeroed struct) — which is actually what the fast-test path wants (no fsync → fast tests). But the `index_debounce` fsync (Task 5) and SECTION_WRITE fsync (Task 7) are gated on `fsync_data`, so tests get the fast path automatically. Confirm the `TestIndex` fixture's `index_create` calls produce `fsync_data = false` (zeroed) → tests stay fast. If a test WANTS fsync, it sets `index->fsync_data = true` explicitly. Document this in the fixture.

- [ ] **Step 4: Build + run the full BlockCache suite**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*:*Block*:*Section*`
Expected: no regression.

- [ ] **Step 5: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c src/BlockCache/block_cache.h src/BlockCache/block_cache.c src/BlockCache/sections.h src/BlockCache/sections.c src/BlockCache/section.h src/BlockCache/section.c src/BlockCache/index.h src/BlockCache/index.c
git commit -m "feat(config): add fsync_data flag, plumb through block_cache/sections/section/index"
```

---

### Task 7: SECTION_WRITE fsync after pwrite

**Files:**
- Modify: `src/BlockCache/section.c` (`SECTION_WRITE`, after line 451)

- [ ] **Step 1: Add the fsync**

In `src/BlockCache/section.c` `SECTION_WRITE` (line 419-472), after the successful `platform_file_pwrite` and the short-write check (lines 445-451), before `atomic_store(&section->dirty, 1)` (line 452):

```c
      if (section->fsync_data) {
        platform_file_sync(section->file);
      }
```

This fsyncs the block data BEFORE the WAL entry is acknowledged (the WAL fsync happens at `block_cache_sync`/shutdown; the data-before-WAL ordering is what makes the entry durable). Per the spec: data fsync before WAL fsync.

- [ ] **Step 2: Build + run**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Block*:*Section*`
Expected: no regression (tests have `fsync_data = false` by default → no fsync → fast).

- [ ] **Step 3: Commit**

```bash
git add src/BlockCache/section.c
git commit -m "feat(section): fsync block data after pwrite (gated by fsync_data)"
```

---

### Task 8: `section_save_meta` fsync

**Files:**
- Modify: `src/BlockCache/section.c` (`section_save_meta`, lines 853-869)

- [ ] **Step 1: Add the fsync**

In `src/BlockCache/section.c` `section_save_meta` (line 853-869), before `platform_file_close(meta_file)` (line 867):

```c
  if (section->fsync_data) {
    platform_file_sync(meta_file);
  }
```

The write-return check at line 864 already logs on failure; keep it. (Making the function return an error would require changing its signature + callers — out of scope; the fsync + existing log is the durability fix.)

- [ ] **Step 2: Build + run**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Section*`
Expected: no regression.

- [ ] **Step 3: Commit**

```bash
git add src/BlockCache/section.c
git commit -m "feat(section): fsync section meta file (gated by fsync_data)"
```

---

### Task 9: `round_robin_save` fwrite-return check + fsync (or atomic write)

**Files:**
- Modify: `src/BlockCache/sections.c` (`round_robin_save`, lines 689-709)

- [ ] **Step 1: Fix `round_robin_save`**

In `src/BlockCache/sections.c` `round_robin_save` (lines 689-709), replace the stdio write with `platform_file_atomic_write` (from Stage 1) + check the return. This reuses the atomic primitive and gives both crash-safety and the write-return check. Add `#include "../Platform/platform_atomic.h"` to sections.c.

```c
void round_robin_save(void* ctx) {
  round_robin_t* robin = (round_robin_t*)ctx;
  cbor_item_t* cbor = round_robin_to_cbor(robin);
  if (cbor == NULL) {
    log_error("Failed to save robin file");
    return;
  }
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  int rc = platform_file_atomic_write(robin->path, cbor_data, cbor_size);
  if (rc != 0) {
    log_error("round_robin_save: atomic write failed (rc=%d) for %s", rc, robin->path);
  }
  free(cbor_data);
  cbor_decref(&cbor);
}
```

`platform_file_atomic_write` already fsyncs the file + parent dir (Stage 1), so no separate fsync needed. Confirm `robin->path` is a `char*` (it is — quoted at sections.c:699). Confirm `platform_atomic.h` is reachable from sections.c (add the include).

- [ ] **Step 2: Build + run**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Section*:*Block*`
Expected: no regression.

- [ ] **Step 3: Commit**

```bash
git add src/BlockCache/sections.c
git commit -m "feat(sections): atomic + fsync round_robin_save via platform_file_atomic_write"
```

---

### Task 10: `block_cache_sync` fsync section data + snapshot at shutdown

**Files:**
- Modify: `src/BlockCache/block_cache.c` (`block_cache_sync`, lines 866-882)

- [ ] **Step 1: Extend `block_cache_sync`**

`block_cache_sync` currently only flushes the debounce (writes snapshot un-fsynced — fixed in Task 5) and `wal_sync`. Extend it to also fsync the section data files + the snapshot, so a shutdown crash doesn't lose them. After the `index_sync(block_cache->index)` call (line 881), add:

```c
  // fsync section data files so the blocks referenced by the (now-durable)
  // WAL/index are also on disk. data-before-WAL ordering is maintained per
  // SECTION_WRITE (Task 7); this is the shutdown belt-and-suspenders.
  if (block_cache->fsync_data && block_cache->sections != NULL) {
    sections_sync(block_cache->sections);  // see below
  }
```

Add a `sections_sync(sections_t* sections)` helper in `src/BlockCache/sections.c` (declared in `sections.h`) that iterates the open sections and calls `platform_file_sync(section->file)` on each non-NULL `section->file`, plus `platform_file_sync` on any meta handles. Match the existing iteration pattern in `sections.c` (look at how `sections_dispatch` or the LRU iterates). If iterating all open sections is complex, a simpler shutdown sync: fsync the robin file (already atomic via Task 9) and rely on per-write SECTION_WRITE fsync (Task 7) for the data files — in which case `block_cache_sync` only needs the existing WAL sync + the Task 5 snapshot fsync (already done in `index_debounce`). In that case, this task reduces to "confirm the shutdown chain is complete" and no new `sections_sync` is needed.

DECISION: prefer the simpler path — the per-write SECTION_WRITE fsync (Task 7) + the debounce snapshot fsync (Task 5) + the WAL sync (existing) already give shutdown durability. `block_cache_sync`'s existing `index_sync` (WAL) + the Task 5 debounce flush (which now fsyncs the snapshot) is sufficient. SKIP adding `sections_sync` unless a test shows a gap. Document this decision in the commit message.

- [ ] **Step 2: Build + run the full suite + valgrind**

Run: `cd build-test && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*:*Block*:*Section*` + valgrind on `TestWalCrashRecovery`.
Expected: no regression, 0 leaks.

- [ ] **Step 3: Commit (if any change; otherwise note the decision)**

If no code change, skip the commit and note in the Task 11 commit message that Task 10 was a no-op (the per-write + debounce fsyncs suffice). If `sections_sync` was added, commit it.

---

### Task 11: De-wonk + ASAN + valgrind + full BlockCache suite

**Files:** none (verification only)

- [ ] **Step 1: Run the de-wonk skill on the Stage 2 changes**

Invoke de-wonk. Fix any unimplemented/stubbed/disabled/broken/weird code. No TODOs.

- [ ] **Step 2: ASAN build + run the BlockCache suite**

Run: `cd build-asan && cmake --build . --target testliboffs && ./test/testliboffs --gtest_filter=*Index*:*Block*:*Section*:*Wal*`
Expected: PASS, no ASAN errors.

- [ ] **Step 3: Valgrind (DWARF-4) on the BlockCache suite**

Run: `cd build-gdwarf4 && cmake --build . --target testliboffs && valgrind --leak-check=full --error-exitcode=1 ./test/testliboffs --gtest_filter=*Index*:*Block*:*Section*:*Wal*`
Expected: 0 leaks, 0 errors.

- [ ] **Step 4: Commit any de-wonk fixes**

```bash
git add -A
git commit -m "test: de-wonk + valgrind-clean for storage-durability pass"
```

---

## Self-Review

**Spec coverage (Section 5):**
- 5.1 WAL replay on happy path → Task 3. ✓
- 5.2 fsync ordering (data→WAL→snapshot) → Tasks 5 (snapshot fsync + old-WAL fsync), 7 (data fsync), 6 (flag). ✓
- 5.3 recovery-path input validation + WAL error handling (CRC verify exists; stop-and-keep-prefix; type-byte default; CBOR shape) → Tasks 1, 2, 4. ✓
- 5.4 tests → re-enabled `TestWalCrashRecovery` + new `IndexRecovery.*` + `WalRead.*` + `CborToIndexEntry.*`. ✓

**Key risks flagged inline:**
- Task 3's `last_id + 1` live-WAL id — confirm the variable name in scope at the `else` branch (read the surrounding code).
- Task 4's rebuilding-branch refactor — preserve the `index->is_rebuilding = 0; current_id; next_id; prune; return index;` block (lines 414-435) that follows the WAL loop.
- Task 5's revert path — the `index->current_file`/`last_file` revert is best-effort; the load-bearing invariant is "old WAL not destroyed on snapshot-write failure."
- Task 6's plumbing — `index->fsync_data` set from `block_cache_create` after `index_create` (no signature change); tests get `fsync_data = false` (zeroed) → fast path.
- Task 4's `IndexRecovery.*` tests — the `GTEST_SKIP` placeholders are acceptable for the first pass IF the core paths are covered by `TestWalCrashRecovery` + `WalRead.UnknownTypeByteReturnsErrorNotCrash`; implement at least `TornTailKeepsPrefix` + `CrcCorruptionKeepsPrefix` for real coverage if feasible.

**Type consistency:** `_index_replay_wal` is introduced in Task 3 and reused in Task 4. `WAL_ERR_UNKNOWN_TYPE` introduced in Task 1, used in Tasks 3/4. `fsync_data` field name consistent across all structs (Task 6).