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
// bloom must be zero-initialized by the caller (e.g. memset).
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