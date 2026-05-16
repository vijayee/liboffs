//
// Created by victor on 5/14/25.
//

#include "network.h"
#include "wire.h"
#include "quic_listener.h"
#include "find_block.h"
#include "store_block.h"
#include "respiration.h"
#include "peer_connection.h"
#include "timing_wheel.h"
#include "topology_metrics.h"
#include "wanted_list.h"
#include "../Timer/timer_actor.h"
#include "../Bloom/elastic_bloom_filter.h"
#include "../BlockCache/block_cache.h"
#include "../BlockCache/index.h"
#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <string.h>
#include <time.h>

// Forward declarations for internal handlers
static void network_sync_hebbian_to_rings(network_t* network);

// --- FindBlock result payload destroy ---
// Frees the heap-allocated result payload and releases the hash buffer reference.

static void network_find_block_result_destroy(void* ptr) {
  if (ptr == NULL) return;
  network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)ptr;
  if (result->hash != NULL) {
    buffer_destroy(result->hash);
  }
  free(ptr);
}

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
  network->wanted_list = wanted_list_create();
  hebbian_table_init(&network->hebbian, 32);
  rate_limit_table_init(&network->rate_limits, 32);
  connection_manager_init(&network->conn_mgr, 16, NULL);
  network->hebbian_decay_timer_id = 0;
  network->metrics_push_timer_id = 0;

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
  wanted_list_destroy(network->wanted_list);
  hebbian_table_deinit(&network->hebbian);
  rate_limit_table_deinit(&network->rate_limits);
  connection_manager_deinit(&network->conn_mgr);
  if (network->hebbian_decay_timer_id != 0) {
    timer_actor_cancel(network->timer, network->hebbian_decay_timer_id);
  }
  if (network->metrics_push_timer_id != 0) {
    timer_actor_cancel(network->timer, network->metrics_push_timer_id);
  }
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
  free(response);
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
  free(response);
  (void)network;
}

static void network_handle_ping_block_response(network_t* network, message_t* msg) {
  wire_ping_block_response_t* response = (wire_ping_block_response_t*)msg->payload;
  if (response == NULL) return;

  // On successful block existence confirmation, strengthen Hebbian weight
  if (response->exists) {
    // Frequency rule: strengthen connection to the peer that confirmed
    // The peer's node_id comes from the message source, not the response itself.
    // When query tracking is wired up, this handler will receive the source peer
    // and apply hebbian_frequency to that peer. For now, the Hebbian update is
    // applied by the query tracking layer.
  }
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
  free(response);
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
  free(response);
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
      &network->conn_mgr,
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

      // Failed FindBlock: insert block_hash into requesting peer's EABF at level 0.
      // The requesting peer is the last node in the path (or original_source if path is empty).
      {
        const node_id_t* sender_id = &state.original_source;
        if (state.path_len > 0) {
          sender_id = &state.path[state.path_len - 1];
        }
        peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, sender_id);
        if (peer != NULL) {
          peer_eabf_subscribe(peer, state.block_hash, 32);
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

      // When forwarding FindBlock, insert block_hash into the requesting peer's EABF
      // at level 0. The requesting peer is the last node in the path.
      {
        const node_id_t* sender_id = &state.original_source;
        if (state.path_len > 1) {
          sender_id = &state.path[state.path_len - 2];
        }
        peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, sender_id);
        if (peer != NULL) {
          peer_eabf_subscribe(peer, state.block_hash, 32);
        }
      }
      // Add self to visited bloom
      find_block_add_visited(state.visited_bloom, &state.visited_count, network->authority->local_id.hash);
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
    }

    hebbian_apply_success(&network->hebbian, response->path, response->path_len,
                          latency_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER);
    network_sync_hebbian_to_rings(network);

    // Populate EABFs along the path in the global table
    for (size_t index = 0; index < network->eabf_table.count; index++) {
      eabf_t* eabf = network->eabf_table.entries[index].eabf;
      if (eabf != NULL) {
        eabf_subscribe(eabf, response->block_hash, 32);
      }
    }

    // Populate peer EABFs along the response path.
    // For each node at position i in the path, insert block hash into
    // the EABF of the next-hop peer.
    for (size_t index = 0; index + 1 < response->path_len; index++) {
      peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &response->path[index + 1]);
      if (peer != NULL) {
        peer_eabf_subscribe(peer, response->block_hash, 32);
      }
    }

    // Check wanted_list — notify any local requesters waiting for this block
    {
      buffer_t* hash_buf = buffer_create_from_pointer_copy(response->block_hash, 32);
      if (hash_buf != NULL) {
        wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, hash_buf);
        if (requesters != NULL) {
          wanted_requester_t* req = requesters;
          while (req != NULL) {
            network_find_block_result_payload_t* result =
                get_clear_memory(sizeof(network_find_block_result_payload_t));
            result->hash = REFERENCE(hash_buf, buffer_t);
            result->found = 1;
            message_t result_msg = {0};
            result_msg.type = NETWORK_FIND_BLOCK_RESULT;
            result_msg.payload = result;
            result_msg.payload_destroy = network_find_block_result_destroy;
            actor_send(req->actor, &result_msg);
            req = req->next;
          }
          wanted_requester_list_destroy(requesters);
        }
        buffer_destroy(hash_buf);
      }
    }
  } else {
    // Block not found — subscribe block_hash in EABFs as negative info
    // This is handled by TTL_EXPIRED in the forwarding path

    // Check wanted_list — notify any local requesters that the block was not found
    {
      buffer_t* hash_buf = buffer_create_from_pointer_copy(response->block_hash, 32);
      if (hash_buf != NULL) {
        wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, hash_buf);
        if (requesters != NULL) {
          wanted_requester_t* req = requesters;
          while (req != NULL) {
            network_find_block_result_payload_t* result =
                get_clear_memory(sizeof(network_find_block_result_payload_t));
            result->hash = REFERENCE(hash_buf, buffer_t);
            result->found = 0;
            message_t result_msg = {0};
            result_msg.type = NETWORK_FIND_BLOCK_RESULT;
            result_msg.payload = result;
            result_msg.payload_destroy = network_find_block_result_destroy;
            actor_send(req->actor, &result_msg);
            req = req->next;
          }
          wanted_requester_list_destroy(requesters);
        }
        buffer_destroy(hash_buf);
      }
    }
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
      &network->conn_mgr,
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

      // Check wanted_list — notify any local requesters waiting for this block
      {
        buffer_t* hash_buf = buffer_create_from_pointer_copy(store->block_hash, 32);
        if (hash_buf != NULL) {
          wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, hash_buf);
          if (requesters != NULL) {
            wanted_requester_t* req = requesters;
            while (req != NULL) {
              network_find_block_result_payload_t* result =
                  get_clear_memory(sizeof(network_find_block_result_payload_t));
              result->hash = REFERENCE(hash_buf, buffer_t);
              result->found = 1;
              message_t result_msg = {0};
              result_msg.type = NETWORK_FIND_BLOCK_RESULT;
              result_msg.payload = result;
              result_msg.payload_destroy = network_find_block_result_destroy;
              actor_send(req->actor, &result_msg);
              req = req->next;
            }
            wanted_requester_list_destroy(requesters);
          }
          buffer_destroy(hash_buf);
        }
      }
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
      find_block_add_visited(state.visited_bloom, &state.visited_count, network->authority->local_id.hash);
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
        if (state.carry_data && store->block_data != NULL && store->block_data_len > 0) {
          forward->block_data = get_clear_memory(store->block_data_len);
          if (forward->block_data != NULL) {
            memcpy(forward->block_data, store->block_data, store->block_data_len);
            forward->block_data_len = store->block_data_len;
          }
        }

        // TODO: Send forward to next_hops[hop] via QUIC when wired up
        wire_store_block_destroy(forward);
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

    // Populate EABFs along the response path in the global table
    for (size_t index = 0; index < network->eabf_table.count; index++) {
      eabf_t* eabf = network->eabf_table.entries[index].eabf;
      if (eabf != NULL) {
        // TODO: Subscribe block_hash, not holder.hash, once wire_store_block_response_t
        // carries block_hash. Currently using holder.hash as a routing hint.
        eabf_subscribe(eabf, response->holder.hash, 32);
      }
    }

    // Populate peer EABFs along the response path.
    for (size_t index = 0; index + 1 < response->path_len; index++) {
      peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &response->path[index + 1]);
      if (peer != NULL) {
        // TODO: Use block_hash, not holder.hash, once wire response carries it.
        peer_eabf_subscribe(peer, response->holder.hash, 32);
      }
    }

    // Recall on block acquisition: check all peers' EABFs at level 0 for this hash.
    // If a peer previously requested this block (failed FindBlock), push it to them.
    // TODO: Use block_hash instead of holder.hash once wire response carries block_hash.
    {
      size_t recall_count = 0;
      peer_connection_t** recall_peers = connection_manager_get_peers_for_topic(
          &network->conn_mgr, response->holder.hash, 32, &recall_count);
      if (recall_peers != NULL && recall_count > 0) {
        for (size_t index = 0; index < recall_count; index++) {
          peer_connection_t* peer = recall_peers[index];
          // Apply amplified Hebbian reinforcement for recall
          peer_hebbian_update(peer, network->conn_mgr.hebbian.recall_reward);
          // TODO: Send StoreBlock(reason=RECALL) to peer via QUIC when wired up
        }
        free(recall_peers);
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
  free(response);
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

#define MAX_RANK_HOPS 6

static void network_handle_rank_block(network_t* network, message_t* msg) {
  wire_rank_block_t* rank = (wire_rank_block_t*)msg->payload;
  if (rank == NULL) return;

  float local_capacity = atomic_load(&network->authority->capacity);

  // Check EABF: if we have this block_hash at some level, it's a positive signal
  for (size_t index = 0; index < network->eabf_table.count; index++) {
    eabf_t* eabf = network->eabf_table.entries[index].eabf;
    if (eabf != NULL) {
      eabf_subscribe(eabf, rank->block_hash, 32);
    }
  }

  // If we don't have the block and capacity < 50% → initiate FindBlock
  if (respiration_should_inhale(local_capacity)) {
    // Build a FindBlock message to search for this block
    wire_find_block_t* find = get_clear_memory(sizeof(wire_find_block_t));
    if (find != NULL) {
      find->message_id = (uint64_t)time(NULL) * 1000 + rank->count;
      memcpy(find->block_hash, rank->block_hash, 32);
      find->ttl = FIND_BLOCK_FORWARD_FANOUT;
      memset(find->visited_bloom, 0, WIRE_MAX_VISITED_BLOOM);
      find->visited_count = 0;
      find->path_len = 0;
      find->start_time = (uint64_t)time(NULL) * 1000;
      memcpy(&find->original_source, &network->authority->local_id, sizeof(node_id_t));

      message_t find_msg;
      find_msg.type = NETWORK_FIND_BLOCK;
      find_msg.payload = find;
      find_msg.payload_destroy = free;
      network_handle_find_block(network, &find_msg);
    }
  }

  // If hop_count < MAX_RANK_HOPS → forward to random subset of peers
  if (rank->hop_count < MAX_RANK_HOPS) {
    // Select up to 3 random peers from ring table for forwarding
    net_node_t* candidates[RING_K * RING_MAX_RINGS];
    size_t candidate_count = 0;

    for (size_t ring_index = 0; ring_index < network->rings->ring_count && candidate_count < 3; ring_index++) {
      ring_t* ring = &network->rings->rings[ring_index];
      for (int node_index = 0; node_index < ring->primary.length && candidate_count < 3; node_index++) {
        net_node_t* node = ring->primary.data[node_index];
        if (node == NULL) continue;
        if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;
        candidates[candidate_count] = node;
        candidate_count++;
      }
    }

    for (size_t hop = 0; hop < candidate_count; hop++) {
      wire_rank_block_t* forward = get_clear_memory(sizeof(wire_rank_block_t));
      if (forward == NULL) continue;
      memcpy(forward->block_hash, rank->block_hash, 32);
      forward->fib = rank->fib;
      forward->count = rank->count;
      memcpy(&forward->origin, &rank->origin, sizeof(node_id_t));
      forward->hop_count = rank->hop_count + 1;

      message_t forward_msg;
      forward_msg.type = NETWORK_RANK_BLOCK;
      forward_msg.payload = forward;
      forward_msg.payload_destroy = free;
      // Forward via QUIC when wired up
      free(forward);
      (void)forward_msg;
    }
  }
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
    free(response);
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
    free(response);
    (void)response_msg;
  }
}

static void network_handle_recall_accept(network_t* network, message_t* msg) {
  wire_recall_accept_t* accept = (wire_recall_accept_t*)msg->payload;
  if (accept == NULL) return;

  // Load block from sections and send StoreBlock with reason=RECALL
  // TODO: Look up block in local cache and send StoreBlock when wired up

  // After the block is stored (or once we know it's arriving via RECALL),
  // check wanted_list and notify any local requesters waiting for this block.
  // Note: The actual block storage is still TODO, but we check wanted_list here
  // so the integration is in place for when storage is implemented.
  {
    buffer_t* hash_buf = buffer_create_from_pointer_copy(accept->block_hash, 32);
    if (hash_buf != NULL) {
      wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, hash_buf);
      if (requesters != NULL) {
        wanted_requester_t* req = requesters;
        while (req != NULL) {
          network_find_block_result_payload_t* result =
              get_clear_memory(sizeof(network_find_block_result_payload_t));
          result->hash = REFERENCE(hash_buf, buffer_t);
          result->found = 1;
          message_t result_msg = {0};
          result_msg.type = NETWORK_FIND_BLOCK_RESULT;
          result_msg.payload = result;
          result_msg.payload_destroy = network_find_block_result_destroy;
          actor_send(req->actor, &result_msg);
          req = req->next;
        }
        wanted_requester_list_destroy(requesters);
      }
      buffer_destroy(hash_buf);
    }
  }
}

static void network_handle_recall_decline(network_t* network, message_t* msg) {
  wire_recall_decline_t* decline = (wire_recall_decline_t*)msg->payload;
  if (decline == NULL) return;

  // Remove block_hash from EABF_{self→source}
  // TODO: Remove from EABF when source node ID is available
  (void)network;
  (void)decline;
}

// --- RateLimited handler ---

static void network_handle_rate_limited(network_t* network, message_t* msg) {
  wire_rate_limited_t* limited = (wire_rate_limited_t*)msg->payload;
  if (limited == NULL) return;

  // A peer is telling us we've been rate limited for a specific RPC type.
  // The peer's node_id would come from the QUIC connection metadata.
  // For now, we can't look up the specific peer without a connection table,
  // so we log the backoff. When QUIC connections are tracked, this handler
  // will use the peer's node_id to call rate_limit_table_get and drain tokens.
  (void)network;

  // The rate limit information is available for future use:
  // limited->type — which RPC type was rate limited
  // limited->retry_after_ms — how long to wait before retrying
  // limited->current_limit — the peer's current rate limit for this type
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

// --- Local FindBlock handler ---
// Handles stream-originated FindBlock requests (NETWORK_LOCAL_FIND_BLOCK).
// Checks local cache first, then wanted_list for dedup, then routes via FindBlock.

static void network_handle_local_find_block(network_t* network, message_t* msg) {
  network_local_find_block_payload_t* payload =
      (network_local_find_block_payload_t*)msg->payload;
  if (payload == NULL || payload->hash == NULL) return;

  // Step 1: Check local block_cache first
  index_entry_t* entry = index_peek(network->block_cache->index, payload->hash);
  if (entry != NULL) {
    // Block found locally — send result immediately
    network_find_block_result_payload_t* result =
        get_clear_memory(sizeof(network_find_block_result_payload_t));
    result->hash = REFERENCE(payload->hash, buffer_t);
    result->found = 1;
    message_t result_msg = {0};
    result_msg.type = NETWORK_FIND_BLOCK_RESULT;
    result_msg.payload = result;
    result_msg.payload_destroy = network_find_block_result_destroy;
    actor_send(payload->reply_to, &result_msg);
    return;
  }

  // Step 2: Check wanted list for dedup — if entry exists, just add this requester
  wanted_entry_t* existing = wanted_list_find(network->wanted_list, payload->hash);
  if (existing != NULL) {
    wanted_requester_t* requester = get_clear_memory(sizeof(wanted_requester_t));
    requester->actor = payload->reply_to;
    requester->next = existing->requesters;
    existing->requesters = requester;
    return;
  }

  // Step 3: New request — add to wanted list
  wanted_list_add(network->wanted_list, payload->hash, payload->reply_to);

  // Step 4: Execute FindBlock routing
  find_block_state_t state;
  memset(&state, 0, sizeof(state));
  state.message_id = (uint64_t)time(NULL) * 1000;
  memcpy(state.block_hash, payload->hash->data, 32);
  state.ttl = FIND_BLOCK_FORWARD_FANOUT;
  state.start_time_ms = (uint64_t)time(NULL) * 1000;
  memcpy(&state.original_source, &network->authority->local_id, sizeof(node_id_t));

  net_node_t* next_hops[FIND_BLOCK_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  find_block_result_e result = find_block_execute(
      &network->eabf_table,
      &network->eabf_ttl,
      &network->conn_mgr,
      network->rings,
      &network->authority->local_id,
      &state,
      next_hops,
      &next_hop_count);

  switch (result) {
    case FIND_BLOCK_FOUND: {
      // Block is local (shouldn't happen since we checked cache, but handle it)
      wanted_requester_t* requesters = wanted_list_remove(network->wanted_list, payload->hash);
      if (requesters != NULL) {
        wanted_requester_t* req = requesters;
        while (req != NULL) {
          network_find_block_result_payload_t* fb_result =
              get_clear_memory(sizeof(network_find_block_result_payload_t));
          fb_result->hash = REFERENCE(payload->hash, buffer_t);
          fb_result->found = 1;
          message_t fb_msg = {0};
          fb_msg.type = NETWORK_FIND_BLOCK_RESULT;
          fb_msg.payload = fb_result;
          fb_msg.payload_destroy = network_find_block_result_destroy;
          actor_send(req->actor, &fb_msg);
          req = req->next;
        }
        wanted_requester_list_destroy(requesters);
      }
      break;
    }

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

      // Block not found — notify all requesters
      wanted_requester_t* requesters = wanted_list_clear_requesters(network->wanted_list, payload->hash);
      if (requesters != NULL) {
        wanted_requester_t* req = requesters;
        while (req != NULL) {
          network_find_block_result_payload_t* fb_result =
              get_clear_memory(sizeof(network_find_block_result_payload_t));
          fb_result->hash = REFERENCE(payload->hash, buffer_t);
          fb_result->found = 0;
          message_t fb_msg = {0};
          fb_msg.type = NETWORK_FIND_BLOCK_RESULT;
          fb_msg.payload = fb_result;
          fb_msg.payload_destroy = network_find_block_result_destroy;
          actor_send(req->actor, &fb_msg);
          req = req->next;
        }
        wanted_requester_list_destroy(requesters);
      }
      break;
    }

    case FIND_BLOCK_FORWARDING: {
      // Add self to path
      if (state.path_len < FIND_BLOCK_MAX_PATH) {
        memcpy(&state.path[state.path_len], &network->authority->local_id, sizeof(node_id_t));
        state.path_len++;
      }
      // Add self to visited bloom
      find_block_add_visited(state.visited_bloom, &state.visited_count, network->authority->local_id.hash);
      // Decrement TTL
      state.ttl--;

      // Forward to each selected next-hop
      for (size_t hop = 0; hop < next_hop_count; hop++) {
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
      network_handle_rate_limited(network, msg);
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
    case NETWORK_QUIC_DATA: {
      quic_data_payload_t* quic_data = (quic_data_payload_t*)msg->payload;
      if (quic_data == NULL || quic_data->data == NULL || quic_data->length == 0) break;
      // Decode CBOR wire message and re-dispatch
      cbor_item_t* wire_msg = cbor_load(quic_data->data, quic_data->length, NULL);
      if (wire_msg != NULL && cbor_isa_array(wire_msg) && cbor_array_size(wire_msg) >= 1) {
        uint8_t type = wire_get_type(wire_msg);
        message_t dispatch_msg;
        memset(&dispatch_msg, 0, sizeof(dispatch_msg));
        dispatch_msg.type = type;
        dispatch_msg.payload_destroy = free;
        switch (type) {
          case WIRE_PING: {
            wire_ping_t* payload = get_clear_memory(sizeof(wire_ping_t));
            if (wire_ping_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_ping(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_PING_CAPACITY: {
            wire_ping_capacity_t* payload = get_clear_memory(sizeof(wire_ping_capacity_t));
            if (wire_ping_capacity_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_ping_capacity(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_PING_BLOCK: {
            wire_ping_block_t* payload = get_clear_memory(sizeof(wire_ping_block_t));
            if (wire_ping_block_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_ping_block(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_FIND_BLOCK: {
            wire_find_block_t* payload = get_clear_memory(sizeof(wire_find_block_t));
            if (wire_find_block_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_find_block(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_FIND_NODE: {
            wire_find_node_t* payload = get_clear_memory(sizeof(wire_find_node_t));
            if (wire_find_node_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_find_node(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_STORE_BLOCK: {
            wire_store_block_t* payload = get_clear_memory(sizeof(wire_store_block_t));
            if (wire_store_block_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              dispatch_msg.payload_destroy = (void (*)(void*))wire_store_block_destroy;
              network_handle_store_block(network, &dispatch_msg);
            } else {
              wire_store_block_destroy(payload);
            }
            break;
          }
          case WIRE_SEEKING_BLOCKS: {
            wire_seeking_blocks_t* payload = get_clear_memory(sizeof(wire_seeking_blocks_t));
            if (wire_seeking_blocks_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              dispatch_msg.payload_destroy = (void (*)(void*))wire_seeking_blocks_destroy;
              network_handle_seeking_blocks(network, &dispatch_msg);
            } else {
              wire_seeking_blocks_destroy(payload);
            }
            break;
          }
          case WIRE_RANK_BLOCK: {
            wire_rank_block_t* payload = get_clear_memory(sizeof(wire_rank_block_t));
            if (wire_rank_block_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_rank_block(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          case WIRE_RECALL_BLOCK: {
            wire_recall_block_t* payload = get_clear_memory(sizeof(wire_recall_block_t));
            if (wire_recall_block_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_recall_block(network, &dispatch_msg);
            } else {
              free(payload);
            }
            break;
          }
          default:
            // Response types are handled by their respective handlers when
            // dispatched by the QUIC connection layer with the correct message_type
            break;
        }
        cbor_decref(&wire_msg);
      }
      break;
    }
    case NETWORK_QUIC_CONNECTED: {
      // New QUIC connection — add peer to connection manager
      quic_connected_payload_t* quic_conn = (quic_connected_payload_t*)msg->payload;
      if (quic_conn != NULL) {
        // Node_id extraction from TLS certificate is deferred to Task 7.
        // Using zeroed node_id means only one QUIC-connected peer can exist
        // at a time — all subsequent connects will match the first via
        // connection_manager_lookup.
        node_id_t peer_id;
        node_id_clear(&peer_id);
        peer_connection_t* peer = connection_manager_add(
            &network->conn_mgr, &peer_id, &quic_conn->peer_addr, network->pool);
#ifdef HAS_MSQUIC
        if (peer != NULL) {
          peer->quic_connection = quic_conn->connection;
        }
#endif
      }
      break;
    }
    case NETWORK_QUIC_DISCONNECTED: {
      // QUIC connection closed — mark peer disconnected in connection manager
      // Node_id extraction from QUIC connection handle is deferred to Task 7.
      // Without the node_id we can't remove the peer from the connection manager.
      // Peers remain in the table as disconnected until Hebbian decay removes them.
      break;
    }
    case CM_PEER_CONNECTED: {
      // Peer connection created by connection_manager_add — handled internally
      break;
    }
    case CM_PEER_DISCONNECTED: {
      // Peer connection removed — handled internally
      break;
    }
    case PEER_EABF_TICK: {
      // Advance EABF timing wheels on all peer connections
      for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
        if (network->conn_mgr.peers[index]->connected) {
          peer_eabf_tick(network->conn_mgr.peers[index]);
        }
      }
      break;
    }
    case NETWORK_LOCAL_FIND_BLOCK: {
      // Stream requests block from network — check cache, dedup via wanted_list, route
      network_handle_local_find_block(network, msg);
      break;
    }
    case NETWORK_FIND_BLOCK_RESULT: {
      // Network returns FindBlock result to stream
      // Handled by stream actor — dispatch routes to reply_to
      break;
    }
    case NETWORK_LOCAL_STORE_BLOCK: {
      // Stream announces new block to network
      network_local_store_block_payload_t* payload =
          (network_local_store_block_payload_t*)msg->payload;
      if (payload != NULL && payload->hash != NULL) {
        // Full implementation in later tasks
      }
      break;
    }
    case NETWORK_STORE_BLOCK_RESULT: {
      // Network returns StoreBlock result to stream
      // Handled by stream actor — dispatch routes to reply_to
      break;
    }
    default:
      break;
  }
  if (msg->payload != NULL && msg->payload_destroy != NULL) {
    msg->payload_destroy(msg->payload);
  }
}