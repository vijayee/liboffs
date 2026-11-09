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