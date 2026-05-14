//
// Created by victor on 5/14/25.
//

#include "network.h"
#include "wire.h"
#include "find_block.h"
#include "store_block.h"
#include "respiration.h"
#include "../Timer/timer_actor.h"
#include "../Bloom/elastic_bloom_filter.h"
#include "../Util/allocator.h"
#include <string.h>
#include <time.h>

// Forward declarations for internal handlers
static void network_sync_hebbian_to_rings(network_t* network);

network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool) {
  network_t* network = get_clear_memory(sizeof(network_t));
  network->authority = authority;
  network->block_cache = block_cache;
  network->timer = timer;
  network->pool = pool;
  network->running = ATOMIC_VAR_INIT(0);
  network->rings = ring_set_create(0, 0, 0);
  network->latency_cache = latency_cache_create(0);
  eabf_table_init(&network->eabf_table, 16);
  eabf_ttl_table_init(&network->eabf_ttl, 64);
  hebbian_table_init(&network->hebbian, 32);

  gossip_handle_init(&network->gossip,
                     GOSSIP_INIT_INTERVAL_S,
                     GOSSIP_INIT_COUNT,
                     GOSSIP_STEADY_INTERVAL_S,
                     GOSSIP_TIMEOUT_MS);

  actor_init(&network->actor, network, network_dispatch, pool);

  // Start gossip timer: first tick in init_interval_s, then recurring
  network->gossip_timer_id = timer_actor_set(timer,
      (uint64_t)GOSSIP_INIT_INTERVAL_S * 1000,
      (uint64_t)GOSSIP_INIT_INTERVAL_S * 1000,
      &network->actor,
      NETWORK_GOSSIP_TICK);

  // Start EABF maintenance sweep: recurring 60-second timer
  network->eabf_maintenance_timer_id = timer_actor_set(timer,
      EABF_MAINTENANCE_MS,
      EABF_MAINTENANCE_MS,
      &network->actor,
      NETWORK_EABF_EXPIRE);

  return network;
}

void network_destroy(network_t* network) {
  if (network == NULL) return;
  if (network->gossip_timer_id != 0) {
    timer_actor_cancel(network->timer, network->gossip_timer_id);
  }
  if (network->eabf_maintenance_timer_id != 0) {
    timer_actor_cancel(network->timer, network->eabf_maintenance_timer_id);
  }
  gossip_handle_deinit(&network->gossip);
  ring_set_destroy(network->rings);
  latency_cache_destroy(network->latency_cache);
  eabf_table_deinit(&network->eabf_table);
  eabf_ttl_table_deinit(&network->eabf_ttl);
  hebbian_table_deinit(&network->hebbian);
  actor_destroy(&network->actor);
  free(network);
}

// --- Ping handler ---

typedef struct {
  uint64_t message_id;
  uint64_t timestamp;
} ping_payload_t;

static void network_handle_ping(network_t* network, message_t* msg) {
  ping_payload_t* ping = (ping_payload_t*)msg->payload;
  if (ping == NULL) return;
  // Respond with cached capacity and phase, echo timestamp for RTT
  ping_payload_t* response = get_clear_memory(sizeof(ping_payload_t));
  response->message_id = ping->message_id;
  response->timestamp = ping->timestamp;
  // Send response back via actor_send (reply_to will be set by the caller)
  message_t response_msg;
  response_msg.type = NETWORK_PING_RESPONSE;
  response_msg.payload = response;
  response_msg.payload_destroy = free;
  // If the ping came from a known peer, send response to that peer's actor
  // For now, we echo back through whatever channel sent the ping
  (void)network;
  // TODO: Send response to peer connection when QUIC is wired up
}

static void network_handle_ping_response(network_t* network, message_t* msg) {
  ping_payload_t* response = (ping_payload_t*)msg->payload;
  if (response == NULL) return;
  // Calculate RTT from echo_time
  uint64_t now = (uint64_t)time(NULL) * 1000;
  uint64_t rtt_ms = now - response->timestamp;
  (void)rtt_ms;
  // TODO: Update latency cache and Hebbian weights when ring table is implemented
  (void)network;
}

// --- PingBlock handler ---

static void network_handle_ping_block(network_t* network, message_t* msg) {
  wire_ping_block_t* ping = (wire_ping_block_t*)msg->payload;
  if (ping == NULL) return;

  // Check if we have the block locally
  // TODO: Look up block in block_cache via actor_send when wired up
  uint8_t exists = 0;
  uint32_t fib = 0;
  uint8_t healthy = 0;

  // Respond with PingBlockResponse
  wire_ping_block_response_t* response = get_clear_memory(sizeof(wire_ping_block_response_t));
  response->message_id = ping->message_id;
  response->exists = exists;
  response->fib = fib;
  response->healthy = healthy;

  message_t response_msg;
  response_msg.type = NETWORK_PING_BLOCK_RESPONSE;
  response_msg.payload = response;
  response_msg.payload_destroy = free;
  // TODO: Send response to peer connection when QUIC is wired up
  (void)network;
}

static void network_handle_ping_block_response(network_t* network, message_t* msg) {
  wire_ping_block_response_t* response = (wire_ping_block_response_t*)msg->payload;
  if (response == NULL) return;

  // On successful block existence confirmation, strengthen Hebbian weight
  if (response->exists) {
    // Frequency rule: strengthen connection to the peer that confirmed
    // The peer's node_id comes from the message source, not the response itself
    // TODO: Look up source from query tracking when wired up
    float delta = hebbian_compute_delta(0, HEBBIAN_FIND_BLOCK_MULTIPLIER);
    (void)delta;
  }
  (void)network;
}

// --- Gossip tick handler ---

static void network_handle_gossip_tick(network_t* network, message_t* msg) {
  (void)msg;
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;

  // Expire stale gossip exchanges and timed-out queries
  gossip_handle_expire_queries(&network->gossip, now_ms);

  // Apply Hebbian decay on each gossip tick
  hebbian_decay(&network->hebbian);

  // Tick the scheduler to determine if we should gossip
  gossip_scheduler_tick(&network->gossip.scheduler, now_ms);
  if (!network->gossip.scheduler.should_gossip) return;

  // Select gossip targets from ring table
  // TODO: Select random peers from ring table and send PingCapacity
  // For now, the tick just drives the scheduler state machine
}

static void network_handle_gossip_expire(network_t* network, message_t* msg) {
  (void)msg;
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;
  gossip_handle_expire_queries(&network->gossip, now_ms);
}

// --- PingCapacity handler ---
// A peer sends PingCapacity to exchange capacity/phase info and measure latency

static void network_handle_ping_capacity(network_t* network, message_t* msg) {
  wire_ping_capacity_t* ping = (wire_ping_capacity_t*)msg->payload;
  if (ping == NULL) return;

  // Look up or create node entry for the sender
  net_node_t* node = ring_set_find_by_id(network->rings, &ping->source);
  if (node != NULL) {
    // Update cached capacity/phase from the ping
    node->capacity = ping->capacity;
    node->phase = ping->phase;
    node->last_gossip_time = (uint64_t)time(NULL) * 1000;
    net_node_record_success(node);
  }

  // Build response with our own capacity/phase
  wire_ping_capacity_response_t* response = get_clear_memory(sizeof(wire_ping_capacity_response_t));
  response->message_id = ping->message_id;
  response->capacity = atomic_load(&network->authority->capacity);
  response->phase = atomic_load(&network->authority->phase);

  message_t response_msg;
  response_msg.type = NETWORK_PING_CAPACITY_RESPONSE;
  response_msg.payload = response;
  response_msg.payload_destroy = free;
  // TODO: Send response to peer connection when QUIC is wired up
  (void)response_msg;
}

static void network_handle_ping_capacity_response(network_t* network, message_t* msg) {
  wire_ping_capacity_response_t* response = (wire_ping_capacity_response_t*)msg->payload;
  if (response == NULL) return;

  // Update latency and ring table for the peer that responded
  // The peer's capacity/phase are in the response
  float latency_ms = 0;  // RTT will be calculated from query timestamps
  (void)latency_ms;

  // Update Hebbian weights on successful response
  // TODO: Wire Hebbian weight update when hebbian module is implemented
  (void)network;
}

// --- FindNode handler ---
// Returns the K closest nodes to a target ID from our ring table

static void network_handle_find_node(network_t* network, message_t* msg) {
  wire_find_node_t* find = (wire_find_node_t*)msg->payload;
  if (find == NULL) return;

  wire_find_node_response_t* response = get_clear_memory(sizeof(wire_find_node_response_t));
  response->message_id = find->message_id;
  response->closest_count = 0;

  // Walk rings from lowest latency to highest, collecting up to 8 closest nodes
  for (size_t ring_idx = 0; ring_idx < network->rings->ring_count && response->closest_count < 8; ring_idx++) {
    ring_t* ring = &network->rings->rings[ring_idx];
    for (int node_idx = 0; node_idx < ring->primary.length && response->closest_count < 8; node_idx++) {
      net_node_t* node = ring->primary.data[node_idx];
      // Skip rendezvous-only nodes
      if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;
      memcpy(&response->closest_nodes[response->closest_count], &node->id, sizeof(node_id_t));
      response->closest_count++;
    }
  }

  message_t response_msg;
  response_msg.type = NETWORK_FIND_NODE_RESPONSE;
  response_msg.payload = response;
  response_msg.payload_destroy = free;
  // TODO: Send response to peer connection when QUIC is wired up
  (void)response_msg;
}

static void network_handle_find_node_response(network_t* network, message_t* msg) {
  wire_find_node_response_t* response = (wire_find_node_response_t*)msg->payload;
  if (response == NULL) return;

  // Insert responding peers into our ring table based on measured latency
  for (uint8_t index = 0; index < response->closest_count; index++) {
    float latency_ms = 0;
    latency_cache_get(network->latency_cache, &response->closest_nodes[index], &latency_ms);
    uint32_t latency_us = (uint32_t)(latency_ms * 1000);
    net_node_t* node = net_node_create(&response->closest_nodes[index], 0, 0);
    if (node != NULL) {
      ring_set_insert(network->rings, node, latency_us);
    }
  }
  (void)network;
}

// --- FindBlock handler ---
// Directed walk: check local → EABF gravity wells → ring candidates → forward

static void network_handle_find_block(network_t* network, message_t* msg) {
  wire_find_block_t* find = (wire_find_block_t*)msg->payload;
  if (find == NULL) return;

  // Build find_block_state from the wire message
  find_block_state_t state;
  memset(&state, 0, sizeof(state));
  state.message_id = find->message_id;
  memcpy(state.block_hash, find->block_hash, 32);
  state.ttl = find->ttl;
  memcpy(state.visited_bloom, find->visited_bloom, FIND_BLOCK_MAX_VISITED_BLOOM);
  state.visited_count = find->visited_count;
  state.path_len = find->path_len;
  if (state.path_len > FIND_BLOCK_MAX_PATH) state.path_len = FIND_BLOCK_MAX_PATH;
  memcpy(state.path, find->path, state.path_len * sizeof(node_id_t));
  state.start_time_ms = find->start_time;
  memcpy(&state.original_source, &find->original_source, sizeof(node_id_t));

  // Execute FindBlock logic
  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &network->eabf_table,
      &network->eabf_ttl,
      network->rings,
      &network->authority->local_id,
      &state,
      next_hops,
      &next_hop_count);

  switch (result) {
    case FIND_BLOCK_FOUND:
      // Block is local — caller should have checked block_cache before dispatching
      // This shouldn't happen; if it does, treat as NOT_FOUND
      break;

    case FIND_BLOCK_NOT_FOUND:
    case FIND_BLOCK_TTL_EXPIRED: {
      // On TTL expiry, subscribe block_hash in our EABFs as negative info
      if (result == FIND_BLOCK_TTL_EXPIRED) {
        for (size_t index = 0; index < network->eabf_table.count; index++) {
          eabf_t* eabf = network->eabf_table.entries[index].eabf;
          if (eabf != NULL) {
            eabf_subscribe(eabf, state.block_hash, 32);
          }
        }
      }
      // Send NOT_FOUND response back along the path
      // TODO: Send response via QUIC when wired up
      break;
    }

    case FIND_BLOCK_FORWARDING: {
      // Add self to path
      if (state.path_len < FIND_BLOCK_MAX_PATH) {
        memcpy(&state.path[state.path_len], &network->authority->local_id, sizeof(node_id_t));
        state.path_len++;
      }
      // Add self to visited bloom
      find_block_add_visited(state.visited_bloom, &state.visited_count, state.block_hash);
      // Decrement TTL
      state.ttl--;

      // Forward to each selected next-hop
      for (size_t hop = 0; hop < next_hop_count; hop++) {
        // Build forwarded FindBlock message
        wire_find_block_t* forward = get_clear_memory(sizeof(wire_find_block_t));
        if (forward == NULL) continue;
        forward->message_id = state.message_id;
        memcpy(forward->block_hash, state.block_hash, 32);
        forward->ttl = state.ttl;
        memcpy(forward->visited_bloom, state.visited_bloom, WIRE_MAX_VISITED_BLOOM);
        forward->visited_count = state.visited_count;
        memcpy(forward->path, state.path, state.path_len * sizeof(node_id_t));
        forward->path_len = state.path_len;
        forward->start_time = state.start_time_ms;
        memcpy(&forward->original_source, &state.original_source, sizeof(node_id_t));

        // TODO: Send forward to next_hops[hop] via QUIC when wired up
        free(forward);
      }
      break;
    }
  }
}

static void network_handle_find_block_response(network_t* network, message_t* msg) {
  wire_find_block_response_t* response = (wire_find_block_response_t*)msg->payload;
  if (response == NULL) return;

  if (response->found) {
    // Block found — apply Hebbian learning rules along the response path
    // Use latency from start_time to now as the total search time
    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
    uint64_t latency_ms = 0;
    if (response->latency_ms > 0) {
      latency_ms = response->latency_ms;
    } else if (now_ms > 0) {
      latency_ms = now_ms;
    }

    hebbian_apply_success(&network->hebbian, response->path, response->path_len,
                          latency_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER);
    network_sync_hebbian_to_rings(network);

    // Populate EABFs along the path
    for (size_t index = 0; index < network->eabf_table.count; index++) {
      eabf_t* eabf = network->eabf_table.entries[index].eabf;
      if (eabf != NULL) {
        eabf_subscribe(eabf, response->holder.hash, 32);
      }
    }
  } else {
    // Block not found — subscribe block_hash in EABFs as negative info
    // This is handled by TTL_EXPIRED in the forwarding path
  }
}

// --- Hebbian weight sync ---
// After Hebbian updates, sync weights back to ring table nodes

static void network_sync_hebbian_to_rings(network_t* network) {
  for (size_t index = 0; index < network->hebbian.count; index++) {
    hebbian_weight_t* weight = &network->hebbian.entries[index];
    net_node_t* node = ring_set_find_by_id(network->rings, &weight->peer_id);
    if (node != NULL) {
      node->weight = weight->weight;
    }
  }
}

// --- StoreBlock handler ---
// Accept/decline/forward storage request

static void network_handle_store_block(network_t* network, message_t* msg) {
  wire_store_block_t* store = (wire_store_block_t*)msg->payload;
  if (store == NULL) return;

  // Build store_block_state from the wire message
  store_block_state_t state;
  memset(&state, 0, sizeof(state));
  state.message_id = store->message_id;
  memcpy(state.block_hash, store->block_hash, 32);
  state.block_size = store->block_size;
  state.block_fib = store->block_fib;
  state.replicas_needed = store->replicas_needed;
  state.max_hops = store->max_hops;
  memcpy(state.visited_bloom, store->visited_bloom, STORE_BLOCK_MAX_VISITED_BLOOM);
  state.visited_count = store->visited_count;
  state.path_len = store->path_len;
  if (state.path_len > STORE_BLOCK_MAX_HOPS) state.path_len = STORE_BLOCK_MAX_HOPS;
  memcpy(state.path, store->path, state.path_len * sizeof(node_id_t));
  state.start_time_ms = store->start_time;
  state.carry_data = store->carry_data;

  // Get current capacity/phase from authority (atomics)
  float local_capacity = atomic_load(&network->authority->capacity);
  node_phase_e local_phase = atomic_load(&network->authority->phase);

  // Execute StoreBlock logic
  net_node_t* next_hops[STORE_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  store_block_result_e result = store_block_execute(
      &network->eabf_table,
      network->rings,
      &network->authority->local_id,
      local_capacity,
      local_phase,
      &state,
      next_hops,
      &next_hop_count);

  switch (result) {
    case STORE_BLOCK_ACCEPTED: {
      // Block accepted — write to sections, update index, put in hot cache
      // The caller (block_cache actor) handles actual storage.
      // Decrement replicas_needed and forward if more replicas needed.
      if (state.replicas_needed > 0) {
        state.replicas_needed--;
      }
      // TODO: Store block in block_cache via actor_send
      // TODO: If replicas_needed > 0, forward with decremented count
      break;
    }

    case STORE_BLOCK_DECLINED:
    case STORE_BLOCK_MAX_HOPS_REACHED: {
      // Decline — respond with not accepted
      // TODO: Send StoreBlockResponse(accepted=false) via QUIC when wired up
      break;
    }

    case STORE_BLOCK_FORWARDING: {
      // Add self to path and visited bloom, decrement max_hops
      if (state.path_len < STORE_BLOCK_MAX_HOPS) {
        memcpy(&state.path[state.path_len], &network->authority->local_id, sizeof(node_id_t));
        state.path_len++;
      }
      find_block_add_visited(state.visited_bloom, &state.visited_count, state.block_hash);
      state.max_hops--;

      // Forward to each selected next-hop
      for (size_t hop = 0; hop < next_hop_count; hop++) {
        wire_store_block_t* forward = get_clear_memory(sizeof(wire_store_block_t));
        if (forward == NULL) continue;
        forward->message_id = state.message_id;
        memcpy(forward->block_hash, state.block_hash, 32);
        forward->block_size = state.block_size;
        forward->block_fib = state.block_fib;
        forward->replicas_needed = state.replicas_needed;
        forward->max_hops = state.max_hops;
        memcpy(forward->visited_bloom, state.visited_bloom, WIRE_MAX_VISITED_BLOOM);
        forward->visited_count = state.visited_count;
        memcpy(forward->path, state.path, state.path_len * sizeof(node_id_t));
        forward->path_len = state.path_len;
        forward->start_time = state.start_time_ms;
        forward->carry_data = state.carry_data;
        // TODO: Copy block_data if carry_data is true

        // TODO: Send forward to next_hops[hop] via QUIC when wired up
        free(forward);
      }
      break;
    }
  }
}

static void network_handle_store_block_response(network_t* network, message_t* msg) {
  wire_store_block_response_t* response = (wire_store_block_response_t*)msg->payload;
  if (response == NULL) return;

  if (response->accepted) {
    // Block accepted — apply Hebbian learning rules along the response path
    uint64_t latency_ms = response->latency_ms;
    hebbian_apply_success(&network->hebbian, response->path, response->path_len,
                          latency_ms, HEBBIAN_STORE_BLOCK_EXHALE_MULTIPLIER);
    network_sync_hebbian_to_rings(network);

    // Populate EABFs along the response path
    for (size_t index = 0; index < network->eabf_table.count; index++) {
      eabf_t* eabf = network->eabf_table.entries[index].eabf;
      if (eabf != NULL) {
        eabf_subscribe(eabf, response->holder.hash, 32);
      }
    }
  }
}

// --- SeekingBlocks handler ---
// Inhale: peer asks what blocks we have that they might want

static void network_handle_seeking_blocks(network_t* network, message_t* msg) {
  wire_seeking_blocks_t* seeking = (wire_seeking_blocks_t*)msg->payload;
  if (seeking == NULL) return;

  float local_capacity = atomic_load(&network->authority->capacity);

  // Only respond if we have blocks and the requester has capacity < 50%
  if (local_capacity <= 0.0f) {
    // No blocks to offer
    return;
  }

  // Build response with offers from our local block index
  // TODO: Query block_cache for available blocks when wired up
  wire_seeking_blocks_response_t* response = get_clear_memory(sizeof(wire_seeking_blocks_response_t));
  response->message_id = seeking->message_id;
  response->offer_count = 0;

  // TODO: Select up to MAX_OFFERS blocks from local index
  // Use PICK-BLOCK-FOR-REPRESENTATION: walk ranks from highest fib downward,
  // probability proportional to fibonacci(fib)
  // Exclude hashes in seeking->exclude_hashes

  message_t response_msg;
  response_msg.type = NETWORK_SEEKING_BLOCKS_RESPONSE;
  response_msg.payload = response;
  response_msg.payload_destroy = free;
  // TODO: Send response via QUIC when wired up
  (void)response_msg;
}

static void network_handle_seeking_blocks_response(network_t* network, message_t* msg) {
  wire_seeking_blocks_response_t* response = (wire_seeking_blocks_response_t*)msg->payload;
  if (response == NULL) return;

  float local_capacity = atomic_load(&network->authority->capacity);

  // Evaluate each offer: if we should pull (capacity < 50% and block not stored)
  if (!respiration_should_inhale(local_capacity)) {
    return;  // We're not in inhale phase anymore
  }

  for (uint8_t index = 0; index < response->offer_count; index++) {
    wire_block_offer_t* offer = &response->offers[index];
    // TODO: Check if block is already stored in block_cache
    // If not stored and capacity allows, send FindBlock for this hash
    (void)offer;
  }
}

// --- RankBlock handler ---
// Fire-and-forget: upgrade our local rank or initiate seek

static void network_handle_rank_block(network_t* network, message_t* msg) {
  wire_rank_block_t* rank = (wire_rank_block_t*)msg->payload;
  if (rank == NULL) return;

  float local_capacity = atomic_load(&network->authority->capacity);

  // TODO: Look up block in local index when wired up
  // If we have the block and msg.fib > local.fib → upgrade
  // If we don't have the block and capacity < 50% → initiate FindBlock
  // If hop_count < MAX_RANK_HOPS → forward to random subset

  (void)network;
  (void)rank;
  (void)local_capacity;
}

// --- RecallBlock handler ---

static void network_handle_recall_block(network_t* network, message_t* msg) {
  wire_recall_block_t* recall = (wire_recall_block_t*)msg->payload;
  if (recall == NULL) return;

  float local_capacity = atomic_load(&network->authority->capacity);

  // If capacity < 50% → accept recall, otherwise decline
  if (local_capacity < RESPIRATION_INHALE_THRESHOLD) {
    // Send RecallAccept
    wire_recall_accept_t* response = get_clear_memory(sizeof(wire_recall_accept_t));
    response->message_id = recall->message_id;

    message_t response_msg;
    response_msg.type = NETWORK_RECALL_ACCEPT;
    response_msg.payload = response;
    response_msg.payload_destroy = free;
    // TODO: Send response via QUIC when wired up
    (void)response_msg;
  } else {
    // Send RecallDecline
    wire_recall_decline_t* response = get_clear_memory(sizeof(wire_recall_decline_t));
    response->message_id = recall->message_id;

    message_t response_msg;
    response_msg.type = NETWORK_RECALL_DECLINE;
    response_msg.payload = response;
    response_msg.payload_destroy = free;
    // TODO: Send response via QUIC when wired up
    (void)response_msg;
  }
}

static void network_handle_recall_accept(network_t* network, message_t* msg) {
  wire_recall_accept_t* accept = (wire_recall_accept_t*)msg->payload;
  if (accept == NULL) return;

  // Load block from sections and send StoreBlock with reason=RECALL
  // TODO: Look up block in local cache and send StoreBlock when wired up
  (void)network;
  (void)accept;
}

static void network_handle_recall_decline(network_t* network, message_t* msg) {
  wire_recall_decline_t* decline = (wire_recall_decline_t*)msg->payload;
  if (decline == NULL) return;

  // Remove block_hash from EABF_{self→source}
  // TODO: Remove from EABF when source node ID is available
  (void)network;
  (void)decline;
}

// --- EABF expire handler ---
// When a timer fires for an EABF entry TTL, remove the fingerprint from the EABF.
// The timer payload contains the timer_id which maps to an eabf_ttl_entry_t.

static void network_handle_eabf_expire(network_t* network, message_t* msg) {
  // The timer_completion_payload_t has timer_id that we can look up in the TTL table
  timer_completion_payload_t* completion = (timer_completion_payload_t*)msg->payload;
  if (completion == NULL) return;

  eabf_ttl_entry_t entry;
  if (eabf_ttl_table_remove_by_timer(&network->eabf_ttl, completion->timer_id, &entry) == 0) {
    // Found the entry — remove the fingerprint from the EABF
    eabf_t* eabf = eabf_table_lookup(&network->eabf_table, &entry.peer_id);
    if (eabf != NULL) {
      elastic_bloom_filter_t* level = eabf_get_level(eabf, entry.level);
      if (level != NULL && entry.bucket_index < level->bucket_count) {
        // Remove the fingerprint from the bucket
        ebf_bucket_entry_t** bucket = &level->buckets[entry.bucket_index];
        ebf_bucket_entry_t* prev = NULL;
        ebf_bucket_entry_t* current = *bucket;
        while (current != NULL) {
          if (current->fingerprint == entry.fingerprint) {
            if (prev == NULL) {
              *bucket = current->next;
            } else {
              prev->next = current->next;
            }
            free(current);
            break;
          }
          prev = current;
          current = current->next;
        }
        // If bucket is now empty, clear the bitset bit
        if (*bucket == NULL) {
          bitset_set(level->bits, entry.bucket_index, false);
        }
      }
    }
  }
  (void)network;
}

// --- Network dispatch ---

void network_dispatch(void* state, message_t* msg) {
  network_t* network = (network_t*)state;
  switch (msg->type) {
    case NETWORK_PING:
      network_handle_ping(network, msg);
      break;
    case NETWORK_PING_RESPONSE:
      network_handle_ping_response(network, msg);
      break;
    case NETWORK_PING_CAPACITY:
      network_handle_ping_capacity(network, msg);
      break;
    case NETWORK_PING_CAPACITY_RESPONSE:
      network_handle_ping_capacity_response(network, msg);
      break;
    case NETWORK_PING_BLOCK:
      network_handle_ping_block(network, msg);
      break;
    case NETWORK_PING_BLOCK_RESPONSE:
      network_handle_ping_block_response(network, msg);
      break;
    case NETWORK_FIND_BLOCK:
      network_handle_find_block(network, msg);
      break;
    case NETWORK_FIND_BLOCK_RESPONSE:
      network_handle_find_block_response(network, msg);
      break;
    case NETWORK_FIND_NODE:
      network_handle_find_node(network, msg);
      break;
    case NETWORK_FIND_NODE_RESPONSE:
      network_handle_find_node_response(network, msg);
      break;
    case NETWORK_STORE_BLOCK:
      network_handle_store_block(network, msg);
      break;
    case NETWORK_STORE_BLOCK_RESPONSE:
      network_handle_store_block_response(network, msg);
      break;
    case NETWORK_SEEKING_BLOCKS:
      network_handle_seeking_blocks(network, msg);
      break;
    case NETWORK_SEEKING_BLOCKS_RESPONSE:
      network_handle_seeking_blocks_response(network, msg);
      break;
    case NETWORK_RANK_BLOCK:
      network_handle_rank_block(network, msg);
      break;
    case NETWORK_RECALL_BLOCK:
      network_handle_recall_block(network, msg);
      break;
    case NETWORK_RECALL_ACCEPT:
      network_handle_recall_accept(network, msg);
      break;
    case NETWORK_RECALL_DECLINE:
      network_handle_recall_decline(network, msg);
      break;
    case NETWORK_RATE_LIMITED:
      break;
    case NETWORK_EABF_EXPIRE:
      network_handle_eabf_expire(network, msg);
      break;
    case NETWORK_GOSSIP_TICK:
      network_handle_gossip_tick(network, msg);
      break;
    case NETWORK_GOSSIP_EXPIRE:
      network_handle_gossip_expire(network, msg);
      break;
    case NETWORK_QUIC_DATA:
      break;
    case NETWORK_QUIC_CONNECTED:
      break;
    case NETWORK_QUIC_DISCONNECTED:
      break;
    default:
      break;
  }
  if (msg->payload != NULL && msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}