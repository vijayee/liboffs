//
// Created by victor on 5/14/25.
//

#ifndef OFFS_WIRE_H
#define OFFS_WIRE_H

#include "node_id.h"
#include "authority.h"
#include <cbor.h>
#include <stdint.h>
#include <stddef.h>

// Wire protocol message types
#define WIRE_PING                1
#define WIRE_PING_RESPONSE       2
#define WIRE_PING_CAPACITY       3
#define WIRE_PING_CAPACITY_RESPONSE 4
#define WIRE_PING_BLOCK          5
#define WIRE_PING_BLOCK_RESPONSE 6
#define WIRE_FIND_BLOCK          7
#define WIRE_FIND_BLOCK_RESPONSE 8
#define WIRE_FIND_NODE           9
#define WIRE_FIND_NODE_RESPONSE  10
#define WIRE_STORE_BLOCK         11
#define WIRE_STORE_BLOCK_RESPONSE 12
#define WIRE_SEEKING_BLOCKS      13
#define WIRE_SEEKING_BLOCKS_RESPONSE 14
#define WIRE_RANK_BLOCK          15
#define WIRE_RECALL_BLOCK        16
#define WIRE_RECALL_ACCEPT       17
#define WIRE_RECALL_DECLINE      18
#define WIRE_RATE_LIMITED        19

// Magic number for protocol identification
#define WIRE_MAGIC 0x4F464653  // "OFFS"

// Limits from Network Design spec
#define WIRE_MAX_PATH             6
#define WIRE_MAX_VISITED_BLOOM    256  // bytes = 2048 bits
#define WIRE_MAX_OFFERS           20
#define WIRE_MAX_SAMPLES          8

// --- Ping ---

typedef struct {
  uint64_t message_id;
  uint64_t timestamp;
} wire_ping_t;

// --- PingResponse ---

typedef struct {
  uint64_t message_id;
  uint64_t echo_time;
  float capacity;
  node_phase_e phase;
} wire_ping_response_t;

// --- PingCapacity ---

typedef struct {
  uint64_t message_id;
  node_id_t source;
  float capacity;
  node_phase_e phase;
} wire_ping_capacity_t;

typedef struct {
  float latency_ms;
  float capacity;
  node_phase_e phase;
} wire_peer_sample_t;

// --- PingCapacityResponse ---

typedef struct {
  uint64_t message_id;
  float capacity;
  node_phase_e phase;
} wire_ping_capacity_response_t;

// --- PingBlock ---

typedef struct {
  uint64_t message_id;
  uint8_t block_hash[32];
} wire_ping_block_t;

// --- PingBlockResponse ---

typedef struct {
  uint64_t message_id;
  uint8_t exists;
  uint32_t fib;
  uint8_t healthy;
} wire_ping_block_response_t;

// --- FindBlock ---

typedef struct {
  uint64_t message_id;
  uint8_t block_hash[32];
  uint8_t ttl;
  uint8_t visited_bloom[WIRE_MAX_VISITED_BLOOM];
  uint16_t visited_count;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
  uint64_t start_time;
  node_id_t original_source;
} wire_find_block_t;

// --- FindBlockResponse ---

typedef struct {
  uint64_t message_id;
  uint8_t found;
  node_id_t holder;
  uint32_t fib;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
  uint64_t latency_ms;
} wire_find_block_response_t;

// --- FindNode ---

typedef struct {
  uint64_t message_id;
  node_id_t target_id;
} wire_find_node_t;

// --- FindNodeResponse ---

typedef struct {
  uint64_t message_id;
  node_id_t closest_nodes[8];
  uint8_t closest_count;
} wire_find_node_response_t;

// --- StoreBlock ---

typedef struct {
  uint64_t message_id;
  uint8_t block_hash[32];
  uint32_t block_size;
  uint32_t block_fib;
  uint8_t replicas_needed;
  uint8_t max_hops;
  uint8_t visited_bloom[WIRE_MAX_VISITED_BLOOM];
  uint16_t visited_count;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
  uint64_t start_time;
  uint8_t carry_data;
  uint8_t* block_data;
  size_t block_data_len;
} wire_store_block_t;

// --- StoreBlockResponse ---

typedef struct {
  uint64_t message_id;
  uint8_t accepted;
  node_id_t holder;
  uint8_t replicas_remaining;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
  uint64_t latency_ms;
} wire_store_block_response_t;

// --- SeekingBlocks ---

typedef struct {
  uint64_t message_id;
  float capacity;
  uint8_t** exclude_hashes;
  size_t exclude_count;
} wire_seeking_blocks_t;

typedef struct {
  uint8_t hash[32];
  uint32_t fib;
  uint32_t size;
} wire_block_offer_t;

// --- SeekingBlocksResponse ---

typedef struct {
  uint64_t message_id;
  wire_block_offer_t offers[WIRE_MAX_OFFERS];
  uint8_t offer_count;
} wire_seeking_blocks_response_t;

// --- RankBlock ---

typedef struct {
  uint8_t block_hash[32];
  uint32_t fib;
  uint32_t count;
  node_id_t origin;
  uint8_t hop_count;
} wire_rank_block_t;

// --- RecallBlock ---

typedef struct {
  uint64_t message_id;
  uint8_t block_hash[32];
} wire_recall_block_t;

// --- RecallAccept / RecallDecline ---

typedef struct {
  uint64_t message_id;
} wire_recall_accept_t;

typedef struct {
  uint64_t message_id;
} wire_recall_decline_t;

// --- RateLimited ---

typedef struct {
  uint64_t message_id;
  uint8_t type;
  uint32_t retry_after_ms;
  float current_limit;
} wire_rate_limited_t;

// Encode functions — return CBOR item (caller must cbor_decref)
cbor_item_t* wire_ping_encode(const wire_ping_t* msg);
cbor_item_t* wire_ping_response_encode(const wire_ping_response_t* msg);
cbor_item_t* wire_ping_capacity_encode(const wire_ping_capacity_t* msg);
cbor_item_t* wire_ping_capacity_response_encode(const wire_ping_capacity_response_t* msg);
cbor_item_t* wire_ping_block_encode(const wire_ping_block_t* msg);
cbor_item_t* wire_ping_block_response_encode(const wire_ping_block_response_t* msg);
cbor_item_t* wire_find_block_encode(const wire_find_block_t* msg);
cbor_item_t* wire_find_block_response_encode(const wire_find_block_response_t* msg);
cbor_item_t* wire_find_node_encode(const wire_find_node_t* msg);
cbor_item_t* wire_find_node_response_encode(const wire_find_node_response_t* msg);
cbor_item_t* wire_store_block_encode(const wire_store_block_t* msg);
cbor_item_t* wire_store_block_response_encode(const wire_store_block_response_t* msg);
cbor_item_t* wire_seeking_blocks_encode(const wire_seeking_blocks_t* msg);
cbor_item_t* wire_seeking_blocks_response_encode(const wire_seeking_blocks_response_t* msg);
cbor_item_t* wire_rank_block_encode(const wire_rank_block_t* msg);
cbor_item_t* wire_recall_block_encode(const wire_recall_block_t* msg);
cbor_item_t* wire_recall_accept_encode(const wire_recall_accept_t* msg);
cbor_item_t* wire_recall_decline_encode(const wire_recall_decline_t* msg);
cbor_item_t* wire_rate_limited_encode(const wire_rate_limited_t* msg);

// Decode functions — fill existing struct, return 0 on success, -1 on error
int wire_ping_decode(cbor_item_t* item, wire_ping_t* msg);
int wire_ping_response_decode(cbor_item_t* item, wire_ping_response_t* msg);
int wire_ping_capacity_decode(cbor_item_t* item, wire_ping_capacity_t* msg);
int wire_ping_capacity_response_decode(cbor_item_t* item, wire_ping_capacity_response_t* msg);
int wire_ping_block_decode(cbor_item_t* item, wire_ping_block_t* msg);
int wire_ping_block_response_decode(cbor_item_t* item, wire_ping_block_response_t* msg);
int wire_find_block_decode(cbor_item_t* item, wire_find_block_t* msg);
int wire_find_block_response_decode(cbor_item_t* item, wire_find_block_response_t* msg);
int wire_find_node_decode(cbor_item_t* item, wire_find_node_t* msg);
int wire_find_node_response_decode(cbor_item_t* item, wire_find_node_response_t* msg);
int wire_store_block_decode(cbor_item_t* item, wire_store_block_t* msg);
int wire_store_block_response_decode(cbor_item_t* item, wire_store_block_response_t* msg);
int wire_seeking_blocks_decode(cbor_item_t* item, wire_seeking_blocks_t* msg);
int wire_seeking_blocks_response_decode(cbor_item_t* item, wire_seeking_blocks_response_t* msg);
int wire_rank_block_decode(cbor_item_t* item, wire_rank_block_t* msg);
int wire_recall_block_decode(cbor_item_t* item, wire_recall_block_t* msg);
int wire_recall_accept_decode(cbor_item_t* item, wire_recall_accept_t* msg);
int wire_recall_decline_decode(cbor_item_t* item, wire_recall_decline_t* msg);
int wire_rate_limited_decode(cbor_item_t* item, wire_rate_limited_t* msg);

// Helper: extract type byte from CBOR item
uint8_t wire_get_type(cbor_item_t* item);

#endif // OFFS_WIRE_H