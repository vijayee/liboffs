//
// Created by victor on 5/14/25.
//

#ifndef OFFS_WIRE_H
#define OFFS_WIRE_H

#include "node_id.h"
#include "authority.h"
#include "ring.h"
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
#define WIRE_SALUTATION          20
#define WIRE_RELAY_SEND          30
#define WIRE_RELAY_RECEIVED      31
#define WIRE_ADDR_REQUEST        32
#define WIRE_ADDR_RESPONSE       33
#define WIRE_RELAY_CHALLENGE     34
#define WIRE_RELAY_CHALLENGE_RESPONSE 35

// Magic number for protocol identification
#define WIRE_MAGIC 0x4F464653  // "OFFS"

// Limits from Network Design spec
#define WIRE_MAX_PATH             6
#define WIRE_MAX_VISITED_BLOOM    256  // bytes = 2048 bits
#define WIRE_MAX_OFFERS           20
#define WIRE_MAX_SAMPLES          8

// Max hops for a forwarded search — separate from the per-protocol
// *_FORWARD_FANOUT constants (branch width = how many next-hops to contact at
// each hop). Was conflated with *_FORWARD_FANOUT=3, silently capping reach at
// 3 hops even though WIRE_MAX_PATH / FIND_BLOCK_MAX_PATH = 6. See audit #22.
#define FORWARD_TTL 6

// Closest-N protocol limits
#define CLOSEST_NODES_MAX_PATH       6
#define CLOSEST_NODES_MAX_VISITED    256  // bytes = 2048 bits
#define CLOSEST_NODES_MAX_COUNT      8
#define CLOSEST_NODES_FORWARD_FANOUT 3
#define CLOSEST_NODES_MAX_RING_SAMPLES 8
#define MEASURE_NODES_MAX_TARGETS    8

// --- Ping ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint64_t timestamp;
} wire_ping_t;

// --- PingResponse ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
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
  node_id_t sender_id;
  float capacity;
  node_phase_e phase;
} wire_ping_capacity_response_t;

// --- PingBlock ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t block_hash[32];
} wire_ping_block_t;

// --- PingBlockResponse ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
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
  uint8_t block_hash[32];
  uint8_t found;
  node_id_t holder;
  uint32_t fib;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
  uint64_t latency_ms;
  uint8_t* block_data;       /* block content (NULL if not found) */
  size_t   block_data_len;   /* length of block data */
  uint32_t block_fib;        /* FIB counter from the holder */
} wire_find_block_response_t;

// --- FindNode ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  node_id_t target_id;
} wire_find_node_t;

// --- FindNodeResponse ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
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
  uint8_t block_hash[32];  // hash of the stored block (not holder.hash)
} wire_store_block_response_t;

// --- SeekingBlocks ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
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
  node_id_t sender_id;
  wire_block_offer_t offers[WIRE_MAX_OFFERS];
  uint8_t offer_count;
} wire_seeking_blocks_response_t;

// --- RankBlock ---

#define RANK_BLOCK_MAX_HOPS 6
#define RANK_BLOCK_FORWARD_FANOUT 3

typedef struct {
  uint8_t block_hash[32];
  uint32_t fib;
  uint32_t count;
  node_id_t origin;
  uint8_t hop_count;
  uint8_t visited_bloom[WIRE_MAX_VISITED_BLOOM];
  uint16_t visited_count;
  node_id_t path[WIRE_MAX_PATH];
  uint8_t path_len;
} wire_rank_block_t;

// --- RecallBlock ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t block_hash[32];
} wire_recall_block_t;

// --- RecallAccept / RecallDecline ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t block_hash[32];
  uint8_t* block_data;       /* the requested block's data */
  size_t   block_data_len;  /* length of block data */
  uint32_t block_fib;        /* FIB counter for the block */
} wire_recall_accept_t;

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t block_hash[32];  // hash of the block that was declined
} wire_recall_decline_t;

// --- RateLimited ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t type;
  uint32_t retry_after_ms;
  float current_limit;
} wire_rate_limited_t;

// --- Salutation (identity handshake) ---

typedef struct {
  node_id_t sender_id;
  uint8_t* public_key;
  size_t    public_key_len;
} wire_salutation_t;

// --- RelaySend ---

typedef struct wire_relay_send_t {
  uint32_t src_endpoint_id;
  uint32_t dest_endpoint_id;
  uint8_t* payload;
  size_t payload_len;
} wire_relay_send_t;

// --- RelayReceived ---

typedef struct wire_relay_received_t {
  uint32_t src_endpoint_id;
  uint8_t* payload;
  size_t payload_len;
} wire_relay_received_t;

// --- RelayChallenge / RelayChallengeResponse ---
// Inner wire messages for the relay signed-nonce challenge (audit #8 relay).
// The challenge carries a fresh nonce + the challenger's relay endpoint id
// (so the responder can route the response back). The response carries the
// nonce + the responder's public_key + a signature of the nonce under the
// private key for public_key. The challenger verifies BLAKE3(public_key)
// == responder_id and the signature.

typedef struct wire_relay_challenge_t {
  node_id_t challenger_id;         // the node sending the challenge
  uint32_t challenger_endpoint_id; // the challenger's relay endpoint (route the response back)
  uint8_t  nonce[32];              // a fresh random nonce
} wire_relay_challenge_t;

typedef struct wire_relay_challenge_response_t {
  node_id_t responder_id;  // the node responding (the original relayed sender)
  uint8_t  nonce[32];      // the nonce being responded to
  uint8_t* public_key;     // the responder's public key (BLAKE3 of this must equal responder_id)
  size_t   public_key_len;
  uint8_t* signature;      // signature of the nonce under the private key for public_key
  size_t   signature_len;
} wire_relay_challenge_response_t;

// --- AddrRequest ---

typedef struct wire_addr_request_t {
  uint64_t message_id;
} wire_addr_request_t;

// --- AddrResponse ---

typedef struct wire_addr_response_t {
  uint64_t message_id;
  uint32_t endpoint_id;
  uint32_t reflexive_addr;
  uint16_t reflexive_port;
} wire_addr_response_t;

// --- Gossip (ring maintenance / peer discovery) ---

#define WIRE_GOSSIP            21
#define WIRE_GOSSIP_PULL       22

// --- Closest-N protocol (Meridian proximity routing) ---
#define WIRE_CLOSEST_NODES             23
#define WIRE_CLOSEST_NODES_RESPONSE    24
#define WIRE_MEASURE_NODES             25
#define WIRE_MEASURE_NODES_RESPONSE    26
#define WIRE_CLOSEST_NODES_PROGRESS    27

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;
  uint16_t  rendezvous_port;
  node_id_t targets[RING_MAX_RINGS];  // 1 random node per non-empty ring
  uint8_t   target_count;
} wire_gossip_t;

typedef struct {
  uint64_t  message_id;
  node_id_t sender_id;
  uint32_t  rendezvous_addr;
  uint16_t  rendezvous_port;
  node_id_t targets[RING_MAX_RINGS];
  uint8_t   target_count;
} wire_gossip_pull_t;

// --- ClosestNodes (Meridian proximity routing) ---

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  node_id_t target_id;
  uint8_t count;
  uint16_t beta_numerator;
  uint16_t beta_denominator;
  uint8_t ttl;
  uint8_t visited_bloom[CLOSEST_NODES_MAX_VISITED];
  uint16_t visited_count;
  node_id_t path[CLOSEST_NODES_MAX_PATH];
  uint8_t path_len;
  uint64_t start_time;
  node_id_t original_source;
} wire_closest_nodes_t;

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  node_id_t target_id;
  uint8_t found;
  node_id_t closest;
  uint32_t closest_latency_us;
  node_id_t path[CLOSEST_NODES_MAX_PATH];
  uint8_t path_len;
  uint64_t latency_us;
  node_id_t ring_nodes[CLOSEST_NODES_MAX_RING_SAMPLES];
  uint32_t ring_latencies_us[CLOSEST_NODES_MAX_RING_SAMPLES];
  uint8_t ring_count;
} wire_closest_nodes_response_t;

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  uint8_t probe_type;
  node_id_t targets[MEASURE_NODES_MAX_TARGETS];
  uint8_t target_count;
} wire_measure_nodes_t;

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  node_id_t targets[MEASURE_NODES_MAX_TARGETS];
  uint32_t latencies_us[MEASURE_NODES_MAX_TARGETS];
  uint8_t target_count;
} wire_measure_nodes_response_t;

typedef struct {
  uint64_t message_id;
  node_id_t sender_id;
  node_id_t target_id;
  uint8_t hop_count;
} wire_closest_nodes_progress_t;

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
cbor_item_t* wire_salutation_encode(const wire_salutation_t* msg);
cbor_item_t* wire_relay_send_encode(const wire_relay_send_t* msg);
cbor_item_t* wire_relay_received_encode(const wire_relay_received_t* msg);
cbor_item_t* wire_relay_challenge_encode(const wire_relay_challenge_t* msg);
cbor_item_t* wire_relay_challenge_response_encode(const wire_relay_challenge_response_t* msg);
cbor_item_t* wire_addr_request_encode(const wire_addr_request_t* msg);
cbor_item_t* wire_addr_response_encode(const wire_addr_response_t* msg);
cbor_item_t* wire_gossip_encode(const wire_gossip_t* msg);
cbor_item_t* wire_gossip_pull_encode(const wire_gossip_pull_t* msg);
cbor_item_t* wire_closest_nodes_encode(const wire_closest_nodes_t* msg);
cbor_item_t* wire_closest_nodes_response_encode(const wire_closest_nodes_response_t* msg);
cbor_item_t* wire_measure_nodes_encode(const wire_measure_nodes_t* msg);
cbor_item_t* wire_measure_nodes_response_encode(const wire_measure_nodes_response_t* msg);
cbor_item_t* wire_closest_nodes_progress_encode(const wire_closest_nodes_progress_t* msg);

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
int wire_salutation_decode(cbor_item_t* item, wire_salutation_t* msg);
int wire_relay_send_decode(cbor_item_t* item, wire_relay_send_t* msg);
int wire_relay_received_decode(cbor_item_t* item, wire_relay_received_t* msg);
int wire_relay_challenge_decode(cbor_item_t* item, wire_relay_challenge_t* msg);
int wire_relay_challenge_response_decode(cbor_item_t* item, wire_relay_challenge_response_t* msg);
int wire_addr_request_decode(cbor_item_t* item, wire_addr_request_t* msg);
int wire_addr_response_decode(cbor_item_t* item, wire_addr_response_t* msg);
int wire_gossip_decode(cbor_item_t* item, wire_gossip_t* msg);
int wire_gossip_pull_decode(cbor_item_t* item, wire_gossip_pull_t* msg);
int wire_closest_nodes_decode(cbor_item_t* item, wire_closest_nodes_t* msg);
int wire_closest_nodes_response_decode(cbor_item_t* item, wire_closest_nodes_response_t* msg);
int wire_measure_nodes_decode(cbor_item_t* item, wire_measure_nodes_t* msg);
int wire_measure_nodes_response_decode(cbor_item_t* item, wire_measure_nodes_response_t* msg);
int wire_closest_nodes_progress_decode(cbor_item_t* item, wire_closest_nodes_progress_t* msg);

// Helper: extract type byte from CBOR item
uint8_t wire_get_type(cbor_item_t* item);
int wire_extract_sender_id(cbor_item_t* item, node_id_t* sender_id);

// Destroy helpers for wire types with nested allocations
// Frees block_data and the struct itself
void wire_store_block_destroy(wire_store_block_t* msg);
// Frees block_data and the struct itself
void wire_find_block_response_destroy(wire_find_block_response_t* msg);
// Frees block_data and the struct itself
void wire_recall_accept_destroy(wire_recall_accept_t* msg);
// Frees exclude_hashes array and each hash pointer, then the struct itself
void wire_seeking_blocks_destroy(wire_seeking_blocks_t* msg);
// Frees payload and the struct itself
void wire_relay_send_destroy(wire_relay_send_t* msg);
// Frees public_key and the struct itself
void wire_salutation_destroy(wire_salutation_t* msg);
// Frees payload and the struct itself
void wire_relay_received_destroy(wire_relay_received_t* msg);
// Frees public_key + signature and the struct itself
void wire_relay_challenge_response_destroy(wire_relay_challenge_response_t* msg);
// No nested allocations; frees the struct itself
void wire_relay_challenge_destroy(wire_relay_challenge_t* msg);

#endif // OFFS_WIRE_H