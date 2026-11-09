# Anti-Entropy Divergence Repair over Ring Gossip — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a periodic bloom-filter digest + pull-the-difference exchange between one random connected peer pair per tick, so ring-membership divergence converges in bounded time.

**Architecture:** New stateless module `src/Network/anti_entropy.c/.h` (bloom build/check + novel-id collection over ring sets), three new wire messages (28–30) in `wire.c`, handlers on the existing network protocol actor wired to a new slow timer (`NETWORK_ANTI_ENTROPY_TICK`). Insertions reuse the existing `network_add_node_to_ring()` path. Spec: `docs/superpowers/specs/2026-11-02-anti-entropy-gossip-design.md`.

**Tech Stack:** C (C11 atomics via `src/Util/atomic_compat.h`), libcbor, XXH3 (`deps/xxHash`, include `<xxh3.h>`, include path already on the `offs` target), googletest.

**Conventions:** Follow `docs/STYLE_GUIDE.md` and CLAUDE.md — descriptive variable names (never single letters), no TODO/FIXME in completed work, conventional commits, `_t` types, `type_action()` naming. Create a harmony ticket for this plan at execution start; close it in the final task. Run tests one at a time (no parallel test or valgrind processes). Build/test directory: `build-test`; valgrind directory: `build-vg` (`-gdwarf-4`; valgrind 3.18 crashes on DWARF5).

**Build/test commands used throughout:**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/test/testliboffs --gtest_filter='AntiEntropyTest.*'
```

(NOTE added during Task 1 execution: the gtest binary lives at `build-test/test/testliboffs`. Also, `file(GLOB_RECURSE C_SRC)` has no `CONFIGURE_DEPENDS` — run `cmake -S . -B build-test` after adding any new .c file, and use the same `build-vg/test/` path under valgrind.)

**Distinct-id rule for tests (added during Task 1 execution):** `node_id_from_string()` is a *base58 decoder* — on a non-base58 string it calls `node_id_clear()` and returns -1, so every plain name ("node-ae-a1"…) yields the all-zero id. Any test that needs distinct ids must use the file-local helper installed by Task 1:

```cpp
static void anti_entropy_fill_id(node_id_t* id, uint8_t marker, int index) {
  memset(id, 0, sizeof(*id));
  id->hash[0] = marker;
  id->hash[1] = (uint8_t)(index & 0xFF);
  id->hash[2] = (uint8_t)((index >> 8) & 0xFF);
}
```

(Equality-only assertions on a zero id are still valid round-trip checks, but prefer distinct ids everywhere.)

The `offs` library globs `src/*/*.c` (`CMakeLists.txt:89`), so the new module needs **no** CMake change. `test_network.cpp` is compiled into `testliboffs` (`test/CMakeLists.txt:114`), so its new tests live there; config tests live in `test/test_config_validate.cpp`.

**Test-resource rules for every test in this plan:** `ring_set_destroy` frees only the ring pointer vectors, never the `net_node_t` nodes — every node a test creates must be released with `net_node_destroy` before the test ends, mirroring the pattern in `FindBlockTest` (`test/test_network.cpp:1195`). A file-local helper is provided in Task 1 for that cleanup.

---

### Task 1: `anti_entropy.h/.c` — bloom + novel-id collection (TDD)

**Files:**
- Create: `src/Network/anti_entropy.h`
- Create: `src/Network/anti_entropy.c`
- Modify: `test/test_network.cpp` — add `#include "Network/anti_entropy.h"` next to `#include "Network/gossip.h"` (line 18), add the fixture and tests at the end of the file

- [ ] **Step 1: Write the failing tests** — append to `test/test_network.cpp`:

```cpp
// === Anti-entropy digest bloom + novel-id collection ===

static void anti_entropy_destroy_nodes(net_node_t* const* members, size_t count) {
  for (size_t index = 0; index < count; index++) {
    net_node_destroy(members[index]);
  }
}

TEST_F(AntiEntropyTest, BloomBuildMatchesInsertedMembers) {
  net_node_t* members[3] = { NULL, NULL, NULL };
  for (int index = 0; index < 3; index++) {
    node_id_t id;
    anti_entropy_fill_id(&id, 0x51, index);
    members[index] = net_node_create(&id, 0x0A000001, 8080);
    ASSERT_NE(members[index], nullptr);
    ASSERT_EQ(ring_set_insert(rings, members[index], (uint32_t)(5000 * (index + 1))), 0);
  }

  uint8_t bloom[ANTI_ENTROPY_BLOOM_BYTES] = {};
  size_t count = anti_entropy_bloom_build(rings, bloom);
  EXPECT_EQ(count, 3u);

  for (int index = 0; index < 3; index++) {
    EXPECT_TRUE(anti_entropy_bloom_check(bloom, &members[index]->id));
  }

  anti_entropy_destroy_nodes(members, 3);
}

TEST_F(AntiEntropyTest, BloomEmptySetBuildsZeroFilter) {
  uint8_t bloom[ANTI_ENTROPY_BLOOM_BYTES] = {};
  EXPECT_EQ(anti_entropy_bloom_build(rings, bloom), 0u);
  for (size_t byte_index = 0; byte_index < ANTI_ENTROPY_BLOOM_BYTES; byte_index++) {
    ASSERT_EQ(bloom[byte_index], 0) << "byte " << byte_index;
  }
}

TEST_F(AntiEntropyTest, BloomFalsePositiveRateSane) {
  node_id_t member_ids[120];
  net_node_t* members[120];
  // Each ring holds 8 primary + 4 secondary = 12 members; a fixed latency
  // would park all 120 in one ring and overflow after 12 inserts, so cycle
  // the latency across all 10 rings (12 members each) to fill the set.
  for (int index = 0; index < 120; index++) {
    int ring_slot = index % 10;
    uint32_t latency_us = (ring_slot == 0) ? 1000u
        : (uint32_t)(5000u << (ring_slot - 1));
    anti_entropy_fill_id(&member_ids[index], 0x52, index);
    members[index] = net_node_create(&member_ids[index], 0x0A000001, 8080);
    ASSERT_NE(members[index], nullptr);
    ASSERT_EQ(ring_set_insert(rings, members[index], latency_us), 0);
  }

  uint8_t bloom[ANTI_ENTROPY_BLOOM_BYTES] = {};
  ASSERT_EQ(anti_entropy_bloom_build(rings, bloom), 120u);

  node_id_t absent_id;
  int false_positives = 0;
  for (int index = 0; index < 200; index++) {
    anti_entropy_fill_id(&absent_id, 0x53, index);
    if (anti_entropy_bloom_check(bloom, &absent_id)) false_positives++;
  }
  // Every inserted member must test present.
  for (int index = 0; index < 120; index++) {
    EXPECT_TRUE(anti_entropy_bloom_check(bloom, &member_ids[index]));
  }
  // Expected FP at m=4096 bits, k=3, n=120 is ~0.06% per probe (~0.12 of 200).
  EXPECT_LE(false_positives, 2);

  anti_entropy_destroy_nodes(members, 120);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='AntiEntropyTest.*'
```

Expected: **compile error** — `Network/anti_entropy.h: No such file or directory`.

- [ ] **Step 3: Write `src/Network/anti_entropy.h`**

```c
//
// Created by victor on 11/2/26.
//

#ifndef OFFS_ANTI_ENTROPY_H
#define OFFS_ANTI_ENTROPY_H

#include "ring_set.h"
#include "node_id.h"
#include <stddef.h>
#include <stdbool.h>

// Bloom filter spec for anti-entropy digests. ~120 ring members max
// (10 rings x (8 primary + 4 secondary)), k=3, m=4096 bits -> FP ~0.06%.
// A false positive only drops one repair candidate, which the next tick
// retries, so oversizing further buys nothing.
#define ANTI_ENTROPY_BLOOM_BYTES 512
#define ANTI_ENTROPY_BLOOM_BITS ((size_t)ANTI_ENTROPY_BLOOM_BYTES * 8)
#define ANTI_ENTROPY_HASH_COUNT 3
#define ANTI_ENTROPY_MAX_IDS RING_MAX_RINGS

// Same XXH3 double-hash seed convention as closest_nodes.c / find_block.c.
#define ANTI_ENTROPY_SEED_A 0x4F464653ULL          // "OFFS"
#define ANTI_ENTROPY_SEED_B 0x46534E4F55444E53ULL  // "FSOUNDS"

// Raw primitives: operate on a caller-owned bloom byte array.
void anti_entropy_bloom_set(uint8_t* bloom, const uint8_t* data, size_t len);
bool anti_entropy_bloom_check_raw(const uint8_t* bloom, const uint8_t* data, size_t len);

// Build a digest bloom over every ring member (primary + secondary of all
// rings). Returns the number of member ids hashed in.
size_t anti_entropy_bloom_build(const ring_set_t* rings, uint8_t* bloom);

// Membership test of a single id against a digest bloom.
bool anti_entropy_bloom_check(const uint8_t* bloom, const node_id_t* id);

// Enumerate ids in `mine` (all rings) that test absent in `their_bloom`,
// capped at max_ids. These are the ids the remote peer probably lacks.
size_t anti_entropy_collect_novel(const ring_set_t* mine,
                                  const uint8_t* their_bloom,
                                  node_id_t* out_ids,
                                  size_t max_ids);

#endif // OFFS_ANTI_ENTROPY_H
```

- [ ] **Step 4: Write `src/Network/anti_entropy.c`**

```c
//
// Created by victor on 11/2/26.
//

#include "anti_entropy.h"
#include "wire.h"
#include <xxh3.h>

_Static_assert(WIRE_ANTI_ENTROPY_BLOOM_BYTES == ANTI_ENTROPY_BLOOM_BYTES,
               "wire and anti_entropy bloom sizes must match");
_Static_assert(WIRE_ANTI_ENTROPY_MAX_IDS == ANTI_ENTROPY_MAX_IDS,
               "wire and anti_entropy max ids must match");

void anti_entropy_bloom_set(uint8_t* bloom, const uint8_t* data, size_t len) {
  if (bloom == NULL || data == NULL) return;
  uint64_t hash_a = XXH3_64bits_withSeed(data, len, ANTI_ENTROPY_SEED_A);
  uint64_t hash_b = XXH3_64bits_withSeed(data, len, ANTI_ENTROPY_SEED_B);

  for (uint32_t index = 0; index < ANTI_ENTROPY_HASH_COUNT; index++) {
    uint64_t combined = hash_a + (uint64_t)index * hash_b;
    size_t bit_index = (size_t)(combined % ANTI_ENTROPY_BLOOM_BITS);
    size_t byte_index = bit_index / 8;
    uint8_t bit_offset = (uint8_t)(bit_index % 8);
    bloom[byte_index] |= (uint8_t)(1u << bit_offset);
  }
}

bool anti_entropy_bloom_check_raw(const uint8_t* bloom, const uint8_t* data, size_t len) {
  if (bloom == NULL || data == NULL) return false;
  uint64_t hash_a = XXH3_64bits_withSeed(data, len, ANTI_ENTROPY_SEED_A);
  uint64_t hash_b = XXH3_64bits_withSeed(data, len, ANTI_ENTROPY_SEED_B);

  for (uint32_t index = 0; index < ANTI_ENTROPY_HASH_COUNT; index++) {
    uint64_t combined = hash_a + (uint64_t)index * hash_b;
    size_t bit_index = (size_t)(combined % ANTI_ENTROPY_BLOOM_BITS);
    size_t byte_index = bit_index / 8;
    uint8_t bit_offset = (uint8_t)(bit_index % 8);
    if (!(bloom[byte_index] & (uint8_t)(1u << bit_offset))) {
      return false;
    }
  }
  return true;
}

bool anti_entropy_bloom_check(const uint8_t* bloom, const node_id_t* id) {
  if (bloom == NULL || id == NULL) return false;
  return anti_entropy_bloom_check_raw(bloom, id->hash, NODE_ID_HASH_SIZE);
}

size_t anti_entropy_bloom_build(const ring_set_t* rings, uint8_t* bloom) {
  if (rings == NULL || bloom == NULL) return 0;
  size_t count = 0;
  for (size_t ring_index = 0; ring_index < rings->ring_count; ring_index++) {
    const ring_t* ring = &rings->rings[ring_index];
    for (size_t index = 0; index < ring->primary.length; index++) {
      net_node_t* node = ring->primary.data[index];
      if (node == NULL) continue;
      anti_entropy_bloom_set(bloom, node->id.hash, NODE_ID_HASH_SIZE);
      count++;
    }
    for (size_t index = 0; index < ring->secondary.length; index++) {
      net_node_t* node = ring->secondary.data[index];
      if (node == NULL) continue;
      anti_entropy_bloom_set(bloom, node->id.hash, NODE_ID_HASH_SIZE);
      count++;
    }
  }
  return count;
}

size_t anti_entropy_collect_novel(const ring_set_t* mine,
                                  const uint8_t* their_bloom,
                                  node_id_t* out_ids,
                                  size_t max_ids) {
  if (mine == NULL || their_bloom == NULL || out_ids == NULL) return 0;
  size_t count = 0;
  for (size_t ring_index = 0;
       ring_index < mine->ring_count && count < max_ids;
       ring_index++) {
    const ring_t* ring = &mine->rings[ring_index];
    // Two passes per ring: primary members, then secondary overflow.
    const vec_node_t* member_passes[2] = { &ring->primary, &ring->secondary };
    for (size_t pass_index = 0; pass_index < 2 && count < max_ids; pass_index++) {
      const vec_node_t* members = member_passes[pass_index];
      for (size_t index = 0; index < members->length && count < max_ids; index++) {
        net_node_t* node = members->data[index];
        if (node == NULL) continue;
        if (anti_entropy_bloom_check(their_bloom, &node->id)) continue;
        out_ids[count++] = node->id;
      }
    }
  }
  return count;
}
```

Note: `wire.h` with the new constants from Task 2 does not exist yet, so the `_Static_assert`s will not compile at this step — write them but the header `wire.h` edit below (Task 2 Step 3) is what makes them true. To keep Task 1 self-contained and green before Task 2, guard the asserts temporarily? No — instead, perform Task 2 **Step 3 (wire.h additions) together with Task 1 Step 4** in the same edit pass: the wire structs land in wire.h now, only the codecs wait for Task 2. Concretely, apply the Task 2 wire.h diff immediately after writing anti_entropy.c, then build. The codec tests of Task 2 still fail on missing symbols until its Step 4 — that preserves Task 2's red step.

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='AntiEntropyTest.*'
```

Expected: all `AntiEntropyTest.*` PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Network/anti_entropy.h src/Network/anti_entropy.c src/Network/wire.h test/test_network.cpp
git commit -m "feat(network): anti-entropy bloom digest and novel-id collection"
```

---

### Task 2: Wire codecs (TDD)

**Files:**
- Modify: `src/Network/wire.h` — structs already landed in Task 1 (verify them there); this task adds the six declarations after `wire_gossip_pull_decode` (line ~529)
- Modify: `src/Network/wire.c` — codecs after `wire_gossip_pull_decode`, ~line 2355
- Modify: `test/test_network.cpp` — codec fixture + tests at end

- [ ] **Step 1: Add codec declarations to `src/Network/wire.h`** — right after the `wire_gossip_pull_decode` declaration:

```c
cbor_item_t* wire_anti_entropy_encode(const wire_anti_entropy_t* msg);
cbor_item_t* wire_anti_entropy_offer_encode(const wire_anti_entropy_offer_t* msg);
cbor_item_t* wire_anti_entropy_response_encode(const wire_anti_entropy_offer_t* msg);
int wire_anti_entropy_decode(cbor_item_t* item, wire_anti_entropy_t* msg);
int wire_anti_entropy_offer_decode(cbor_item_t* item, wire_anti_entropy_offer_t* msg);
int wire_anti_entropy_response_decode(cbor_item_t* item, wire_anti_entropy_offer_t* msg);
```

- [ ] **Step 2: Write the failing tests** — append to `test/test_network.cpp`:

```cpp
class WireAntiEntropyDigestTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(WireAntiEntropyDigestTest, Roundtrip) {
  wire_anti_entropy_t original = {};
  original.message_id = 0x1122334455667788ULL;
  anti_entropy_fill_id(&original.sender_id, 0x70, 1);
  for (int byte_index = 0; byte_index < (int)ANTI_ENTROPY_BLOOM_BYTES; byte_index++) {
    original.bloom[byte_index] = (uint8_t)(byte_index * 7);
  }
  original.member_count = 120;

  cbor_item_t* encoded = wire_anti_entropy_encode(&original);
  ASSERT_NE(encoded, nullptr);

  wire_anti_entropy_t decoded = {};
  int rc = wire_anti_entropy_decode(encoded, &decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(decoded.message_id, original.message_id);
  EXPECT_TRUE(node_id_equals(&decoded.sender_id, &original.sender_id));
  EXPECT_EQ(decoded.member_count, 120);
  ASSERT_EQ(0, memcmp(decoded.bloom, original.bloom, ANTI_ENTROPY_BLOOM_BYTES));

  cbor_decref(&encoded);
}

TEST_F(WireAntiEntropyDigestTest, RejectsWrongTypeByte) {
  wire_anti_entropy_t original = {};
  original.message_id = 0x0102030405060708ULL;
  anti_entropy_fill_id(&original.sender_id, 0x70, 1);
  cbor_item_t* encoded = wire_anti_entropy_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Corrupt the leading type byte to a different message type.
  cbor_item_t* wrong_type = cbor_build_uint8(WIRE_GOSSIP);
  ASSERT_TRUE(cbor_array_set(encoded, 0, wrong_type));
  cbor_item_t* released = cbor_array_get(encoded, 0);
  cbor_decref(&released);
  wire_anti_entropy_t decoded = {};
  EXPECT_EQ(wire_anti_entropy_decode(encoded, &decoded), -1);

  cbor_decref(&encoded);
}

TEST_F(WireAntiEntropyDigestTest, RejectsOversizedBloom) {
  wire_anti_entropy_t original = {};
  cbor_item_t* encoded = wire_anti_entropy_encode(&original);
  ASSERT_NE(encoded, nullptr);

  // Replace the 512-byte bloom with a 256-byte bytestring — decode must
  // reject rather than read out of bounds.
  cbor_item_t* short_bloom = cbor_build_bytestring(original.bloom, 256);
  ASSERT_TRUE(cbor_array_set(encoded, 4, short_bloom));
  cbor_item_t* released = cbor_array_get(encoded, 4);
  cbor_decref(&released);
  wire_anti_entropy_t decoded = {};
  EXPECT_EQ(wire_anti_entropy_decode(encoded, &decoded), -1);

  cbor_decref(&encoded);
}

class WireAntiEntropyOfferTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(WireAntiEntropyOfferTest, Roundtrip) {
  wire_anti_entropy_offer_t original = {};
  original.message_id = 0xAABBCCDDEEFF0011ULL;
  anti_entropy_fill_id(&original.sender_id, 0x71, 2);
  for (int byte_index = 0; byte_index < (int)ANTI_ENTROPY_BLOOM_BYTES; byte_index++) {
    original.bloom[byte_index] = (uint8_t)(byte_index * 3);
  }
  original.id_count = 2;
  anti_entropy_fill_id(&original.ids[0], 0x72, 11);
  anti_entropy_fill_id(&original.ids[1], 0x72, 12);

  cbor_item_t* encoded = wire_anti_entropy_offer_encode(&original);
  ASSERT_NE(encoded, nullptr);

  wire_anti_entropy_offer_t decoded = {};
  int rc = wire_anti_entropy_offer_decode(encoded, &decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(decoded.message_id, original.message_id);
  EXPECT_TRUE(node_id_equals(&decoded.sender_id, &original.sender_id));
  EXPECT_EQ(decoded.id_count, 2);
  EXPECT_TRUE(node_id_equals(&decoded.ids[0], &original.ids[0]));
  EXPECT_TRUE(node_id_equals(&decoded.ids[1], &original.ids[1]));
  ASSERT_EQ(0, memcmp(decoded.bloom, original.bloom, ANTI_ENTROPY_BLOOM_BYTES));

  cbor_decref(&encoded);
}

TEST_F(WireAntiEntropyOfferTest, ClampsOversizedIdCount) {
  wire_anti_entropy_offer_t original = {};
  original.id_count = 200;  // above WIRE_ANTI_ENTROPY_MAX_IDS
  for (int index = 0; index < WIRE_ANTI_ENTROPY_MAX_IDS; index++) {
    anti_entropy_fill_id(&original.ids[index], 0x73, index);
  }
  memset(original.bloom, 1, ANTI_ENTROPY_BLOOM_BYTES);

  cbor_item_t* encoded = wire_anti_entropy_offer_encode(&original);
  ASSERT_NE(encoded, nullptr);

  wire_anti_entropy_offer_t decoded = {};
  ASSERT_EQ(wire_anti_entropy_offer_decode(encoded, &decoded), 0);
  EXPECT_EQ(decoded.id_count, (uint8_t)WIRE_ANTI_ENTROPY_MAX_IDS);
  for (int index = 0; index < WIRE_ANTI_ENTROPY_MAX_IDS; index++) {
    EXPECT_TRUE(node_id_equals(&decoded.ids[index], &original.ids[index]));
  }

  cbor_decref(&encoded);
}

class WireAntiEntropyResponseTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(WireAntiEntropyResponseTest, Roundtrip) {
  wire_anti_entropy_offer_t original = {};
  original.message_id = 0x00FEDA76543210EFULL;
  anti_entropy_fill_id(&original.sender_id, 0x74, 4);
  original.id_count = 3;
  anti_entropy_fill_id(&original.ids[0], 0x75, 21);
  anti_entropy_fill_id(&original.ids[1], 0x75, 22);
  anti_entropy_fill_id(&original.ids[2], 0x75, 23);

  cbor_item_t* encoded = wire_anti_entropy_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  wire_anti_entropy_offer_t decoded = {};
  int rc = wire_anti_entropy_response_decode(encoded, &decoded);
  ASSERT_EQ(rc, 0);

  EXPECT_EQ(decoded.message_id, original.message_id);
  EXPECT_TRUE(node_id_equals(&decoded.sender_id, &original.sender_id));
  EXPECT_EQ(decoded.id_count, 3);
  for (int index = 0; index < 3; index++) {
    EXPECT_TRUE(node_id_equals(&decoded.ids[index], &original.ids[index]));
  }

  cbor_decref(&encoded);
}

TEST_F(WireAntiEntropyResponseTest, RejectsResponseEncodedAsOfferType) {
  wire_anti_entropy_offer_t original = {};
  cbor_item_t* encoded = wire_anti_entropy_response_encode(&original);
  ASSERT_NE(encoded, nullptr);

  wire_anti_entropy_offer_t decoded = {};
  EXPECT_EQ(wire_anti_entropy_offer_decode(encoded, &decoded), -1);

  cbor_decref(&encoded);
}
```

- [ ] **Step 3: Run tests to verify they fail**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='WireAntiEntropy*'
```

Expected: **link error** — undefined symbols `wire_anti_entropy_encode` and friends.

- [ ] **Step 4: Add codecs to `src/Network/wire.c`** — append after `wire_gossip_pull_decode` (~line 2355, before the relay destroy helpers):

```c
// --- AntiEntropy digest ---

cbor_item_t* wire_anti_entropy_encode(const wire_anti_entropy_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_ANTI_ENTROPY);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  cbor_item_t* bloom = cbor_build_bytestring(msg->bloom, WIRE_ANTI_ENTROPY_BLOOM_BYTES);
  (void)cbor_array_push(array, bloom);
  cbor_decref(&bloom);

  item = cbor_build_uint8(msg->member_count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int wire_anti_entropy_decode(cbor_item_t* item, wire_anti_entropy_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 6) return -1;
  uint8_t type_byte;
  if (_array_get_uint8(item, 0, &type_byte) != 0 || type_byte != WIRE_ANTI_ENTROPY) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* sender = cbor_array_get(item, 3);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;

  cbor_item_t* bloom_item = cbor_array_get(item, 4);
  int bloom_rc = -1;
  if (cbor_isa_bytestring(bloom_item) &&
      cbor_bytestring_length(bloom_item) == WIRE_ANTI_ENTROPY_BLOOM_BYTES) {
    memcpy(msg->bloom, cbor_bytestring_handle(bloom_item), WIRE_ANTI_ENTROPY_BLOOM_BYTES);
    bloom_rc = 0;
  }
  cbor_decref(&bloom_item);
  if (bloom_rc != 0) return -1;
  if (_array_get_uint8(item, 5, &msg->member_count) != 0) return -1;
  return 0;
}

// --- AntiEntropy offer / response ---
// The two share layout; RESPONSE omits the bloom element. A shared decode
// body keeps the index arithmetic in one place.

cbor_item_t* wire_anti_entropy_offer_encode(const wire_anti_entropy_offer_t* msg) {
  uint8_t count = msg->id_count;
  if (count > WIRE_ANTI_ENTROPY_MAX_IDS) count = WIRE_ANTI_ENTROPY_MAX_IDS;

  cbor_item_t* array = cbor_new_definite_array(7);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_ANTI_ENTROPY_OFFER);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  cbor_item_t* bloom = cbor_build_bytestring(msg->bloom, WIRE_ANTI_ENTROPY_BLOOM_BYTES);
  (void)cbor_array_push(array, bloom);
  cbor_decref(&bloom);

  item = cbor_build_uint8(count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* ids = cbor_new_definite_array(count);
  for (uint8_t index = 0; index < count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->ids[index]);
    (void)cbor_array_push(ids, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, ids);
  cbor_decref(&ids);

  return array;
}

static int _anti_entropy_list_decode(cbor_item_t* item, wire_anti_entropy_offer_t* msg,
                                     uint8_t type_byte, bool has_bloom) {
  size_t minimum = has_bloom ? 7 : 6;
  if (!cbor_isa_array(item) || cbor_array_size(item) < minimum) return -1;
  uint8_t wire_type;
  if (_array_get_uint8(item, 0, &wire_type) != 0 || wire_type != type_byte) return -1;
  uint64_t id_hi, id_lo;
  if (_array_get_int(item, 1, &id_hi) != 0) return -1;
  if (_array_get_int(item, 2, &id_lo) != 0) return -1;
  msg->message_id = (id_hi << 32) | id_lo;
  cbor_item_t* sender = cbor_array_get(item, 3);
  int sender_rc = _node_id_decode(sender, &msg->sender_id);
  cbor_decref(&sender);
  if (sender_rc != 0) return sender_rc;

  size_t count_index = has_bloom ? 5 : 4;
  size_t ids_index = has_bloom ? 6 : 5;
  if (has_bloom) {
    cbor_item_t* bloom_item = cbor_array_get(item, 4);
    int bloom_rc = -1;
    if (cbor_isa_bytestring(bloom_item) &&
        cbor_bytestring_length(bloom_item) == WIRE_ANTI_ENTROPY_BLOOM_BYTES) {
      memcpy(msg->bloom, cbor_bytestring_handle(bloom_item), WIRE_ANTI_ENTROPY_BLOOM_BYTES);
      bloom_rc = 0;
    }
    cbor_decref(&bloom_item);
    if (bloom_rc != 0) return -1;
  }

  if (_array_get_uint8(item, count_index, &msg->id_count) != 0) return -1;
  if (msg->id_count > WIRE_ANTI_ENTROPY_MAX_IDS) msg->id_count = WIRE_ANTI_ENTROPY_MAX_IDS;

  cbor_item_t* ids_arr = cbor_array_get(item, ids_index);
  size_t arr_len = 0;
  if (cbor_isa_array(ids_arr)) {
    arr_len = cbor_array_size(ids_arr);
  }
  size_t decode_count = arr_len < msg->id_count ? arr_len : msg->id_count;
  for (size_t index = 0; index < decode_count; index++) {
    cbor_item_t* node_item = cbor_array_get(ids_arr, index);
    _node_id_decode(node_item, &msg->ids[index]);
    cbor_decref(&node_item);
  }
  msg->id_count = (uint8_t)decode_count;
  cbor_decref(&ids_arr);
  return 0;
}

int wire_anti_entropy_offer_decode(cbor_item_t* item, wire_anti_entropy_offer_t* msg) {
  return _anti_entropy_list_decode(item, msg, WIRE_ANTI_ENTROPY_OFFER, true);
}

cbor_item_t* wire_anti_entropy_response_encode(const wire_anti_entropy_offer_t* msg) {
  uint8_t count = msg->id_count;
  if (count > WIRE_ANTI_ENTROPY_MAX_IDS) count = WIRE_ANTI_ENTROPY_MAX_IDS;

  cbor_item_t* array = cbor_new_definite_array(6);
  cbor_item_t* item;

  item = cbor_build_uint8(WIRE_ANTI_ENTROPY_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id >> 32);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->message_id & 0xFFFFFFFF);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* sender = _node_id_encode(&msg->sender_id);
  (void)cbor_array_push(array, sender);
  cbor_decref(&sender);

  item = cbor_build_uint8(count);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  cbor_item_t* ids = cbor_new_definite_array(count);
  for (uint8_t index = 0; index < count; index++) {
    cbor_item_t* node_id = _node_id_encode(&msg->ids[index]);
    (void)cbor_array_push(ids, node_id);
    cbor_decref(&node_id);
  }
  (void)cbor_array_push(array, ids);
  cbor_decref(&ids);

  return array;
}

int wire_anti_entropy_response_decode(cbor_item_t* item, wire_anti_entropy_offer_t* msg) {
  return _anti_entropy_list_decode(item, msg, WIRE_ANTI_ENTROPY_RESPONSE, false);
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='WireAntiEntropy*:AntiEntropyTest.*'
```

Expected: all PASS (previous suite still green).

- [ ] **Step 6: Commit**

```bash
git add src/Network/wire.h src/Network/wire.c test/test_network.cpp
git commit -m "feat(wire): anti-entropy digest/offer/response CBOR codecs"
```

---

### Task 3: Config field, message enum, network plumbing

**Files:**
- Modify: `src/Configuration/config.h:31` (after `gossip_timeout_ms`)
- Modify: `src/Configuration/config.c:35` (defaults) and `config_validate` (~line 176)
- Modify: `test/test_config_validate.cpp`
- Modify: `src/Actor/message.h:149` (after `NETWORK_GOSSIP_TICK`)
- Modify: `src/Network/network.h:120` and `:138`
- Modify: `src/Network/network.c` (config copy ~line 366, timer arm ~line 408, timer cancel ~line 548)

- [ ] **Step 1: Write the failing config tests** — append to `test/test_config_validate.cpp` (after `RejectsZeroGossipInitInterval`):

```cpp
TEST(TestConfigValidate, RejectsAntiEntropyIntervalBelowMinimum) {
  config_t config = config_default();
  config.anti_entropy_interval_s = 5;
  EXPECT_NE(config_validate(&config), 0);
}

TEST(TestConfigValidate, AcceptsAntiEntropyIntervalAtMinimum) {
  config_t config = config_default();
  config.anti_entropy_interval_s = 10;
  EXPECT_EQ(config_validate(&config), 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='TestConfigValidate.*AntiEntropy*'
```

Expected: **compile error** — no member `anti_entropy_interval_s` in `config_t`.

- [ ] **Step 3: Add the config field, default, and validation**

`src/Configuration/config.h`, immediately after `uint32_t gossip_timeout_ms;` (line 31):

```c
  uint32_t anti_entropy_interval_s;    // anti-entropy digest-exchange cadence (min 10)
```

`src/Configuration/config.c`, immediately after `config.gossip_timeout_ms = 5000;` (line 35):

```c
  config.anti_entropy_interval_s = 300;
```

`src/Configuration/config.c`, in `config_validate`, immediately after the `gossip_timeout_ms == 0` check:

```c
  if (config->anti_entropy_interval_s < 10) {
    log_error("config_validate: anti_entropy_interval_s (%u) must be >= 10",
              config->anti_entropy_interval_s);
    valid = false;
  }
```

- [ ] **Step 4: Run config tests to verify they pass**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='TestConfigValidate.*'
```

Expected: all PASS (including pre-existing `DefaultConfigPasses`, exercising the 300 default).

- [ ] **Step 5: Add the message type and network plumbing**

`src/Actor/message.h`, immediately after `NETWORK_GOSSIP_TICK,` (line 149):

```c
  NETWORK_ANTI_ENTROPY_TICK,
```

`src/Network/network.h`, immediately after `ATOMIC(uint64_t) gossip_timer_id;` (line 120):

```c
  ATOMIC(uint64_t) anti_entropy_timer_id;
```

In the same struct, immediately after `uint32_t gossip_timeout_ms;` (line 138):

```c
  uint32_t anti_entropy_interval_s;
```

`src/Network/network.c`, immediately after `network->gossip_timeout_ms = config->gossip_timeout_ms;` (~line 366):

```c
  network->anti_entropy_interval_s = config->anti_entropy_interval_s;
```

`src/Network/network.c`, in `network_create`, immediately after the gossip timer `timer_actor_set(... NETWORK_GOSSIP_TICK ...)` call (~line 408):

```c
  // Anti-entropy timer: one digest exchange per tick with a random
  // connected peer. Repair is part of correctness, not an optimization, so
  // there is no zero-disable; config validation floors the interval at 10s.
  network->anti_entropy_timer_id = 0;
  timer_actor_set(timer,
      (uint64_t)network->anti_entropy_interval_s * 1000,
      (uint64_t)network->anti_entropy_interval_s * 1000,
      &network->actor,
      NETWORK_ANTI_ENTROPY_TICK,
      &network->anti_entropy_timer_id);
```

`src/Network/network.c`, in the destroy path, immediately after the gossip timer cancel block (~line 545):

```c
  if (atomic_load(&network->anti_entropy_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->anti_entropy_timer_id));
    atomic_store(&network->anti_entropy_timer_id, 0);
  }
```

- [ ] **Step 6: Build and run the full gtest suite to check for regressions**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs
```

Expected: full suite green, one binary at a time.

- [ ] **Step 7: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c test/test_config_validate.cpp src/Actor/message.h src/Network/network.h src/Network/network.c
git commit -m "feat(network): anti-entropy tick timer, config field, and lifecycle"
```

---

### Task 4: Network handlers + dispatch wiring

**Files:**
- Modify: `src/Network/network.c` — forward declarations ~line 128; handlers after `network_handle_gossip_pull_received` ~line 2400; `NETWORK_ANTI_ENTROPY_TICK` dispatch ~line 5128; three wire cases in BOTH wire-dispatch switches after each `WIRE_GOSSIP_PULL` case (~line 5496 and ~line 6207)

- [ ] **Step 1: Add forward declarations and includes** — with the other handler forward declarations (~line 128):

```c
static void network_handle_anti_entropy_tick(network_t* network, message_t* msg);
static void network_handle_anti_entropy_received(network_t* network, message_t* msg);
static void network_handle_anti_entropy_offer_received(network_t* network, message_t* msg);
static void network_handle_anti_entropy_response_received(network_t* network, message_t* msg);
```

Add to the includes at the top, next to the other Network includes:

```c
#include "anti_entropy.h"
```

If `../Platform/platform_random.h` is not already included (grep the includes), also add `#include "../Platform/platform_random.h"`.

- [ ] **Step 2: Add the handlers** — place immediately after `network_handle_gossip_pull_received` (~line 2400):

```c
// --- Anti-entropy (ring-membership divergence repair) ---
// Three-leg digest/offer/response exchange with one random connected peer
// per tick. Novel ids per exchange are capped at ANTI_ENTROPY_MAX_IDS by
// anti_entropy_collect_novel; network_add_node_to_ring skips already-known
// ids, so cap consumption is genuinely-novel-only.

static peer_connection_t* network_pick_random_connected_peer(network_t* network) {
  size_t connected_count = 0;
  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* candidate = network->conn_mgr.peers[index];
    if (candidate != NULL && candidate->connected) connected_count++;
  }
  if (connected_count == 0) return NULL;

  size_t pick = platform_random_uniform_index(connected_count);
  size_t passed = 0;
  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* candidate = network->conn_mgr.peers[index];
    if (candidate == NULL || !candidate->connected) continue;
    if (passed == pick) return candidate;
    passed++;
  }
  // Unreachable: connected_count entries were counted in the first pass.
  return NULL;
}

static void network_handle_anti_entropy_tick(network_t* network, message_t* msg) {
  (void)msg;
  if (network->rings == NULL) return;

  peer_connection_t* peer = network_pick_random_connected_peer(network);
  if (peer == NULL) return;

  wire_anti_entropy_t digest;
  memset(&digest, 0, sizeof(digest));
  digest.message_id = gossip_handle_next_query_id(&network->gossip);
  memcpy(&digest.sender_id, &network->authority->local_id, sizeof(node_id_t));
  // An empty ring set still sends: the filter is all-zero and the peer
  // offers its full membership list up to the cap.
  size_t member_count = anti_entropy_bloom_build(network->rings, digest.bloom);
  digest.member_count = (uint8_t)member_count;

  cbor_item_t* cbor = wire_anti_entropy_encode(&digest);
  int send_rc = conn_state_send(network, peer, cbor);
  cbor_decref(&cbor);
  if (send_rc != 0) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY, MSG_DIRECTION_SENT,
                       &peer->remote_node_id, digest.message_id, NULL,
                       0, &network->hebbian);
  }
}

static void network_handle_anti_entropy_received(network_t* network, message_t* msg) {
  wire_anti_entropy_t* digest = (wire_anti_entropy_t*)msg->payload;
  if (digest == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY, MSG_DIRECTION_RECEIVED,
                       &digest->sender_id, digest->message_id, NULL,
                       0, &network->hebbian);
  }

  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &digest->sender_id);
  if (peer == NULL || !peer->connected) return;

  wire_anti_entropy_offer_t offer;
  memset(&offer, 0, sizeof(offer));
  offer.message_id = gossip_handle_next_query_id(&network->gossip);
  memcpy(&offer.sender_id, &network->authority->local_id, sizeof(node_id_t));

  (void)anti_entropy_bloom_build(network->rings, offer.bloom);
  offer.id_count = (uint8_t)anti_entropy_collect_novel(
      network->rings, digest->bloom, offer.ids, ANTI_ENTROPY_MAX_IDS);

  cbor_item_t* cbor = wire_anti_entropy_offer_encode(&offer);
  conn_state_send(network, peer, cbor);
  cbor_decref(&cbor);

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY_OFFER, MSG_DIRECTION_SENT,
                       &peer->remote_node_id, offer.message_id, NULL,
                       0, &network->hebbian);
  }
}

static void network_handle_anti_entropy_offer_received(network_t* network, message_t* msg) {
  wire_anti_entropy_offer_t* offer = (wire_anti_entropy_offer_t*)msg->payload;
  if (offer == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY_OFFER, MSG_DIRECTION_RECEIVED,
                       &offer->sender_id, offer->message_id, NULL,
                       0, &network->hebbian);
  }

  // The id count is already clamped by the decoder; network_add_node_to_ring
  // skips already-known ids, so at most ANTI_ENTROPY_MAX_IDS new insertions.
  for (uint8_t index = 0;
       index < offer->id_count && index < ANTI_ENTROPY_MAX_IDS;
       index++) {
    network_add_node_to_ring(network, &offer->ids[index], 0, 0, NULL);
  }

  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &offer->sender_id);
  if (peer == NULL || !peer->connected) return;

  wire_anti_entropy_offer_t response;
  memset(&response, 0, sizeof(response));
  response.message_id = gossip_handle_next_query_id(&network->gossip);
  memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));
  response.id_count = (uint8_t)anti_entropy_collect_novel(
      network->rings, offer->bloom, response.ids, ANTI_ENTROPY_MAX_IDS);

  cbor_item_t* cbor = wire_anti_entropy_response_encode(&response);
  conn_state_send(network, peer, cbor);
  cbor_decref(&cbor);

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY_RESPONSE, MSG_DIRECTION_SENT,
                       &peer->remote_node_id, response.message_id, NULL,
                       0, &network->hebbian);
  }
}

static void network_handle_anti_entropy_response_received(network_t* network, message_t* msg) {
  wire_anti_entropy_offer_t* response = (wire_anti_entropy_offer_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_ANTI_ENTROPY_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  for (uint8_t index = 0;
       index < response->id_count && index < ANTI_ENTROPY_MAX_IDS;
       index++) {
    network_add_node_to_ring(network, &response->ids[index], 0, 0, NULL);
  }
}
```

- [ ] **Step 3: Wire the tick into the message dispatch** — in the network actor's message dispatch, directly after the `NETWORK_GOSSIP_TICK` case (~line 5128), matching that case's exact brace/indent style (inspect it first and copy verbatim; do not alter neighbor control flow):

```c
        case NETWORK_ANTI_ENTROPY_TICK:
          network_handle_anti_entropy_tick(network, msg);
          break;
```

- [ ] **Step 4: Wire the three wire cases into BOTH wire-dispatch switches** — directly after each `case WIRE_GOSSIP_PULL { ... }` block (two sites, ~line 5496 and ~line 6207; match each site's indentation, as the duplicated `WIRE_GOSSIP_PULL` cases do):

```c
        case WIRE_ANTI_ENTROPY: {
          wire_anti_entropy_t* payload = get_clear_memory(sizeof(wire_anti_entropy_t));
          if (payload != NULL) {
            if (wire_anti_entropy_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_anti_entropy_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_ANTI_ENTROPY_OFFER: {
          wire_anti_entropy_offer_t* payload = get_clear_memory(sizeof(wire_anti_entropy_offer_t));
          if (payload != NULL) {
            if (wire_anti_entropy_offer_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_anti_entropy_offer_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_ANTI_ENTROPY_RESPONSE: {
          wire_anti_entropy_offer_t* payload = get_clear_memory(sizeof(wire_anti_entropy_offer_t));
          if (payload != NULL) {
            if (wire_anti_entropy_response_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_anti_entropy_response_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
```

- [ ] **Step 5: Build and run the full suite**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs
```

Expected: full suite green. The handlers require a live `network_t` (like all gossip handlers) so they carry no direct gtest coverage; their logic is exercised through the Task 1 module tests and the Task 5 exchange test via the codecs.

- [ ] **Step 6: Commit**

```bash
git add src/Network/network.c
git commit -m "feat(network): wire anti-entropy digest exchange into protocol actor"
```

---

### Task 5: Three-leg convergence integration test

**Files:**
- Modify: `test/test_network.cpp` — append at end

- [ ] **Step 1: Write the tests** (these compose already-tested codecs + logic; they guard the full exchange path end-to-end):

```cpp
// === Anti-entropy three-leg convergence (codec-mediated) ===
// Executes the exchange exactly as the handlers wire it:
//   leg 1: A -> B   = bloom over A's rings
//   leg 2: B -> A   = OFFER(bloom_b, ids B holds absent in bloom_a)
//   leg 3: A -> B   = insert offered ids; RESPONSE(ids A holds absent in bloom_b)
//   leg 4: B inserts response ids. Both sets then equal.
TEST_F(AntiEntropyTest, DigestExchangeConvergesThroughWire) {
  ring_set_t* rings_a = rings;
  ring_set_t* rings_b = ring_set_create(RING_K, RING_M, RING_ALPHA);

  // Distinct ids via the memset helper (node_id_from_string is a base58
  // decoder that zeroes unknown strings — do not use it for distinct ids).
  node_id_t a_ids[3];
  net_node_t* a_members[16] = { NULL };
  size_t a_member_count = 0;
  for (int index = 0; index < 3; index++) {
    anti_entropy_fill_id(&a_ids[index], 0x71, index);
    a_members[a_member_count] = net_node_create(&a_ids[index], 0x0A000001, 8080);
    ASSERT_NE(a_members[a_member_count], nullptr);
    ASSERT_EQ(ring_set_insert(rings_a, a_members[a_member_count],
                              (uint32_t)(5000 * (index + 1))), 0);
    a_member_count++;
  }

  node_id_t b_ids[5];
  net_node_t* b_members[16] = { NULL };
  size_t b_member_count = 0;
  for (int index = 0; index < 5; index++) {
    anti_entropy_fill_id(&b_ids[index], 0x72, index);
    b_members[b_member_count] = net_node_create(&b_ids[index], 0x0B000001, 9080);
    ASSERT_NE(b_members[b_member_count], nullptr);
    ASSERT_EQ(ring_set_insert(rings_b, b_members[b_member_count],
                              (uint32_t)(6000 * (index + 1))), 0);
    b_member_count++;
  }

  uint8_t bloom_a[ANTI_ENTROPY_BLOOM_BYTES] = {};
  uint8_t bloom_b[ANTI_ENTROPY_BLOOM_BYTES] = {};
  EXPECT_EQ(anti_entropy_bloom_build(rings_a, bloom_a), 3u);
  EXPECT_EQ(anti_entropy_bloom_build(rings_b, bloom_b), 5u);

  // Leg 2: B collects ids absent in A's filter and sends them encoded.
  node_id_t offered[ANTI_ENTROPY_MAX_IDS];
  size_t offered_count = anti_entropy_collect_novel(rings_b, bloom_a, offered, ANTI_ENTROPY_MAX_IDS);
  EXPECT_EQ(offered_count, 5u);  // sets are disjoint

  wire_anti_entropy_offer_t offer = {};
  offer.message_id = 0x1000000000000001ULL;
  for (size_t index = 0; index < offered_count; index++) offer.ids[index] = offered[index];
  offer.id_count = (uint8_t)offered_count;
  cbor_item_t* offer_cbor = wire_anti_entropy_offer_encode(&offer);
  ASSERT_NE(offer_cbor, nullptr);
  wire_anti_entropy_offer_t received_offer = {};
  ASSERT_EQ(wire_anti_entropy_offer_decode(offer_cbor, &received_offer), 0);
  cbor_decref(&offer_cbor);

  // A's receive side of the offer: insert novel ids (tracked for cleanup).
  for (uint8_t index = 0; index < received_offer.id_count && index < ANTI_ENTROPY_MAX_IDS; index++) {
    node_id_t* id = &received_offer.ids[index];
    if (ring_set_find_by_id(rings_a, id) != NULL) continue;
    net_node_t* node = net_node_create(id, 0, 0);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(ring_set_insert(rings_a, node, 0), 0);
    a_members[a_member_count++] = node;
  }
  // B's set has 5 members, all novel to A, so A now holds 3 + 5 = 8.
  EXPECT_EQ(a_member_count, 8u);

  // Leg 3: A collects its ids absent in B's filter.
  node_id_t response_ids[ANTI_ENTROPY_MAX_IDS];
  size_t response_count = anti_entropy_collect_novel(rings_a, bloom_b, response_ids, ANTI_ENTROPY_MAX_IDS);
  // The 5 just-inserted ids ARE B's own member ids, so they test PRESENT
  // in bloom_b (built from exactly those members). Only A's 3 originals
  // are novel to B. (An 8 here would mean the exchange re-sends ids the
  // peer already has.)
  EXPECT_EQ(response_count, 3u);

  wire_anti_entropy_offer_t response = {};
  response.message_id = 0x1000000000000002ULL;
  for (size_t index = 0; index < response_count; index++) response.ids[index] = response_ids[index];
  response.id_count = (uint8_t)response_count;
  cbor_item_t* response_cbor = wire_anti_entropy_response_encode(&response);
  ASSERT_NE(response_cbor, nullptr);
  wire_anti_entropy_offer_t received_response = {};
  ASSERT_EQ(wire_anti_entropy_response_decode(response_cbor, &received_response), 0);
  cbor_decref(&response_cbor);

  // B's receive side of the response: insert.
  for (uint8_t index = 0; index < received_response.id_count && index < ANTI_ENTROPY_MAX_IDS; index++) {
    node_id_t* id = &received_response.ids[index];
    if (ring_set_find_by_id(rings_b, id) != NULL) continue;
    net_node_t* node = net_node_create(id, 0, 0);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(ring_set_insert(rings_b, node, 0), 0);
    b_members[b_member_count++] = node;
  }
  EXPECT_EQ(b_member_count, 8u);

  // Both directions converged: every A id is in B, every B id is in A.
  for (int index = 0; index < 3; index++) {
    EXPECT_TRUE(ring_set_find_by_id(rings_b, &a_ids[index]) != NULL) << "missing in B: index " << index;
  }
  for (int index = 0; index < 5; index++) {
    EXPECT_TRUE(ring_set_find_by_id(rings_a, &b_ids[index]) != NULL) << "missing in A: index " << index;
  }

  anti_entropy_destroy_nodes(a_members, a_member_count);
  anti_entropy_destroy_nodes(b_members, b_member_count);
  ring_set_destroy(rings_b);
}

TEST_F(AntiEntropyTest, DigestExchangeCapTruncatesLargeDifference) {
  // A: 1 member. B: 15 members, all disjoint. The offer to A must cap at
  // ANTI_ENTROPY_MAX_IDS even though the true difference is 15.
  node_id_t anchor_id;
  anti_entropy_fill_id(&anchor_id, 0x76, 1);
  net_node_t* anchor = net_node_create(&anchor_id, 0x0A000001, 8080);
  ASSERT_NE(anchor, nullptr);
  ASSERT_EQ(ring_set_insert(rings, anchor, 5000), 0);

  ring_set_t* rings_b = ring_set_create(RING_K, RING_M, RING_ALPHA);
  net_node_t* b_members[15] = { NULL };
  for (int index = 0; index < 15; index++) {
    node_id_t id;
    anti_entropy_fill_id(&id, 0x77, index);
    b_members[index] = net_node_create(&id, 0x0B000001, 9080);
    ASSERT_NE(b_members[index], nullptr);
    ASSERT_EQ(ring_set_insert(rings_b, b_members[index], (uint32_t)(7000 * (index + 1))), 0);
  }

  uint8_t bloom_a[ANTI_ENTROPY_BLOOM_BYTES] = {};
  ASSERT_EQ(anti_entropy_bloom_build(rings, bloom_a), 1u);

  node_id_t offer_ids[ANTI_ENTROPY_MAX_IDS];
  size_t offer_count = anti_entropy_collect_novel(rings_b, bloom_a, offer_ids, ANTI_ENTROPY_MAX_IDS);
  EXPECT_EQ(offer_count, (size_t)ANTI_ENTROPY_MAX_IDS);
  EXPECT_EQ(offer_count, 10u);

  wire_anti_entropy_offer_t offer = {};
  offer.message_id = 0x2000000000000001ULL;
  for (size_t index = 0; index < offer_count; index++) offer.ids[index] = offer_ids[index];
  offer.id_count = (uint8_t)offer_count;
  cbor_item_t* encoded = wire_anti_entropy_offer_encode(&offer);
  wire_anti_entropy_offer_t decoded = {};
  ASSERT_EQ(wire_anti_entropy_offer_decode(encoded, &decoded), 0);
  EXPECT_EQ(decoded.id_count, 10u);
  cbor_decref(&encoded);

  net_node_destroy(anchor);
  anti_entropy_destroy_nodes(b_members, 15);
  ring_set_destroy(rings_b);
}
```

Note: `ring_set_insert` puts latency 0 in the innermost ring (5ms bucket), so insertion count assertions are unaffected by ring placement.

- [ ] **Step 2: Run the anti-entropy suite**

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs --gtest_filter='AntiEntropyTest.*'
```

Expected: all PASS, including `DigestExchangeConvergesThroughWire` and `DigestExchangeCapTruncatesLargeDifference`.

- [ ] **Step 3: Commit**

```bash
git add test/test_network.cpp
git commit -m "test(network): anti-entropy three-leg convergence and cap tests"
```

---

### Task 6: Full verification, valgrind, completion gate

**Files:** none modified unless verification surfaces a fix.

- [ ] **Step 1: Full gtest suite, serially** (never parallel — concurrent runs steal resources and flake):

```bash
cmake --build build-test --target testliboffs -j8
./build-test/testliboffs
```

Expected: green.

- [ ] **Step 2: Valgrind the new tests** — single valgrind process, `-gdwarf-4` build:

```bash
cmake --build build-vg --target testliboffs -j8
valgrind --leak-check=full --error-exitcode=1 \
  ./build-vg/test/testliboffs --gtest_filter='AntiEntropyTest.*:WireAntiEntropy*'
```

Expected: 0 definitely-lost/indirectly-lost blocks. The valgrind error catalog in memory lists known pre-existing issues (scheduler shutdown invalid-read, etc.) — anything new pointing at `anti_entropy`, `wire_anti_entropy`, or the new `network.c` handlers is a plan bug; fix it.

- [ ] **Step 3: De-wonk audit** — invoke the `de-wonk` skill and walk its checklist over the touched files (`anti_entropy.c/.h`, `wire.c/.h`, `network.c`, `message.h`, `config.c/.h`, tests). Verify specifically: all four handler entry points are reachable from dispatch (tick case in the message dispatch; three wire cases in **both** wire switches); the `_Static_assert`s compile against `wire.h`; no TODO/FIXME left; inserted novel ids reuse `network_add_node_to_ring` rather than a parallel insertion path.

- [ ] **Step 4: Close the harmony ticket** created at execution start.

- [ ] **Step 5: Finish the branch** — invoke `superpowers:finishing-a-development-branch`. No force-push; no push without explicit user confirmation.