//
// Created by victor on 5/14/25.
//

#include "elastic_bloom_filter.h"
#include "../Util/allocator.h"
#include <xxh3.h>
#include <stdlib.h>
#include <string.h>

// --- Bucket linked list helpers ---

static ebf_bucket_entry_t* bucket_insert(ebf_bucket_entry_t* head, uint32_t fingerprint) {
  ebf_bucket_entry_t* entry = get_clear_memory(sizeof(ebf_bucket_entry_t));
  entry->fingerprint = fingerprint;
  entry->next = head;
  return entry;
}

static ebf_bucket_entry_t* bucket_remove(ebf_bucket_entry_t* head, uint32_t fingerprint, bool* removed) {
  if (head == NULL) {
    if (removed) *removed = false;
    return NULL;
  }
  if (head->fingerprint == fingerprint) {
    ebf_bucket_entry_t* next = head->next;
    free(head);
    if (removed) *removed = true;
    return next;
  }
  head->next = bucket_remove(head->next, fingerprint, removed);
  return head;
}

static bool bucket_contains(const ebf_bucket_entry_t* head, uint32_t fingerprint) {
  const ebf_bucket_entry_t* current = head;
  while (current != NULL) {
    if (current->fingerprint == fingerprint) return true;
    current = current->next;
  }
  return false;
}

static bool bucket_is_empty(const ebf_bucket_entry_t* head) {
  return head == NULL;
}

static void bucket_destroy(ebf_bucket_entry_t* head) {
  while (head != NULL) {
    ebf_bucket_entry_t* next = head->next;
    free(head);
    head = next;
  }
}

// --- Hash computation ---

static void compute_hash_pair(const elastic_bloom_filter_t* ebf, const uint8_t* data,
                              size_t len, uint32_t hash_index,
                              uint32_t* out_fingerprint, size_t* out_index) {
  uint64_t seed = ebf->seed_a + hash_index * ebf->seed_b;
  uint64_t hash = XXH3_64bits_withSeed(data, len, seed);
  *out_index = (size_t)(hash % ebf->bucket_count);
  *out_fingerprint = (uint32_t)((hash / ebf->bucket_count) % ((uint64_t)1 << ebf->fp_bits));
  if (*out_fingerprint == 0) *out_fingerprint = 1;  // 0 means empty
}

// --- Bitset rebuild ---
// Used by decode to reconstruct bitset from buckets

static void rebuild_bitset(elastic_bloom_filter_t* ebf) {
  memset(ebf->bits->data, 0, ebf->bits->size);
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    if (!bucket_is_empty(ebf->buckets[index])) {
      bitset_set(ebf->bits, index, true);
    }
  }
}

// --- Lifecycle ---

elastic_bloom_filter_t* elastic_bloom_filter_create(size_t size, uint32_t hash_count,
                                                     float omega, uint32_t fp_bits) {
  if (size == 0) size = 256;
  if (hash_count == 0) hash_count = 3;
  if (omega <= 0.0f) omega = 0.75f;
  if (fp_bits == 0) fp_bits = EBF_DEFAULT_FP_BITS;

  elastic_bloom_filter_t* ebf = get_clear_memory(sizeof(elastic_bloom_filter_t));
  ebf->bucket_count = size;
  ebf->size = size;
  ebf->buckets = get_clear_memory(size * sizeof(ebf_bucket_entry_t*));
  ebf->bits = bitset_create((size + 7) / 8);
  ebf->hash_count = hash_count;
  ebf->seed_a = 1;
  ebf->seed_b = 2;
  ebf->omega = omega;
  ebf->fp_bits = fp_bits;
  ebf->count = 0;
  return ebf;
}

void elastic_bloom_filter_destroy(elastic_bloom_filter_t* ebf) {
  if (ebf == NULL) return;
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    bucket_destroy(ebf->buckets[index]);
  }
  free(ebf->buckets);
  bitset_destroy(ebf->bits);
  free(ebf);
}

// --- Add / Contains / Remove ---

bool elastic_bloom_filter_add(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
  if (ebf == NULL || data == NULL) return false;
  bool was_new = false;
  for (uint32_t index = 0; index < ebf->hash_count; index++) {
    uint32_t fingerprint;
    size_t bucket_idx;
    compute_hash_pair(ebf, data, len, index, &fingerprint, &bucket_idx);
    if (!bucket_contains(ebf->buckets[bucket_idx], fingerprint)) {
      ebf->buckets[bucket_idx] = bucket_insert(ebf->buckets[bucket_idx], fingerprint);
      bitset_set(ebf->bits, bucket_idx, true);
      was_new = true;
    }
  }
  if (was_new) {
    ebf->count++;
    if (elastic_bloom_filter_ratio(ebf) > ebf->omega) {
      elastic_bloom_filter_expand(ebf);
    }
  }
  return was_new;
}

bool elastic_bloom_filter_contains(const elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
  if (ebf == NULL || data == NULL) return false;
  for (uint32_t index = 0; index < ebf->hash_count; index++) {
    uint32_t fingerprint;
    size_t bucket_idx;
    compute_hash_pair(ebf, data, len, index, &fingerprint, &bucket_idx);
    if (!bitset_get(ebf->bits, bucket_idx)) return false;
    if (!bucket_contains(ebf->buckets[bucket_idx], fingerprint)) return false;
  }
  return true;
}

bool elastic_bloom_filter_remove(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
  if (ebf == NULL || data == NULL) return false;
  bool removed = false;
  for (uint32_t index = 0; index < ebf->hash_count; index++) {
    uint32_t fingerprint;
    size_t bucket_idx;
    compute_hash_pair(ebf, data, len, index, &fingerprint, &bucket_idx);
    bool entry_removed = false;
    ebf->buckets[bucket_idx] = bucket_remove(ebf->buckets[bucket_idx], fingerprint, &entry_removed);
    if (entry_removed) {
      removed = true;
      if (bucket_is_empty(ebf->buckets[bucket_idx])) {
        bitset_set(ebf->bits, bucket_idx, false);
      }
    }
  }
  if (removed) {
    ebf->count--;
    if (elastic_bloom_filter_ratio(ebf) < ebf->omega / 4.0f && ebf->bucket_count > 16) {
      elastic_bloom_filter_compress(ebf);
    }
  }
  return removed;
}

// --- Expand / Compress ---

int elastic_bloom_filter_expand(elastic_bloom_filter_t* ebf) {
  if (ebf == NULL) return -1;
  size_t new_bucket_count = ebf->bucket_count * 2;
  ebf_bucket_entry_t** new_buckets = get_clear_memory(new_bucket_count * sizeof(ebf_bucket_entry_t*));
  bitset_t* new_bits = bitset_create((new_bucket_count + 7) / 8);

  // Redistribute entries by LSB of fingerprint
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    ebf_bucket_entry_t* entry = ebf->buckets[index];
    while (entry != NULL) {
      ebf_bucket_entry_t* next = entry->next;
      uint32_t fp_bit = entry->fingerprint & 1;
      size_t new_idx = (index * 2) + fp_bit;
      uint32_t new_fp = entry->fingerprint >> 1;
      if (new_fp == 0) new_fp = 1;
      entry->fingerprint = new_fp;
      entry->next = new_buckets[new_idx];
      new_buckets[new_idx] = entry;
      bitset_set(new_bits, new_idx, true);
      entry = next;
    }
  }

  free(ebf->buckets);
  bitset_destroy(ebf->bits);
  ebf->buckets = new_buckets;
  ebf->bits = new_bits;
  ebf->bucket_count = new_bucket_count;
  ebf->size = new_bucket_count;
  return 0;
}

int elastic_bloom_filter_compress(elastic_bloom_filter_t* ebf) {
  if (ebf == NULL || ebf->bucket_count <= 16) return -1;
  size_t new_bucket_count = ebf->bucket_count / 2;
  ebf_bucket_entry_t** new_buckets = get_clear_memory(new_bucket_count * sizeof(ebf_bucket_entry_t*));
  bitset_t* new_bits = bitset_create((new_bucket_count + 7) / 8);

  for (size_t index = 0; index < ebf->bucket_count; index++) {
    ebf_bucket_entry_t* entry = ebf->buckets[index];
    while (entry != NULL) {
      ebf_bucket_entry_t* next = entry->next;
      // Prepend disambiguation bit (0 = lower half, 1 = upper half)
      uint32_t half_bit = (index >= new_bucket_count) ? 1 : 0;
      uint32_t new_fp = (entry->fingerprint << 1) | half_bit;
      size_t new_idx = index % new_bucket_count;
      entry->fingerprint = new_fp;
      entry->next = new_buckets[new_idx];
      new_buckets[new_idx] = entry;
      bitset_set(new_bits, new_idx, true);
      entry = next;
    }
  }

  free(ebf->buckets);
  bitset_destroy(ebf->bits);
  ebf->buckets = new_buckets;
  ebf->bits = new_bits;
  ebf->bucket_count = new_bucket_count;
  ebf->size = new_bucket_count;
  return 0;
}

// --- Merge ---

int elastic_bloom_filter_merge(elastic_bloom_filter_t* dest, const elastic_bloom_filter_t* src) {
  if (dest == NULL || src == NULL) return -1;
  if (dest->bucket_count != src->bucket_count) return -1;

  // Bitwise-OR the bitsets
  for (size_t index = 0; index < dest->bits->size && index < src->bits->size; index++) {
    dest->bits->data[index] |= src->bits->data[index];
  }

  // Union fingerprints per bucket
  for (size_t index = 0; index < src->bucket_count; index++) {
    const ebf_bucket_entry_t* entry = src->buckets[index];
    while (entry != NULL) {
      if (!bucket_contains(dest->buckets[index], entry->fingerprint)) {
        dest->buckets[index] = bucket_insert(dest->buckets[index], entry->fingerprint);
      }
      entry = entry->next;
    }
  }
  return 0;
}

// --- Accessors ---

size_t elastic_bloom_filter_count(const elastic_bloom_filter_t* ebf) {
  return ebf == NULL ? 0 : ebf->count;
}

size_t elastic_bloom_filter_size(const elastic_bloom_filter_t* ebf) {
  return ebf == NULL ? 0 : ebf->size;
}

float elastic_bloom_filter_ratio(const elastic_bloom_filter_t* ebf) {
  if (ebf == NULL || ebf->bucket_count == 0) return 0.0f;
  size_t occupied = 0;
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    if (!bucket_is_empty(ebf->buckets[index])) occupied++;
  }
  return (float)occupied / (float)ebf->bucket_count;
}

// --- Iterate / Direct insert ---

void elastic_bloom_filter_iterate(const elastic_bloom_filter_t* ebf, ebf_entry_cb_t callback, void* ctx) {
  if (ebf == NULL || callback == NULL) return;
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    const ebf_bucket_entry_t* entry = ebf->buckets[index];
    while (entry != NULL) {
      callback(ctx, index, entry->fingerprint);
      entry = entry->next;
    }
  }
}

int elastic_bloom_filter_bucket_insert(elastic_bloom_filter_t* ebf, size_t bucket_idx, uint32_t fingerprint) {
  if (ebf == NULL || bucket_idx >= ebf->bucket_count) return -1;
  if (!bucket_contains(ebf->buckets[bucket_idx], fingerprint)) {
    ebf->buckets[bucket_idx] = bucket_insert(ebf->buckets[bucket_idx], fingerprint);
    bitset_set(ebf->bits, bucket_idx, true);
  }
  return 0;
}

// --- CBOR encode/decode ---

cbor_item_t* elastic_bloom_filter_encode(const elastic_bloom_filter_t* ebf) {
  if (ebf == NULL) return NULL;

  // Count occupied buckets for sparse encoding
  size_t num_occupied = 0;
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    if (!bucket_is_empty(ebf->buckets[index])) num_occupied++;
  }

  // [size, hash_count, fp_bits, seed_a, seed_b, bitset_bytes, num_occupied, bucket_entries...]
  cbor_item_t* root = cbor_new_definite_array(8 + (int)num_occupied);
  (void)cbor_array_push(root, cbor_build_uint64(ebf->size));
  (void)cbor_array_push(root, cbor_build_uint32(ebf->hash_count));
  (void)cbor_array_push(root, cbor_build_uint32(ebf->fp_bits));
  (void)cbor_array_push(root, cbor_build_uint64(ebf->seed_a));
  (void)cbor_array_push(root, cbor_build_uint64(ebf->seed_b));
  (void)cbor_array_push(root, cbor_build_bytestring(ebf->bits->data, ebf->bits->size));
  (void)cbor_array_push(root, cbor_build_uint64(num_occupied));

  // Sparse bucket entries
  for (size_t index = 0; index < ebf->bucket_count; index++) {
    if (bucket_is_empty(ebf->buckets[index])) continue;
    // Count fingerprints in this bucket
    size_t fp_count = 0;
    const ebf_bucket_entry_t* entry = ebf->buckets[index];
    while (entry != NULL) { fp_count++; entry = entry->next; }
    // [bucket_index, fp_count, fp1, fp2, ...]
    cbor_item_t* bucket_item = cbor_new_definite_array(2 + (int)fp_count);
    (void)cbor_array_push(bucket_item, cbor_build_uint64(index));
    (void)cbor_array_push(bucket_item, cbor_build_uint32((uint32_t)fp_count));
    entry = ebf->buckets[index];
    while (entry != NULL) {
      (void)cbor_array_push(bucket_item, cbor_build_uint32(entry->fingerprint));
      entry = entry->next;
    }
    (void)cbor_array_push(root, bucket_item);
  }

  return root;
}

elastic_bloom_filter_t* elastic_bloom_filter_decode(cbor_item_t* item) {
  // TODO: implement CBOR decode for EBF
  (void)item;
  return NULL;
}