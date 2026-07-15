//
// Created by victor on 5/14/25.
//

#include "network.h"
#include "peer_info.h"
#include "connection_manager.h"
#include "wire.h"
#include "quic_listener.h"
#include "find_block.h"
#include "store_block.h"
#include "respiration.h"
#include "respiration_actor.h"
#include "peer_connection.h"
#include "timing_wheel.h"
#include "topology_metrics.h"
#include "topology_report.h"
#include "ring_set.h"
#include "wanted_list.h"
#include "relay_client.h"
#include "nat_detect.h"
#include "conn_state.h"
#include "closest_nodes.h"
#include "measure_nodes.h"
#include "msquic_singleton.h"
#include "../Timer/timer_actor.h"
#include "../Bloom/elastic_bloom_filter.h"
#include "../BlockCache/block_cache.h"
#include "../BlockCache/index.h"
#include "../Buffer/buffer.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cbor.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOPOLOGY_METRICS_PUSH_INTERVAL_MS 300000  // 5 minutes
#define PING_CAPACITY_INTERVAL_MS 900000  // 15 minutes
#define FRIEND_RECONNECT_INTERVAL_MS 5000

// Forward declarations for internal handlers
static void network_sync_hebbian_to_rings(network_t* network);
static void network_handle_local_find_block(network_t* network, message_t* msg);
static void network_handle_ping_capacity_tick(network_t* network, message_t* msg);
static void network_handle_closest_nodes(network_t* network, message_t* msg);
static void network_handle_closest_nodes_response(network_t* network, message_t* msg);
static void network_handle_measure_nodes(network_t* network, message_t* msg);
static void network_handle_measure_nodes_response(network_t* network, message_t* msg);
static void network_handle_closest_nodes_progress(network_t* network, message_t* msg);
static void network_handle_local_closest_nodes(network_t* network, message_t* msg);

// --- Local FindBlock payload destroy ---
// Frees the heap-allocated payload and releases the hash buffer reference.

void network_local_find_block_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  network_local_find_block_payload_t* payload = (network_local_find_block_payload_t*)ptr;
  if (payload->hash != NULL) {
    buffer_destroy(payload->hash);
  }
  free(payload);
}

void network_local_store_block_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  network_local_store_block_payload_t* payload = (network_local_store_block_payload_t*)ptr;
  if (payload->hash != NULL) {
    buffer_destroy(payload->hash);
  }
  free(payload);
}

// --- FindBlock result payload destroy ---
// Frees the heap-allocated result payload, releases the hash buffer reference,
// and destroys any attached block (remote-receipt path). When the result came
// from a local cache hit, block is NULL and only the hash is released.

void network_find_block_result_destroy(void* ptr) {
  if (ptr == NULL) return;
  network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)ptr;
  if (result->hash != NULL) {
    buffer_destroy(result->hash);
  }
  if (result->block != NULL) {
    block_destroy(result->block);
  }
  free(ptr);
}

// --- ClosestNodes local payload destroy ---
// Frees the heap-allocated payload for local closest-nodes requests.

void network_local_closest_nodes_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  free(ptr);
}

// --- FindNode local payload destroy ---
// Frees the heap-allocated payload for local find-node requests.

void network_local_find_node_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  free(ptr);
}

// --- ClosestNodes result payload destroy ---
// Frees the heap-allocated result payload for closest-nodes results.

void network_closest_nodes_result_payload_destroy(void* ptr) {
  if (ptr == NULL) return;
  free(ptr);
}

network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool,
                          const config_t* config) {
  network_t* network = get_clear_memory(sizeof(network_t));
  network->authority = authority;
  authority_init_local_id(authority);
  network->block_cache = block_cache;
  network->timer = timer;
  network->pool = pool;
  network->running = ATOMIC_VAR_INIT(0);
  network->rings = ring_set_create(0, 0, 0);
  network->latency_cache = latency_cache_create(0);
  eabf_table_init(&network->eabf_table, 16,
                  config->eabf_base_ttl_ms, config->eabf_maintenance_ms);
  eabf_ttl_table_init(&network->eabf_ttl, 64);
  network->wanted_list = wanted_list_create();
  hebbian_table_init(&network->hebbian, 32, config->hebbian_decay_factor);
  rate_limit_table_init(&network->rate_limits, 32);
  network->log = NULL;
  network->topology_metrics = topology_metrics_create(pool);
  connection_manager_init(&network->conn_mgr, 16, NULL);
  network->hebbian_decay_timer_id = 0;
  network->metrics_push_timer_id = 0;
  network->ping_capacity_timer_id = 0;
  network->friend_reconnect_timer_id = 0;
  network->relay = NULL;
  network->nat_detect = NULL;
  network->local_nat_type = NAT_TYPE_UNKNOWN;

  // Store config values for downstream consumers
  network->gossip_init_interval_s = config->gossip_init_interval_s;
  network->gossip_init_count = config->gossip_init_count;
  network->gossip_steady_interval_s = config->gossip_steady_interval_s;
  network->gossip_timeout_ms = config->gossip_timeout_ms;
  network->hebbian_decay_factor = config->hebbian_decay_factor;
  network->eabf_base_ttl_ms = config->eabf_base_ttl_ms;
  network->eabf_maintenance_ms = config->eabf_maintenance_ms;
  network->respiration_tau_min_ms = config->respiration_tau_min_ms;
  network->respiration_tau_max_ms = config->respiration_tau_max_ms;
  network->relay_max_retries = config->relay_max_retries;
  network->relay_retry_delay_ms = config->relay_retry_delay_ms;

#ifdef HAS_MSQUIC
  network->msquic = offs_msquic_open();
#endif

  gossip_handle_init(&network->gossip,
                     network->gossip_init_interval_s,
                     network->gossip_init_count,
                     network->gossip_steady_interval_s,
                     network->gossip_timeout_ms);

  actor_init(&network->actor, network, network_dispatch, pool);

  network->respiration = respiration_actor_create(network, pool);
  block_cache->respiration = network->respiration;

  // Start gossip timer: first tick in init_interval_s, then recurring
  network->gossip_timer_id = 0;
  timer_actor_set(timer,
      (uint64_t)network->gossip_init_interval_s * 1000,
      (uint64_t)network->gossip_init_interval_s * 1000,
      &network->actor,
      NETWORK_GOSSIP_TICK,
      &network->gossip_timer_id);

  // Start EABF maintenance sweep
  network->eabf_maintenance_timer_id = 0;
  timer_actor_set(timer,
      network->eabf_maintenance_ms,
      network->eabf_maintenance_ms,
      &network->actor,
      NETWORK_EABF_EXPIRE,
      &network->eabf_maintenance_timer_id);

  // Start metrics push timer: recurring 5-minute collection
  network->metrics_push_timer_id = 0;
  timer_actor_set(timer,
      TOPOLOGY_METRICS_PUSH_INTERVAL_MS,
      TOPOLOGY_METRICS_PUSH_INTERVAL_MS,
      &network->actor,
      NETWORK_METRICS_PUSH,
      &network->metrics_push_timer_id);

  // PingCapacity timer: periodic capacity exchange with all peers
  network->ping_capacity_timer_id = 0;
  timer_actor_set(timer,
      PING_CAPACITY_INTERVAL_MS,
      PING_CAPACITY_INTERVAL_MS,
      &network->actor,
      NETWORK_PING_CAPACITY_TICK,
      &network->ping_capacity_timer_id);

  // Friend reconnect timer: attempt reconnection periodically
  network->friend_reconnect_timer_id = 0;
  timer_actor_set(timer,
      FRIEND_RECONNECT_INTERVAL_MS,
      FRIEND_RECONNECT_INTERVAL_MS,
      &network->actor,
      NETWORK_FRIEND_RECONNECT_TICK,
      &network->friend_reconnect_timer_id);

  return network;
}

void network_shutdown_connections(network_t* network) {
  if (network == NULL) return;

  /* Route the shutdown loop through the network actor so ConnectionShutdown,
     connection_manager_remove, and ConnectionClose all run on the same thread.
     The old code iterated conn_mgr.peers on the main thread while the network
     actor's worker ran connection_manager_remove (freed peers + memmove) on
     SHUTDOWN_COMPLETE -> UAF on the peers array and a race on the HQUIC
     lifecycle. See concurrency-pass.md F3. */
#ifdef HAS_MSQUIC
  if (network->msquic != NULL) {
    network_shutdown_payload_t* payload = get_clear_memory(sizeof(network_shutdown_payload_t));
    if (payload != NULL) {
      payload->lock = platform_mutex_create();
      payload->done = platform_condvar_create();
      atomic_store(&payload->done_flag, false);
      if (payload->lock != NULL && payload->done != NULL) {
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = NETWORK_SHUTDOWN_CONNECTIONS;
        msg.payload = payload;
        /* payload_destroy = NULL: the main thread frees the payload after the
           wait; the network actor only signals the condvar. */
        msg.payload_destroy = NULL;
        actor_send(&network->actor, &msg);

        /* Wait for the network actor to post done_flag. The network actor's
           worker is live during offs_node_stop phase 5 (joins in phase 7), so
           it can process the message and the subsequent NETWORK_QUIC_DISCONNECT
           callbacks from SHUTDOWN_COMPLETE. */
        platform_mutex_lock(payload->lock);
        while (!atomic_load(&payload->done_flag)) {
          platform_condvar_wait(payload->done, payload->lock);
        }
        platform_mutex_unlock(payload->lock);

        platform_condvar_destroy(payload->done);
        platform_mutex_destroy(payload->lock);
        free(payload);
      } else {
        if (payload->lock != NULL) platform_mutex_destroy(payload->lock);
        if (payload->done != NULL) platform_condvar_destroy(payload->done);
        free(payload);
      }
    }
  }
#endif

  if (network->relay != NULL) {
    relay_client_disconnect(network->relay);
  }
}

void network_destroy(network_t* network) {
  if (network == NULL) return;
  if (atomic_load(&network->gossip_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->gossip_timer_id));
  }
  if (atomic_load(&network->eabf_maintenance_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->eabf_maintenance_timer_id));
  }
  gossip_handle_deinit(&network->gossip);
  ring_set_destroy(network->rings);
  latency_cache_destroy(network->latency_cache);
  eabf_table_deinit(&network->eabf_table);
  eabf_ttl_table_deinit(&network->eabf_ttl);
  wanted_list_destroy(network->wanted_list);
  hebbian_table_deinit(&network->hebbian);
  rate_limit_table_deinit(&network->rate_limits);
  topology_metrics_destroy(network->topology_metrics);
  connection_manager_deinit(&network->conn_mgr);
  if (network->hebbian_decay_timer_id != 0) {
    timer_actor_cancel(network->timer, network->hebbian_decay_timer_id);
  }
  if (atomic_load(&network->metrics_push_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->metrics_push_timer_id));
  }
  if (atomic_load(&network->ping_capacity_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->ping_capacity_timer_id));
    network->ping_capacity_timer_id = 0;
  }
  if (atomic_load(&network->friend_reconnect_timer_id) != 0) {
    timer_actor_cancel(network->timer, atomic_load(&network->friend_reconnect_timer_id));
    atomic_store(&network->friend_reconnect_timer_id, 0);
  }
  if (network->relay != NULL) {
    relay_client_destroy(network->relay);
    network->relay = NULL;
  }
  if (network->nat_detect != NULL) {
    nat_detect_destroy(network->nat_detect);
    network->nat_detect = NULL;
  }
  respiration_actor_destroy(network->respiration);
  network->respiration = NULL;

  // Free any remaining pending QUIC connections (never saluted)
  pending_quic_t* pending = network->pending_connections;
  while (pending != NULL) {
    pending_quic_t* next = pending->next;
    free(pending);
    pending = next;
  }
  network->pending_connections = NULL;
#ifdef HAS_MSQUIC
  if (network->msquic != NULL) {
    offs_msquic_close();
  }
#endif
  actor_destroy(&network->actor);
  if (network->log != NULL) {
    message_log_clear(network->log);
    free(network->log);
    network->log = NULL;
  }
  free(network);
}

int network_connect_relay(network_t* network, const char* host, uint16_t port) {
  if (network == NULL || host == NULL) return -1;

  // Create NAT detection module
  network->nat_detect = nat_detect_create(network, network->pool);
  if (network->nat_detect == NULL) {
    log_error("network_connect_relay: failed to create NAT detection");
    return -1;
  }

  // Create relay client
  network->relay = relay_client_create(network, network->pool,
      network->relay_max_retries, network->relay_retry_delay_ms);
  if (network->relay == NULL) {
    log_error("network_connect_relay: failed to create relay client");
    nat_detect_destroy(network->nat_detect);
    network->nat_detect = NULL;
    return -1;
  }

  // Propagate cert paths from authority to relay client
  if (network->authority && network->authority->node_cert_path && network->authority->node_key_path) {
    network->relay->cert_path = strdup(network->authority->node_cert_path);
    network->relay->key_path = strdup(network->authority->node_key_path);
  }

  // Connect to relay server — share the quic_listener's registration to avoid
  // UDP socket conflicts when both a listener and relay client are active in
  // the same process (MsQuic routes connections through the registration's socket)
#ifdef HAS_MSQUIC
  HQUIC shared_reg = NULL;
  if (network->quic_listener != NULL) {
    shared_reg = network->quic_listener->registration;
  }
  log_info("network_connect_relay: shared_registration=%p, quic_listener=%p",
           (void*)shared_reg, (void*)network->quic_listener);
#else
  void* shared_reg = NULL;
#endif

  if (relay_client_connect(network->relay, host, port, shared_reg) != 0) {
    log_error("network_connect_relay: failed to connect to relay");
    relay_client_destroy(network->relay);
    network->relay = NULL;
    // nat_detect stays — it will be used when relay_b is configured
    return -1;
  }

  return 0;
}

int network_connect_peer(network_t* network, const char* host, uint16_t port) {
  if (network == NULL || host == NULL) return -1;

#ifdef HAS_MSQUIC
  quic_listener_t* listener = network->quic_listener;
  if (listener == NULL) {
    log_error("network_connect_peer: no QUIC listener available");
    return -1;
  }
  return quic_listener_connect(listener, host, port);
#else
  (void)port;
  log_error("network_connect_peer: QUIC not available (HAS_MSQUIC not defined)");
  return -1;
#endif
}

// --- Pending QUIC connection helpers ---

static pending_quic_t* pending_quic_find(network_t* network, void* quic_connection);

static void pending_quic_add(network_t* network, void* quic_connection,
                             void* quic_stream,
                             const struct sockaddr_storage* peer_addr) {
  // Avoid duplicates
  if (pending_quic_find(network, quic_connection) != NULL) return;
  pending_quic_t* entry = get_clear_memory(sizeof(pending_quic_t));
  if (entry == NULL) return;
  entry->quic_connection = quic_connection;
  entry->quic_stream = quic_stream;
  if (peer_addr != NULL) {
    memcpy(&entry->peer_addr, peer_addr, sizeof(struct sockaddr_storage));
  }
  entry->next = network->pending_connections;
  network->pending_connections = entry;
}

static pending_quic_t* pending_quic_find(network_t* network, void* quic_connection) {
  pending_quic_t* current = network->pending_connections;
  while (current != NULL) {
    if (current->quic_connection == quic_connection) return current;
    current = current->next;
  }
  return NULL;
}

static pending_quic_t* pending_quic_remove(network_t* network, void* quic_connection) {
  pending_quic_t** indirect = &network->pending_connections;
  while (*indirect != NULL) {
    if ((*indirect)->quic_connection == quic_connection) {
      pending_quic_t* found = *indirect;
      *indirect = found->next;
      return found;
    }
    indirect = &(*indirect)->next;
  }
  return NULL;
}

// --- Salutation handler ---

static void network_handle_salutation(network_t* network, message_t* msg,
                                       void* quic_connection) {
  wire_salutation_t* salut = (wire_salutation_t*)msg->payload;
  if (salut == NULL) return;

  // Verify: BLAKE3(public_key) must match sender_id.hash
  if (salut->public_key == NULL || salut->public_key_len == 0) {
    log_error("salutation: missing public_key, cannot verify identity");
    wire_salutation_destroy(salut);
    return;
  }

  node_id_t computed_id;
  if (node_id_from_public_key(salut->public_key, salut->public_key_len,
                              &computed_id) != 0) {
    log_error("salutation: failed to derive node_id from public_key");
    wire_salutation_destroy(salut);
    return;
  }
  if (!node_id_equals(&computed_id, &salut->sender_id)) {
    log_error("salutation: public_key hash does not match sender_id");
    wire_salutation_destroy(salut);
    return;
  }

  // Find the pending QUIC connection
  pending_quic_t* pending = pending_quic_remove(network, quic_connection);
  if (pending == NULL) {
    log_error("salutation: no pending connection for quic handle");
    wire_salutation_destroy(salut);
    return;
  }

  // Add peer to connection manager with verified identity
  peer_connection_t* peer = connection_manager_add(
      &network->conn_mgr, &salut->sender_id, &pending->peer_addr, network->pool);

  if (peer != NULL) {
#ifdef HAS_MSQUIC
    peer->quic_connection = quic_connection;
    peer->quic_stream = pending->quic_stream;
#endif
    conn_state_on_direct_connected(peer);

    // Insert the authenticated peer into the ring table so find_block_execute
    // can route FindBlock requests to it. Without this, the ring table only
    // gets populated via gossip exchanges which may not have run yet.
    net_node_t* node = net_node_create(&salut->sender_id, 0, 0);
    if (node != NULL) {
      node->weight = FIND_BLOCK_MIN_WEIGHT;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
      ring_set_insert(network->rings, node, 0);
    }

    // Send immediate PingCapacity to newly connected peer
    {
      wire_ping_capacity_t ping_cap;
      memset(&ping_cap, 0, sizeof(ping_cap));
      ping_cap.message_id = gossip_handle_next_query_id(&network->gossip);
      memcpy(&ping_cap.source, &network->authority->local_id, sizeof(node_id_t));
      ping_cap.capacity = atomic_load(&network->authority->capacity);
      ping_cap.phase = atomic_load(&network->authority->phase);
      cbor_item_t* ping_cap_cbor = wire_ping_capacity_encode(&ping_cap);
      conn_state_send(network, peer, ping_cap_cbor);
      cbor_decref(&ping_cap_cbor);
    }
  }

  free(pending);

  wire_salutation_destroy(salut);
}

// --- Ping handler ---

static void network_handle_ping(network_t* network, message_t* msg) {
  wire_ping_t* ping = (wire_ping_t*)msg->payload;
  if (ping == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING, MSG_DIRECTION_RECEIVED,
                       &ping->sender_id, ping->message_id, NULL,
                       0, &network->hebbian);
  }

  // Build wire PingResponse with our capacity/phase and echo timestamp for RTT
  wire_ping_response_t response;
  memset(&response, 0, sizeof(response));
  response.message_id = ping->message_id;
  response.echo_time = ping->timestamp;
  response.capacity = atomic_load(&network->authority->capacity);
  response.phase = atomic_load(&network->authority->phase);
  memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &ping->sender_id);
  if (peer != NULL) {
    cbor_item_t* cbor = wire_ping_response_encode(&response);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}

static void network_handle_ping_response(network_t* network, message_t* msg) {
  wire_ping_response_t* response = (wire_ping_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  // Calculate RTT from echo_time (the timestamp field holds the original send time)
  uint64_t now_ms = (uint64_t)time(NULL) * 1000;
  uint64_t rtt_ms = 0;
  if (now_ms > response->echo_time) {
    rtt_ms = now_ms - response->echo_time;
  }
  // Update latency cache with the measured RTT
  latency_cache_insert(network->latency_cache, &response->sender_id, 0, 0, (float)rtt_ms);
  // Update the peer's RTT in the connection manager
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &response->sender_id);
  if (peer != NULL) {
    peer_update_rtt(peer, (double)rtt_ms);
  }

  // Hebbian frequency update: responsive peers get weight increase
  float delta_w = hebbian_compute_delta(rtt_ms, 1.0f);
  hebbian_frequency(&network->hebbian, &response->sender_id, delta_w);
}

// --- PingBlock handler ---
// Pending PingBlock request — tracks async block_cache lookups
typedef struct pending_ping_block_t {
  uint64_t message_id;
  node_id_t sender_id;
  struct pending_ping_block_t* next;
} pending_ping_block_t;

static void network_handle_ping_block(network_t* network, message_t* msg) {
  wire_ping_block_t* ping = (wire_ping_block_t*)msg->payload;
  if (ping == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING_BLOCK, MSG_DIRECTION_RECEIVED,
                       &ping->sender_id, ping->message_id, ping->block_hash,
                       0, &network->hebbian);
  }

  // Check if we have the block locally via synchronous index_peek
  buffer_t* hash_buf = buffer_create_from_pointer_copy(ping->block_hash, 32);
  if (hash_buf == NULL) return;

  index_entry_t* entry = index_peek(network->block_cache->index, hash_buf);
  if (entry != NULL) {
    // Block exists locally — respond immediately
    wire_ping_block_response_t response;
    memset(&response, 0, sizeof(response));
    response.message_id = ping->message_id;
    response.exists = 1;
    response.fib = entry->counter.fib;
    response.healthy = 1;
    memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &ping->sender_id);
    if (peer != NULL) {
      cbor_item_t* cbor = wire_ping_block_response_encode(&response);
      conn_state_send(network, peer, cbor);
      cbor_decref(&cbor);
    }
    buffer_destroy(hash_buf);
  } else {
    // Block not in index — respond with exists=0
    wire_ping_block_response_t response;
    memset(&response, 0, sizeof(response));
    response.message_id = ping->message_id;
    response.exists = 0;
    response.fib = 0;
    response.healthy = 0;
    memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &ping->sender_id);
    if (peer != NULL) {
      cbor_item_t* cbor = wire_ping_block_response_encode(&response);
      conn_state_send(network, peer, cbor);
      cbor_decref(&cbor);
    }
    buffer_destroy(hash_buf);
  }
}

static void network_handle_ping_block_response(network_t* network, message_t* msg) {
  wire_ping_block_response_t* response = (wire_ping_block_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING_BLOCK_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  // On successful block existence confirmation, strengthen Hebbian weight
  if (response->exists) {
    float delta_w = hebbian_compute_delta(network->gossip_timeout_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER);
    hebbian_frequency(&network->hebbian, &response->sender_id, delta_w);
    network_sync_hebbian_to_rings(network);
  }
}

// --- PingCapacity periodic tick ---
// Sends PingCapacity to all connected peers every 15 minutes.
// Separate from gossip because PingCapacity is about capacity awareness,
// not ring maintenance/peer discovery.

static void network_handle_ping_capacity_tick(network_t* network, message_t* msg) {
  (void)msg;
  if (network->conn_mgr.peer_count == 0) return;

  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* peer = network->conn_mgr.peers[index];
    if (peer == NULL || !peer->connected) continue;

    wire_ping_capacity_t ping_cap;
    memset(&ping_cap, 0, sizeof(ping_cap));
    ping_cap.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&ping_cap.source, &network->authority->local_id, sizeof(node_id_t));
    ping_cap.capacity = atomic_load(&network->authority->capacity);
    ping_cap.phase = atomic_load(&network->authority->phase);

    cbor_item_t* cbor = wire_ping_capacity_encode(&ping_cap);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}

// --- Add node to ring table (helper for gossip handlers) ---

static void network_add_node_to_ring(network_t* network,
                                      const node_id_t* node_id,
                                      uint32_t addr,
                                      uint16_t port) {
  if (network == NULL || node_id == NULL) return;

  // Check if already in ring table — skip duplicate insertion
  net_node_t* existing = ring_set_find_by_id(network->rings, node_id);
  if (existing != NULL) {
    // Update last_gossip_time and record success
    existing->last_gossip_time = (uint64_t)time(NULL) * 1000;
    net_node_record_success(existing);
    return;
  }

  // Check probe cache — use cached latency if available
  float cached_latency_ms;
  if (latency_cache_get(network->latency_cache, node_id, &cached_latency_ms) == 0) {
    uint32_t latency_us = (uint32_t)(cached_latency_ms * 1000);
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      node->weight = FIND_BLOCK_MIN_WEIGHT;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
      ring_set_insert(network->rings, node, latency_us);
    }
    return;
  }

  // Not in cache — only probe if already connected
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, node_id);
  if (peer != NULL && peer->connected) {
    // Send Ping to measure latency — ring insertion deferred until PingResponse
    wire_ping_t ping;
    memset(&ping, 0, sizeof(ping));
    ping.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&ping.sender_id, &network->authority->local_id, sizeof(node_id_t));
    ping.timestamp = (uint64_t)time(NULL) * 1000;

    cbor_item_t* cbor = wire_ping_encode(&ping);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  } else {
    // Not connected — insert at latency=0 (outermost ring)
    net_node_t* node = net_node_create(node_id, addr, port);
    if (node != NULL) {
      node->weight = FIND_BLOCK_MIN_WEIGHT;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
      ring_set_insert(network->rings, node, 0);
    }
  }
}

// --- Gossip tick handler (Meridian algorithm) ---

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

  // Select 1 random node per ring as gossip targets (Meridian algorithm)
  net_node_t targets[RING_MAX_RINGS];
  size_t target_count = ring_set_get_random_nodes(
      network->rings, targets, RING_MAX_RINGS,
      &network->authority->local_id);

  for (size_t index = 0; index < target_count; index++) {
    peer_connection_t* peer = connection_manager_lookup(
        &network->conn_mgr, &targets[index].id);
    if (peer == NULL || !peer->connected) continue;

    // Build gossip message with ring membership (excluding target)
    wire_gossip_t gossip;
    memset(&gossip, 0, sizeof(gossip));
    gossip.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&gossip.sender_id, &network->authority->local_id, sizeof(node_id_t));
    gossip.rendezvous_addr = 0;  // 0 until NAT detection fills this
    gossip.rendezvous_port = 0;

    // Fill targets: 1 random node per ring, excluding the target itself
    net_node_t ring_targets[RING_MAX_RINGS];
    gossip.target_count = (uint8_t)ring_set_get_random_nodes(
        network->rings, ring_targets, RING_MAX_RINGS, &targets[index].id);
    for (uint8_t target_index = 0;
         target_index < gossip.target_count && target_index < RING_MAX_RINGS;
         target_index++) {
      memcpy(&gossip.targets[target_index], &ring_targets[target_index],
             sizeof(node_id_t));
    }

    cbor_item_t* cbor = wire_gossip_encode(&gossip);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);

    if (network->log != NULL) {
      message_log_record(network->log, WIRE_GOSSIP, MSG_DIRECTION_SENT,
                         &targets[index].id, gossip.message_id, NULL,
                         0, &network->hebbian);
    }
  }
}

// --- Gossip receipt handler (PUSHPULL: receive gossip, send pull response) ---

static void network_handle_gossip_received(network_t* network, message_t* msg) {
  wire_gossip_t* gossip = (wire_gossip_t*)msg->payload;
  if (gossip == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_GOSSIP, MSG_DIRECTION_RECEIVED,
                       &gossip->sender_id, gossip->message_id, NULL,
                       0, &network->hebbian);
  }

  // Add sender to ring table
  network_add_node_to_ring(network, &gossip->sender_id,
                            gossip->rendezvous_addr, gossip->rendezvous_port);

  // Add all targets from the gossip packet
  for (uint8_t index = 0; index < gossip->target_count && index < RING_MAX_RINGS; index++) {
    network_add_node_to_ring(network, &gossip->targets[index], 0, 0);
  }

  // PUSHPULL: send our ring membership back to the sender
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &gossip->sender_id);
  if (peer != NULL && peer->connected) {
    wire_gossip_pull_t pull;
    memset(&pull, 0, sizeof(pull));
    pull.message_id = gossip_handle_next_query_id(&network->gossip);
    memcpy(&pull.sender_id, &network->authority->local_id, sizeof(node_id_t));
    pull.rendezvous_addr = 0;
    pull.rendezvous_port = 0;

    net_node_t ring_targets[RING_MAX_RINGS];
    pull.target_count = (uint8_t)ring_set_get_random_nodes(
        network->rings, ring_targets, RING_MAX_RINGS, &gossip->sender_id);
    for (uint8_t target_index = 0;
         target_index < pull.target_count && target_index < RING_MAX_RINGS;
         target_index++) {
      memcpy(&pull.targets[target_index], &ring_targets[target_index],
             sizeof(node_id_t));
    }

    cbor_item_t* cbor = wire_gossip_pull_encode(&pull);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}

// --- Gossip pull receipt handler (PUSHPULL: pull half, no response) ---

static void network_handle_gossip_pull_received(network_t* network, message_t* msg) {
  wire_gossip_pull_t* pull = (wire_gossip_pull_t*)msg->payload;
  if (pull == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_GOSSIP_PULL, MSG_DIRECTION_RECEIVED,
                       &pull->sender_id, pull->message_id, NULL,
                       0, &network->hebbian);
  }

  // Add sender to ring table
  network_add_node_to_ring(network, &pull->sender_id,
                            pull->rendezvous_addr, pull->rendezvous_port);

  // Add all targets from the gossip pull packet
  for (uint8_t index = 0; index < pull->target_count && index < RING_MAX_RINGS; index++) {
    network_add_node_to_ring(network, &pull->targets[index], 0, 0);
  }

  // No response — this is the pull half of PUSHPULL
}

// --- ClosestNodes handler ---
// Receive a WIRE_CLOSEST_NODES query: execute routing logic, respond or forward.

static void network_handle_closest_nodes(network_t* network, message_t* msg) {
  wire_closest_nodes_t* query = (wire_closest_nodes_t*)msg->payload;
  if (query == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_CLOSEST_NODES, MSG_DIRECTION_RECEIVED,
                       &query->sender_id, query->message_id, NULL,
                       0, &network->hebbian);
  }

  net_node_t* next_hops[CLOSEST_NODES_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  closest_nodes_result_e result = closest_nodes_execute(
      &network->eabf_table, &network->eabf_ttl, &network->conn_mgr,
      network->rings, network->latency_cache, &network->authority->local_id,
      query, next_hops, &next_hop_count);

  switch (result) {
    case CLOSEST_NODES_FOUND: {
      // We are the target or beta-converged — respond with closest info
      const node_id_t* reply_to = &query->original_source;
      if (query->path_len > 0) {
        reply_to = &query->path[query->path_len - 1];
      }
      peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
      if (reply_peer != NULL) {
        wire_closest_nodes_response_t response;
        memset(&response, 0, sizeof(response));
        response.message_id = query->message_id;
        memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));
        memcpy(&response.target_id, &query->target_id, sizeof(node_id_t));
        response.found = 1;
        memcpy(&response.closest, &network->authority->local_id, sizeof(node_id_t));
        // Look up latency from cache
        float latency_ms = 0.0f;
        latency_cache_get(network->latency_cache, &query->target_id, &latency_ms);
        response.closest_latency_us = (uint32_t)(latency_ms * 1000.0f);
        // Reverse the path for response routing
        memcpy(response.path, query->path, query->path_len * sizeof(node_id_t));
        response.path_len = query->path_len;
        response.latency_us = 0;
        // Collect ring samples
        response.ring_count = (uint8_t)closest_nodes_select_ring_samples(
            network->rings, &network->authority->local_id,
            response.ring_nodes, response.ring_latencies_us,
            CLOSEST_NODES_MAX_RING_SAMPLES);

        cbor_item_t* cbor = wire_closest_nodes_response_encode(&response);
        conn_state_send(network, reply_peer, cbor);
        cbor_decref(&cbor);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_CLOSEST_NODES_RESPONSE, MSG_DIRECTION_SENT,
                             reply_to, query->message_id, NULL, 0, &network->hebbian);
        }
      }
      break;
    }

    case CLOSEST_NODES_FORWARDING: {
      // Forward the query to next hops
      for (size_t hop = 0; hop < next_hop_count; hop++) {
        wire_closest_nodes_t* forward = get_clear_memory(sizeof(wire_closest_nodes_t));
        if (forward == NULL) continue;
        forward->message_id = query->message_id;
        memcpy(&forward->sender_id, &network->authority->local_id, sizeof(node_id_t));
        memcpy(&forward->target_id, &query->target_id, sizeof(node_id_t));
        forward->count = query->count;
        forward->beta_numerator = query->beta_numerator;
        forward->beta_denominator = query->beta_denominator;
        forward->ttl = query->ttl - 1;
        memcpy(forward->visited_bloom, query->visited_bloom, CLOSEST_NODES_MAX_VISITED);
        forward->visited_count = query->visited_count;
        closest_nodes_add_visited(forward->visited_bloom, &forward->visited_count,
                                  network->authority->local_id.hash);
        memcpy(forward->path, query->path, query->path_len * sizeof(node_id_t));
        forward->path_len = query->path_len;
        if (forward->path_len < CLOSEST_NODES_MAX_PATH) {
          memcpy(&forward->path[forward->path_len], &network->authority->local_id, sizeof(node_id_t));
          forward->path_len++;
        }
        forward->start_time = query->start_time;
        memcpy(&forward->original_source, &query->original_source, sizeof(node_id_t));

        peer_connection_t* next_peer = connection_manager_lookup(
            &network->conn_mgr, &next_hops[hop]->id);
        if (next_peer != NULL) {
          cbor_item_t* cbor = wire_closest_nodes_encode(forward);
          conn_state_send(network, next_peer, cbor);
          cbor_decref(&cbor);
        }
        free(forward);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_CLOSEST_NODES, MSG_DIRECTION_FORWARDED,
                             &next_hops[hop]->id, query->message_id, NULL,
                             1, &network->hebbian);
        }
      }
      break;
    }

    case CLOSEST_NODES_TTL_EXPIRED:
    case CLOSEST_NODES_NOT_FOUND: {
      // Send NOT_FOUND response back along the path
      const node_id_t* reply_to = &query->original_source;
      if (query->path_len > 0) {
        reply_to = &query->path[query->path_len - 1];
      }
      peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
      if (reply_peer != NULL) {
        wire_closest_nodes_response_t response;
        memset(&response, 0, sizeof(response));
        response.message_id = query->message_id;
        memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));
        memcpy(&response.target_id, &query->target_id, sizeof(node_id_t));
        response.found = 0;
        memcpy(response.path, query->path, query->path_len * sizeof(node_id_t));
        response.path_len = query->path_len;

        cbor_item_t* cbor = wire_closest_nodes_response_encode(&response);
        conn_state_send(network, reply_peer, cbor);
        cbor_decref(&cbor);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_CLOSEST_NODES_RESPONSE, MSG_DIRECTION_SENT,
                             reply_to, query->message_id, NULL,
                             2, &network->hebbian);
        }
      }
      break;
    }
  }
}

// --- ClosestNodes pending reply tracking ---

static void network_closest_pending_add(network_t* network, uint64_t message_id,
                                         actor_t* reply_to);
static actor_t* network_closest_pending_remove(network_t* network, uint64_t message_id);

// --- ClosestNodes response handler ---
// Receive WIRE_CLOSEST_NODES_RESPONSE: update latency cache and forward along path or deliver locally.

static void network_handle_closest_nodes_response(network_t* network, message_t* msg) {
  wire_closest_nodes_response_t* response = (wire_closest_nodes_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_CLOSEST_NODES_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       response->found ? 0 : 2, &network->hebbian);
  }

  // Update latency cache with ring sample latencies
  for (uint8_t index = 0; index < response->ring_count && index < CLOSEST_NODES_MAX_RING_SAMPLES; index++) {
    if (response->ring_latencies_us[index] > 0) {
      float latency_ms = (float)response->ring_latencies_us[index] / 1000.0f;
      latency_cache_insert(network->latency_cache, &response->ring_nodes[index],
                           0, 0, latency_ms);
    }
  }

  if (response->found && response->closest_latency_us > 0) {
    // Also update latency for the closest node
    float latency_ms = (float)response->closest_latency_us / 1000.0f;
    latency_cache_insert(network->latency_cache, &response->closest,
                         0, 0, latency_ms);
  }

  // Forward response along the path (pop the last hop to route back)
  if (response->path_len > 0) {
    // The last node in the path is the next hop toward the origin
    const node_id_t* next_hop = &response->path[response->path_len - 1];
    peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, next_hop);
    if (reply_peer != NULL) {
      // Remove our hop from the path before forwarding
      wire_closest_nodes_response_t forward;
      memset(&forward, 0, sizeof(forward));
      forward.message_id = response->message_id;
      memcpy(&forward.sender_id, &response->sender_id, sizeof(node_id_t));
      memcpy(&forward.target_id, &response->target_id, sizeof(node_id_t));
      forward.found = response->found;
      memcpy(&forward.closest, &response->closest, sizeof(node_id_t));
      forward.closest_latency_us = response->closest_latency_us;
      memcpy(forward.path, response->path, (response->path_len - 1) * sizeof(node_id_t));
      forward.path_len = response->path_len - 1;
      forward.latency_us = response->latency_us;
      memcpy(forward.ring_nodes, response->ring_nodes,
             response->ring_count * sizeof(node_id_t));
      memcpy(forward.ring_latencies_us, response->ring_latencies_us,
             response->ring_count * sizeof(uint32_t));
      forward.ring_count = response->ring_count;

      cbor_item_t* cbor = wire_closest_nodes_response_encode(&forward);
      conn_state_send(network, reply_peer, cbor);
      cbor_decref(&cbor);
    }
  } else {
    // We are the origin — deliver result to the local requestor
    actor_t* reply_to = network_closest_pending_remove(network, response->message_id);
    if (reply_to != NULL) {
      network_closest_nodes_result_payload_t* result =
          get_clear_memory(sizeof(network_closest_nodes_result_payload_t));
      if (result != NULL) {
        result->found = response->found;
        memcpy(&result->closest, &response->closest, sizeof(node_id_t));
        result->closest_latency_us = response->closest_latency_us;
        result->ring_count = response->ring_count;
        if (result->ring_count > CLOSEST_NODES_MAX_RING_SAMPLES_MSG) {
          result->ring_count = CLOSEST_NODES_MAX_RING_SAMPLES_MSG;
        }
        memcpy(result->ring_nodes, response->ring_nodes, result->ring_count * sizeof(node_id_t));
        memcpy(result->ring_latencies_us, response->ring_latencies_us,
               result->ring_count * sizeof(uint32_t));
        result->reply_to = NULL;

        message_t result_msg = {0};
        result_msg.type = NETWORK_CLOSEST_NODES_RESULT;
        result_msg.payload = result;
        result_msg.payload_destroy = network_closest_nodes_result_payload_destroy;
        actor_send(reply_to, &result_msg);
      }
    }
  }
}

// --- MeasureNodes handler ---
// Receive WIRE_MEASURE_NODES: look up latencies and send response back to sender.

static void network_handle_measure_nodes(network_t* network, message_t* msg) {
  wire_measure_nodes_t* measure = (wire_measure_nodes_t*)msg->payload;
  if (measure == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_MEASURE_NODES, MSG_DIRECTION_RECEIVED,
                       &measure->sender_id, measure->message_id, NULL,
                       0, &network->hebbian);
  }

  // Build response
  wire_measure_nodes_response_t response;
  memset(&response, 0, sizeof(response));
  response.message_id = measure->message_id;
  memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));
  response.target_count = (uint8_t)measure_nodes_execute(
      network->latency_cache, measure, response.latencies_us);
  memcpy(response.targets, measure->targets, measure->target_count * sizeof(node_id_t));

  // Send response back to sender
  peer_connection_t* sender_peer = connection_manager_lookup(&network->conn_mgr, &measure->sender_id);
  if (sender_peer != NULL) {
    cbor_item_t* cbor = wire_measure_nodes_response_encode(&response);
    conn_state_send(network, sender_peer, cbor);
    cbor_decref(&cbor);
  }
}

// --- MeasureNodes response handler ---
// Receive WIRE_MEASURE_NODES_RESPONSE: update latency cache with measured latencies.

static void network_handle_measure_nodes_response(network_t* network, message_t* msg) {
  wire_measure_nodes_response_t* response = (wire_measure_nodes_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_MEASURE_NODES_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  // Update latency cache with each target/latency pair
  for (uint8_t index = 0; index < response->target_count && index < MEASURE_NODES_MAX_TARGETS; index++) {
    if (response->latencies_us[index] > 0) {
      float latency_ms = (float)response->latencies_us[index] / 1000.0f;
      latency_cache_insert(network->latency_cache, &response->targets[index],
                           0, 0, latency_ms);
    }
  }
}

// --- ClosestNodes progress handler ---
// Receive WIRE_CLOSEST_NODES_PROGRESS: log the progress update.

static void network_handle_closest_nodes_progress(network_t* network, message_t* msg) {
  wire_closest_nodes_progress_t* progress = (wire_closest_nodes_progress_t*)msg->payload;
  if (progress == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_CLOSEST_NODES_PROGRESS, MSG_DIRECTION_RECEIVED,
                       &progress->sender_id, progress->message_id, NULL,
                       0, &network->hebbian);
  }

  // Progress messages are informational only — no forwarding needed
}

// --- ClosestNodes pending reply tracking ---

static void network_closest_pending_add(network_t* network, uint64_t message_id,
                                         actor_t* reply_to) {
  if (network->closest_pending_count >= CLOSEST_NODES_PENDING_MAX) {
    size_t oldest = 0;
    for (size_t idx = 1; idx < network->closest_pending_count; idx++) {
      if (network->closest_pending[idx].message_id <
          network->closest_pending[oldest].message_id) {
        oldest = idx;
      }
    }
    // Deliver "not found" to the evicted requestor
    actor_t* evicted_reply_to = network->closest_pending[oldest].reply_to;
    if (evicted_reply_to != NULL) {
      network_closest_nodes_result_payload_t* evicted_result =
          get_clear_memory(sizeof(network_closest_nodes_result_payload_t));
      if (evicted_result != NULL) {
        evicted_result->found = 0;
        evicted_result->reply_to = NULL;
        message_t evicted_msg = {0};
        evicted_msg.type = NETWORK_CLOSEST_NODES_RESULT;
        evicted_msg.payload = evicted_result;
        evicted_msg.payload_destroy = network_closest_nodes_result_payload_destroy;
        actor_send(evicted_reply_to, &evicted_msg);
      }
    }
    network->closest_pending[oldest].message_id = message_id;
    network->closest_pending[oldest].reply_to = reply_to;
    return;
  }
  network->closest_pending[network->closest_pending_count].message_id = message_id;
  network->closest_pending[network->closest_pending_count].reply_to = reply_to;
  network->closest_pending_count++;
}

static actor_t* network_closest_pending_remove(network_t* network, uint64_t message_id) {
  for (size_t idx = 0; idx < network->closest_pending_count; idx++) {
    if (network->closest_pending[idx].message_id == message_id) {
      actor_t* reply_to = network->closest_pending[idx].reply_to;
      network->closest_pending[idx] = network->closest_pending[network->closest_pending_count - 1];
      network->closest_pending_count--;
      return reply_to;
    }
  }
  return NULL;
}

// --- Local ClosestNodes handler ---
// Stream requests closest-nodes query: initiate a WIRE_CLOSEST_NODES query.

static void network_handle_local_closest_nodes(network_t* network, message_t* msg) {
  network_local_closest_nodes_payload_t* payload =
      (network_local_closest_nodes_payload_t*)msg->payload;
  if (payload == NULL) return;

  // Build wire message from local request
  wire_closest_nodes_t wire_query;
  memset(&wire_query, 0, sizeof(wire_query));
  uint64_t now_ts = (uint64_t)time(NULL) * 1000;
  wire_query.message_id = now_ts;
  memcpy(&wire_query.sender_id, &network->authority->local_id, sizeof(node_id_t));
  memcpy(&wire_query.target_id, &payload->target_id, sizeof(node_id_t));
  wire_query.count = payload->count;
  wire_query.beta_numerator = payload->beta_numerator;
  wire_query.beta_denominator = payload->beta_denominator;
  wire_query.ttl = CLOSEST_NODES_FORWARD_FANOUT;
  closest_nodes_add_visited(wire_query.visited_bloom, &wire_query.visited_count,
                            network->authority->local_id.hash);
  memcpy(&wire_query.path[0], &network->authority->local_id, sizeof(node_id_t));
  wire_query.path_len = 1;
  wire_query.start_time = now_ts;
  memcpy(&wire_query.original_source, &network->authority->local_id, sizeof(node_id_t));

  // Execute routing logic to find next hops
  net_node_t* next_hops[CLOSEST_NODES_FORWARD_FANOUT];
  size_t next_hop_count = 0;

  closest_nodes_result_e result = closest_nodes_execute(
      &network->eabf_table, &network->eabf_ttl, &network->conn_mgr,
      network->rings, network->latency_cache, &network->authority->local_id,
      &wire_query, next_hops, &next_hop_count);

  if (result == CLOSEST_NODES_FOUND) {
    // We are the closest — deliver result immediately
    network_closest_nodes_result_payload_t* cn_result =
        get_clear_memory(sizeof(network_closest_nodes_result_payload_t));
    if (cn_result != NULL) {
      cn_result->found = 1;
      memcpy(&cn_result->closest, &network->authority->local_id, sizeof(node_id_t));
      cn_result->closest_latency_us = 0;
      cn_result->ring_count = (uint8_t)closest_nodes_select_ring_samples(
          network->rings, &network->authority->local_id,
          cn_result->ring_nodes, cn_result->ring_latencies_us,
          CLOSEST_NODES_MAX_RING_SAMPLES_MSG);
      cn_result->reply_to = payload->reply_to;

      message_t result_msg = {0};
      result_msg.type = NETWORK_CLOSEST_NODES_RESULT;
      result_msg.payload = cn_result;
      result_msg.payload_destroy = network_closest_nodes_result_payload_destroy;
      actor_send(payload->reply_to, &result_msg);
    }
  } else if (result == CLOSEST_NODES_FORWARDING) {
    // Track reply_to so we can deliver the result when response arrives
    network_closest_pending_add(network, wire_query.message_id, payload->reply_to);
    // Forward to next hops
    for (size_t hop = 0; hop < next_hop_count; hop++) {
      peer_connection_t* peer = connection_manager_lookup(
          &network->conn_mgr, &next_hops[hop]->id);
      if (peer != NULL) {
        cbor_item_t* cbor = wire_closest_nodes_encode(&wire_query);
        conn_state_send(network, peer, cbor);
        cbor_decref(&cbor);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_CLOSEST_NODES, MSG_DIRECTION_SENT,
                             &next_hops[hop]->id, wire_query.message_id, NULL,
                             0, &network->hebbian);
        }
      }
    }
  } else {
    // Not found — send failure result to local stream
    network_closest_nodes_result_payload_t* cn_result =
        get_clear_memory(sizeof(network_closest_nodes_result_payload_t));
    if (cn_result != NULL) {
      cn_result->found = 0;
      cn_result->reply_to = payload->reply_to;

      message_t result_msg = {0};
      result_msg.type = NETWORK_CLOSEST_NODES_RESULT;
      result_msg.payload = cn_result;
      result_msg.payload_destroy = network_closest_nodes_result_payload_destroy;
      actor_send(payload->reply_to, &result_msg);
    }
  }
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

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING_CAPACITY, MSG_DIRECTION_RECEIVED,
                       &ping->source, ping->message_id, NULL,
                       0, &network->hebbian);
  }

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
  wire_ping_capacity_response_t response;
  memset(&response, 0, sizeof(response));
  response.message_id = ping->message_id;
  response.capacity = atomic_load(&network->authority->capacity);
  response.phase = atomic_load(&network->authority->phase);
  memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

  // ping->source carries the sender's node_id from the wire message.
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &ping->source);
  if (peer != NULL) {
    cbor_item_t* cbor = wire_ping_capacity_response_encode(&response);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
}

static void network_handle_ping_capacity_response(network_t* network, message_t* msg) {
  wire_ping_capacity_response_t* response = (wire_ping_capacity_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_PING_CAPACITY_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  // Calculate delta_w from RTT. PingCapacity doesn't carry echo_time,
  // so we use a default latency estimate from the gossip timeout.
  float delta_w = hebbian_compute_delta(network->gossip_timeout_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER);

  // Update Hebbian weights: the peer that responded is the one we sent PingCapacity to.
  // Apply frequency rule: w_{self→peer} += delta_w
  hebbian_frequency(&network->hebbian, &response->sender_id, delta_w);
  network_sync_hebbian_to_rings(network);

  // Update the peer's cached capacity/phase in the connection manager
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &response->sender_id);
  if (peer != NULL) {
    // Update the ring table node for this peer
    net_node_t* node = ring_set_find_by_id(network->rings, &response->sender_id);
    if (node != NULL) {
      node->capacity = response->capacity;
      node->phase = response->phase;
      node->last_gossip_time = (uint64_t)time(NULL) * 1000;
      net_node_record_success(node);
    }
  }
}

// --- FindNode handler ---
// Returns the K closest nodes to a target ID from our ring table

static void network_handle_find_node(network_t* network, message_t* msg) {
  wire_find_node_t* find = (wire_find_node_t*)msg->payload;
  if (find == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_FIND_NODE, MSG_DIRECTION_RECEIVED,
                       &find->sender_id, find->message_id, NULL,
                       0, &network->hebbian);
  }

  wire_find_node_response_t* response = get_clear_memory(sizeof(wire_find_node_response_t));
  response->message_id = find->message_id;
  memcpy(&response->sender_id, &network->authority->local_id, sizeof(node_id_t));
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

  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &find->sender_id);
  if (peer != NULL) {
    cbor_item_t* cbor = wire_find_node_response_encode(response);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
  free(response);
}

static void network_handle_find_node_response(network_t* network, message_t* msg) {
  wire_find_node_response_t* response = (wire_find_node_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_FIND_NODE_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  // Insert responding peers into our ring table based on measured latency
  for (uint8_t index = 0; index < response->closest_count; index++) {
    if (ring_set_find_by_id(network->rings, &response->closest_nodes[index]) != NULL) {
      continue;
    }
    float latency_ms = 0;
    latency_cache_get(network->latency_cache, &response->closest_nodes[index], &latency_ms);
    uint32_t latency_us = (uint32_t)(latency_ms * 1000);
    net_node_t* node = net_node_create(&response->closest_nodes[index], 0, 0);
    if (node != NULL) {
      ring_set_insert(network->rings, node, latency_us);
    }
  }
}

// --- FindBlock handler ---
// Directed walk: check local → EABF gravity wells → ring candidates → forward

static void network_handle_find_block(network_t* network, message_t* msg) {
  wire_find_block_t* find = (wire_find_block_t*)msg->payload;
  if (find == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_FIND_BLOCK, MSG_DIRECTION_RECEIVED,
                       &find->original_source, find->message_id, find->block_hash,
                       0, &network->hebbian);
  }

  // Check local block cache first — if we have the block, respond immediately
  if (network->block_cache != NULL && network->block_cache->index != NULL) {
    buffer_t* hash_buf = buffer_create_from_pointer_copy(find->block_hash, 32);
    if (hash_buf != NULL) {
      index_entry_t* entry = index_peek(network->block_cache->index, hash_buf);
      if (entry != NULL) {
        // Block found locally — send FOUND response back along the path
        const node_id_t* reply_to = &find->original_source;
        if (find->path_len > 0) {
          reply_to = &find->path[find->path_len - 1];
        }
        // Hebbian: holder strengthens weight toward predecessor
        {
          float delta_w = hebbian_compute_delta(network->gossip_timeout_ms, HEBBIAN_FIND_BLOCK_MULTIPLIER);
          hebbian_frequency(&network->hebbian, reply_to, delta_w);
          network_sync_hebbian_to_rings(network);
        }
        peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
        if (reply_peer != NULL) {
          wire_find_block_response_t found_resp;
          memset(&found_resp, 0, sizeof(found_resp));
          found_resp.message_id = find->message_id;
          memcpy(found_resp.block_hash, find->block_hash, 32);
          found_resp.found = 1;
          memcpy(&found_resp.holder, &network->authority->local_id, sizeof(node_id_t));
          found_resp.fib = entry->counter.fib;
          // Build response path: incoming path + self (holder)
          memcpy(found_resp.path, find->path, find->path_len * sizeof(node_id_t));
          found_resp.path_len = find->path_len;
          if (found_resp.path_len < WIRE_MAX_PATH) {
            memcpy(&found_resp.path[found_resp.path_len], &network->authority->local_id, sizeof(node_id_t));
            found_resp.path_len++;
          }
          found_resp.latency_ms = 0;

          /* Include block data from LRU cache so the requester can store it */
          block_t* block = block_lru_cache_get(network->block_cache->lru, hash_buf);
          if (block != NULL) {
            found_resp.block_data = block->data->data;
            found_resp.block_data_len = block->data->size;
            found_resp.block_fib = entry->counter.fib;
          }

          cbor_item_t* cbor = wire_find_block_response_encode(&found_resp);
          conn_state_send(network, reply_peer, cbor);
          cbor_decref(&cbor);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_SENT,
                             reply_to, find->message_id, find->block_hash,
                             0, &network->hebbian);
        }

          if (block != NULL) {
            DESTROY(block, block);
          }
        }
        buffer_destroy(hash_buf);
        return;
      }
      buffer_destroy(hash_buf);
    }
  }

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
      // Already handled above via cache check
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
      {
        wire_find_block_response_t not_found;
        memset(&not_found, 0, sizeof(not_found));
        not_found.message_id = state.message_id;
        memcpy(not_found.block_hash, state.block_hash, 32);
        not_found.found = 0;
        memcpy(&not_found.holder, &network->authority->local_id, sizeof(node_id_t));
        not_found.path_len = 0;
        not_found.latency_ms = 0;
        // Reply to the sender: last node in the path, or original_source
        const node_id_t* reply_to = &state.original_source;
        if (state.path_len > 0) {
          reply_to = &state.path[state.path_len - 1];
        }
        peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
        if (reply_peer != NULL) {
          cbor_item_t* cbor = wire_find_block_response_encode(&not_found);
          conn_state_send(network, reply_peer, cbor);
          cbor_decref(&cbor);

        if (network->log != NULL) {
          message_log_record(network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_SENT,
                             reply_to, state.message_id, state.block_hash,
                             2, &network->hebbian);
        }
        }
      }
      break;
    }

    case FIND_BLOCK_FORWARDING: {
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
      // Add self to visited bloom and path
      find_block_add_visited(state.visited_bloom, &state.visited_count, network->authority->local_id.hash);
      if (state.path_len < FIND_BLOCK_MAX_PATH) {
        memcpy(&state.path[state.path_len], &network->authority->local_id, sizeof(node_id_t));
        state.path_len++;
      }
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

        // Encode and send FindBlock to next_hops[hop] via QUIC
        cbor_item_t* cbor = wire_find_block_encode(forward);
        peer_connection_t* next_peer = connection_manager_lookup(
            &network->conn_mgr, &next_hops[hop]->id);
        if (next_peer != NULL) {
          conn_state_send(network, next_peer, cbor);
        }
        cbor_decref(&cbor);
        if (network->log != NULL) {
          message_log_record(network->log, WIRE_FIND_BLOCK, MSG_DIRECTION_FORWARDED,
                             &next_hops[hop]->id, state.message_id, state.block_hash,
                             1, &network->hebbian);
        }
        free(forward);
      }
      break;
    }
  }
}

static void network_handle_find_block_response(network_t* network, message_t* msg) {
  wire_find_block_response_t* response = (wire_find_block_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->holder, response->message_id, response->block_hash,
                       response->found ? 0 : 2, &network->hebbian);
  }

  if (response->found) {
    // Block found — apply Hebbian learning rules along the response path
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

    // Relay found response upstream if we are an intermediate hop.
    // Find self in the path; if we're not the first node, relay to predecessor.
    {
      int self_index = -1;
      for (int index = 0; index < (int)response->path_len; index++) {
        if (node_id_equals(&response->path[index], &network->authority->local_id)) {
          self_index = index;
          break;
        }
      }
      if (self_index > 0) {
        const node_id_t* predecessor = &response->path[self_index - 1];
        peer_connection_t* relay_peer = connection_manager_lookup(&network->conn_mgr, predecessor);
        if (relay_peer != NULL) {
          cbor_item_t* cbor = wire_find_block_response_encode(response);
          conn_state_send(network, relay_peer, cbor);
          cbor_decref(&cbor);
          if (network->log != NULL) {
            message_log_record(network->log, WIRE_FIND_BLOCK_RESPONSE, MSG_DIRECTION_FORWARDED,
                               predecessor, response->message_id, response->block_hash,
                               2, &network->hebbian);
          }
        }
      }
    }

    // If the response includes block data, build a block_t and best-effort
    // store it in the local cache. The block is also attached directly to the
    // NETWORK_FIND_BLOCK_RESULT payload below so the consumer can use it
    // without re-fetching from the cache (cache-full no longer stalls GET).
    block_t* block = NULL;
    if (response->block_data != NULL && response->block_data_len > 0 && network->block_cache != NULL) {
      buffer_t* data_buf = buffer_create_from_pointer_copy(response->block_data, response->block_data_len);
      buffer_t* hash_buf = buffer_create_from_pointer_copy(response->block_hash, 32);
      if (data_buf != NULL && hash_buf != NULL) {
        block_size_e block_type = network->block_cache->type;
        if (response->block_data_len == mega) block_type = mega;
        else if (response->block_data_len == standard) block_type = standard;
        else if (response->block_data_len == mini) block_type = mini;
        else if (response->block_data_len == nano) block_type = nano;
        block = block_create_existing_data_hash_by_type(data_buf, hash_buf, block_type);
      }
      if (block != NULL) {
        block_cache_put(network->block_cache, block, response->block_fib, &network->actor);
        DESTROY(data_buf, buffer);
        DESTROY(hash_buf, buffer);
      } else {
        if (data_buf != NULL) DESTROY(data_buf, buffer);
        if (hash_buf != NULL) DESTROY(hash_buf, buffer);
      }
    }

    // Check wanted_list — notify any local requesters waiting for this block.
    // The block is attached directly to the result so the consumer can use it
    // without re-fetching from the cache. DESTROY(block, block) is deferred
    // until after the result is constructed so the payload's reference keeps
    // the block alive.
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
            result->block = (block != NULL) ? REFERENCE(block, block_t) : NULL;
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

    if (block != NULL) {
      DESTROY(block, block);
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

  if (network->log != NULL) {
    node_id_t store_sender;
    memset(&store_sender, 0, sizeof(node_id_t));
    if (store->path_len > 0) {
      memcpy(&store_sender, &store->path[store->path_len - 1], sizeof(node_id_t));
    }
    message_log_record(network->log, WIRE_STORE_BLOCK, MSG_DIRECTION_RECEIVED,
                       &store_sender, store->message_id, store->block_hash,
                       0, &network->hebbian);
  }

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
      // Block accepted — store in block_cache and forward if more replicas needed
      if (state.replicas_needed > 0) {
        state.replicas_needed--;
      }

      // Build a block_t from the pushed block data and best-effort store it in
      // block_cache via block_cache_put (which references the block internally,
      // so the network retains its own ref). The block is also attached
      // directly to the NETWORK_FIND_BLOCK_RESULT payload below so the
      // consumer can use it without re-fetching from the cache (cache-full no
      // longer stalls GET). The block_t* is hoisted to the case scope so it is
      // visible at the wanted_list notification below; DESTROY(block, block)
      // is deferred until after the result is constructed so the payload's
      // reference keeps the block alive.
      block_t* block = NULL;
      if (store->block_data != NULL && store->block_data_len > 0) {
        buffer_t* hash_buf = buffer_create_from_pointer_copy(store->block_hash, 32);
        buffer_t* data_buf = buffer_create_from_pointer_copy(store->block_data, store->block_data_len);
        if (hash_buf != NULL && data_buf != NULL) {
          block = block_create_existing_data_hash_by_type(
              data_buf, hash_buf, network->block_cache->type);
        }
        if (block != NULL) {
          block_cache_put(network->block_cache, block, store->block_fib, &network->actor);
          DESTROY(data_buf, buffer);
          DESTROY(hash_buf, buffer);
        } else {
          if (data_buf != NULL) DESTROY(data_buf, buffer);
          if (hash_buf != NULL) DESTROY(hash_buf, buffer);
        }
      }

      // If replicas_needed > 0, forward to next hops
      if (state.replicas_needed > 0 && next_hop_count > 0) {
        for (size_t hop = 0; hop < next_hop_count; hop++) {
          wire_store_block_t* forward = get_clear_memory(sizeof(wire_store_block_t));
          if (forward == NULL) continue;
          forward->message_id = state.message_id;
          memcpy(forward->block_hash, state.block_hash, 32);
          forward->block_size = store->block_size;
          forward->block_fib = store->block_fib;
          forward->replicas_needed = state.replicas_needed;
          forward->max_hops = state.max_hops;
          memcpy(forward->visited_bloom, state.visited_bloom, STORE_BLOCK_MAX_VISITED_BLOOM);
          forward->visited_count = state.visited_count;
          memcpy(forward->path, state.path, state.path_len * sizeof(node_id_t));
          forward->path_len = state.path_len;
          forward->start_time = state.start_time_ms;
          forward->carry_data = store->carry_data;
          if (store->carry_data && store->block_data != NULL && store->block_data_len > 0) {
            forward->block_data = get_clear_memory(store->block_data_len);
            if (forward->block_data != NULL) {
              memcpy(forward->block_data, store->block_data, store->block_data_len);
              forward->block_data_len = store->block_data_len;
            }
          }

          cbor_item_t* cbor = wire_store_block_encode(forward);
          peer_connection_t* next_peer = connection_manager_lookup(
              &network->conn_mgr, &next_hops[hop]->id);
          if (next_peer != NULL) {
            conn_state_send(network, next_peer, cbor);
          }
          cbor_decref(&cbor);
        if (network->log != NULL) {
          message_log_record(network->log, WIRE_STORE_BLOCK, MSG_DIRECTION_FORWARDED,
                             &next_hops[hop]->id, state.message_id, state.block_hash,
                             1, &network->hebbian);
        }
          wire_store_block_destroy(forward);
        }
      }

      // Check wanted_list — notify any local requesters waiting for this block.
      // The block is attached directly to the result so the consumer can use it
      // without re-fetching from the cache. block_cache_put already added a
      // reference for the cache actor; the result gets its own reference via
      // refcounter_reference. DESTROY(block, block) is deferred until after
      // the result is constructed so the payload's reference keeps the block
      // alive.
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
              result->block = (block != NULL) ? REFERENCE(block, block_t) : NULL;
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

      if (block != NULL) {
        DESTROY(block, block);
      }

      // Apply Hebbian learning: the accepting node strengthens weight toward
      // the immediate predecessor that sent this block
      {
        const node_id_t* predecessor = &store->path[0];
        if (store->path_len > 0) {
          predecessor = &store->path[store->path_len - 1];
        }
        uint64_t now_ms = (uint64_t)time(NULL) * 1000;
        uint64_t latency_ms = (now_ms > state.start_time_ms)
            ? (now_ms - state.start_time_ms) : 0;
        float delta_w = hebbian_compute_delta((float)latency_ms, HEBBIAN_STORE_BLOCK_EXHALE_MULTIPLIER);
        hebbian_frequency(&network->hebbian, predecessor, delta_w);
        network_sync_hebbian_to_rings(network);
      }

      // Respond with StoreBlockResponse(accepted=true) to the sender
      {
        wire_store_block_response_t accept_resp;
        memset(&accept_resp, 0, sizeof(accept_resp));
        accept_resp.message_id = store->message_id;
        accept_resp.accepted = 1;
        memcpy(&accept_resp.holder, &network->authority->local_id, sizeof(node_id_t));
        memcpy(accept_resp.block_hash, store->block_hash, 32);
        accept_resp.replicas_remaining = (uint8_t)state.replicas_needed;

        // Build response path: incoming path + self
        uint8_t resp_path_len = state.path_len;
        if (resp_path_len < WIRE_MAX_PATH) {
          memcpy(&accept_resp.path[resp_path_len], &network->authority->local_id, sizeof(node_id_t));
          resp_path_len++;
        }
        memcpy(accept_resp.path, state.path, state.path_len * sizeof(node_id_t));
        accept_resp.path_len = resp_path_len;

        uint64_t now_ms = (uint64_t)time(NULL) * 1000;
        accept_resp.latency_ms = (now_ms > state.start_time_ms)
            ? (uint64_t)(now_ms - state.start_time_ms) : 0;

        const node_id_t* reply_to = &store->path[0];
        if (store->path_len > 0) {
          reply_to = &store->path[store->path_len - 1];
        }
        peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
        if (reply_peer != NULL) {
          cbor_item_t* cbor = wire_store_block_response_encode(&accept_resp);
          conn_state_send(network, reply_peer, cbor);
          cbor_decref(&cbor);
          if (network->log != NULL) {
            message_log_record(network->log, WIRE_STORE_BLOCK_RESPONSE, MSG_DIRECTION_SENT,
                               reply_to, store->message_id, store->block_hash,
                               0, &network->hebbian);
          }
        }
      }
      break;
    }

    case STORE_BLOCK_DECLINED:
    case STORE_BLOCK_MAX_HOPS_REACHED: {
      // Decline — respond with StoreBlockResponse(accepted=false)
      {
        wire_store_block_response_t decline;
        memset(&decline, 0, sizeof(decline));
        decline.message_id = store->message_id;
        decline.accepted = 0;
        memcpy(&decline.holder, &network->authority->local_id, sizeof(node_id_t));
        memcpy(decline.block_hash, store->block_hash, 32);
        decline.replicas_remaining = 0;
        decline.path_len = 0;
        decline.latency_ms = 0;
        // Reply to sender: last node in path, or original_source
        const node_id_t* reply_to = &store->path[0];
        if (store->path_len > 0) {
          reply_to = &store->path[store->path_len - 1];
        }
        peer_connection_t* reply_peer = connection_manager_lookup(&network->conn_mgr, reply_to);
        if (reply_peer != NULL) {
          cbor_item_t* cbor = wire_store_block_response_encode(&decline);
          conn_state_send(network, reply_peer, cbor);
          cbor_decref(&cbor);
        if (network->log != NULL) {
          message_log_record(network->log, WIRE_STORE_BLOCK_RESPONSE, MSG_DIRECTION_SENT,
                             reply_to, store->message_id, store->block_hash,
                             3, &network->hebbian);
        }
        }
      }
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

        // Encode and send StoreBlock to next_hops[hop] via QUIC
        cbor_item_t* cbor = wire_store_block_encode(forward);
        peer_connection_t* next_peer = connection_manager_lookup(
            &network->conn_mgr, &next_hops[hop]->id);
        if (next_peer != NULL) {
          conn_state_send(network, next_peer, cbor);
        }
        cbor_decref(&cbor);
      if (network->log != NULL) {
        message_log_record(network->log, WIRE_STORE_BLOCK, MSG_DIRECTION_FORWARDED,
                           &next_hops[hop]->id, state.message_id, state.block_hash,
                           1, &network->hebbian);
      }
        wire_store_block_destroy(forward);
      }
      break;
    }
  }
}

static void network_handle_store_block_response(network_t* network, message_t* msg) {
  wire_store_block_response_t* response = (wire_store_block_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_STORE_BLOCK_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->holder, response->message_id, response->block_hash,
                       response->accepted ? 0 : 3, &network->hebbian);
  }

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
        eabf_subscribe(eabf, response->block_hash, 32);
      }
    }

    // Populate peer EABFs along the response path.
    for (size_t index = 0; index + 1 < response->path_len; index++) {
      peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &response->path[index + 1]);
      if (peer != NULL) {
        peer_eabf_subscribe(peer, response->block_hash, 32);
      }
    }

    // Relay accepted response upstream if we are an intermediate hop.
    // Find self in the path; if we're not the first node, relay to our predecessor.
    {
      int self_index = -1;
      for (int index = 0; index < (int)response->path_len; index++) {
        if (node_id_equals(&response->path[index], &network->authority->local_id)) {
          self_index = index;
          break;
        }
      }
      if (self_index > 0) {
        // We are an intermediate hop — relay response to predecessor
        const node_id_t* predecessor = &response->path[self_index - 1];
        peer_connection_t* relay_peer = connection_manager_lookup(&network->conn_mgr, predecessor);
        if (relay_peer != NULL) {
          cbor_item_t* cbor = wire_store_block_response_encode(response);
          conn_state_send(network, relay_peer, cbor);
          cbor_decref(&cbor);
          if (network->log != NULL) {
            message_log_record(network->log, WIRE_STORE_BLOCK_RESPONSE, MSG_DIRECTION_FORWARDED,
                               predecessor, response->message_id, response->block_hash,
                               2, &network->hebbian);
          }
        }
      }
    }

    // Recall on block acquisition: check all peers' EABFs at level 0 for this hash.
    // If a peer previously requested this block (failed FindBlock), push it to them.
    {
      size_t recall_count = 0;
      peer_connection_t** recall_peers = connection_manager_get_peers_for_topic(
          &network->conn_mgr, response->block_hash, 32, &recall_count);
      if (recall_peers != NULL && recall_count > 0) {
        for (size_t index = 0; index < recall_count; index++) {
          peer_connection_t* peer = recall_peers[index];
          // Apply amplified Hebbian reinforcement for recall
          peer_hebbian_update(peer, network->conn_mgr.hebbian.recall_reward);
          // Send RecallBlock to peer via QUIC — full StoreBlock RECALL
          // will be wired when block data streaming is implemented.
          // For now, send a RecallBlock request so the peer can re-request.
          {
            wire_recall_block_t recall;
            memset(&recall, 0, sizeof(recall));
            recall.message_id = response->message_id;
            memcpy(recall.block_hash, response->block_hash, 32);
            memcpy(&recall.sender_id, &network->authority->local_id, sizeof(node_id_t));
            cbor_item_t* cbor = wire_recall_block_encode(&recall);
            conn_state_send(network, peer, cbor);
            cbor_decref(&cbor);
          }
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

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_SEEKING_BLOCKS, MSG_DIRECTION_RECEIVED,
                       &seeking->sender_id, seeking->message_id, NULL,
                       0, &network->hebbian);
  }

  float local_capacity = atomic_load(&network->authority->capacity);

  // Only respond if we have blocks and the requester has capacity < 50%
  if (local_capacity <= 0.0f) {
    // No blocks to offer
    return;
  }

  // Build response with offers from our local block index
  wire_seeking_blocks_response_t* response = get_clear_memory(sizeof(wire_seeking_blocks_response_t));
  if (response == NULL) return;
  response->message_id = seeking->message_id;
  memcpy(&response->sender_id, &network->authority->local_id, sizeof(node_id_t));
  response->offer_count = 0;

  // Walk the block index and select up to WIRE_MAX_OFFERS blocks,
  // sorted by FIB counter descending, excluding any hashes in seeking->exclude_hashes
  index_entry_vec_t* entries = index_to_array(network->block_cache->index);
  if (entries != NULL) {
    // Sort entries by FIB counter descending (highest FIB = most important)
    size_t entry_count = 0;
    for (size_t index = 0; index < entries->length && entry_count < (size_t)WIRE_MAX_OFFERS; index++) {
      index_entry_t* entry = entries->data[index];
      if (entry == NULL || entry->hash == NULL) continue;

      // Check if this hash is in the exclude list
      uint8_t excluded = 0;
      for (size_t ex = 0; ex < seeking->exclude_count; ex++) {
        if (seeking->exclude_hashes[ex] != NULL &&
            memcmp(entry->hash->data, seeking->exclude_hashes[ex], 32) == 0) {
          excluded = 1;
          break;
        }
      }
      if (excluded) continue;

      // Add this block to the offers
      memcpy(response->offers[entry_count].hash, entry->hash->data, 32);
      response->offers[entry_count].fib = entry->counter.fib;
      response->offers[entry_count].size = 0;  // Size not tracked in index_entry
      entry_count++;
    }
    response->offer_count = (uint8_t)entry_count;
    vec_deinit(entries);
    free(entries);
  }

  // Encode and send SeekingBlocksResponse via QUIC to the sender's peer connection.
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &seeking->sender_id);
  if (peer != NULL) {
    cbor_item_t* cbor = wire_seeking_blocks_response_encode(response);
    conn_state_send(network, peer, cbor);
    cbor_decref(&cbor);
  }
  free(response);
}

static void network_handle_seeking_blocks_response(network_t* network, message_t* msg) {
  wire_seeking_blocks_response_t* response = (wire_seeking_blocks_response_t*)msg->payload;
  if (response == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_SEEKING_BLOCKS_RESPONSE, MSG_DIRECTION_RECEIVED,
                       &response->sender_id, response->message_id, NULL,
                       0, &network->hebbian);
  }

  float local_capacity = atomic_load(&network->authority->capacity);

  // Evaluate each offer: if we should pull (capacity < 50% and block not stored)
  if (!respiration_should_inhale(local_capacity)) {
    return;  // We're not in inhale phase anymore
  }

  for (uint8_t index = 0; index < response->offer_count; index++) {
    wire_block_offer_t* offer = &response->offers[index];

    // Check if block is already stored locally
    buffer_t* hash_buf = buffer_create_from_pointer_copy(offer->hash, 32);
    if (hash_buf == NULL) continue;

    index_entry_t* entry = index_peek(network->block_cache->index, hash_buf);
    if (entry != NULL) {
      // Block already stored — skip this offer
      buffer_destroy(hash_buf);
      continue;
    }

    // Block not stored and we should inhale — send FindBlock for this hash
    if (respiration_should_inhale(local_capacity)) {
      // Add to wanted list and route a FindBlock
      network_local_find_block_payload_t find_payload;
      memset(&find_payload, 0, sizeof(find_payload));
      find_payload.hash = hash_buf;
      find_payload.reply_to = &network->actor;
      message_t find_msg = {0};
      find_msg.type = NETWORK_LOCAL_FIND_BLOCK;
      find_msg.payload = &find_payload;
      find_msg.payload_destroy = NULL;  // stack-allocated payload
      network_handle_local_find_block(network, &find_msg);
      buffer_destroy(hash_buf);
    } else {
      buffer_destroy(hash_buf);
    }
  }
}

// --- RankBlock handler ---
// Fire-and-forget: upgrade our local rank or initiate seek

static void network_handle_rank_block(network_t* network, message_t* msg) {
  wire_rank_block_t* rank = (wire_rank_block_t*)msg->payload;
  if (rank == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_RANK_BLOCK, MSG_DIRECTION_RECEIVED,
                       &rank->origin, 0, rank->block_hash,
                       0, &network->hebbian);
  }

  // Add self to visited bloom and path
  find_block_add_visited(rank->visited_bloom, &rank->visited_count,
                         network->authority->local_id.hash);
  if (rank->path_len < WIRE_MAX_PATH) {
    memcpy(&rank->path[rank->path_len], &network->authority->local_id, sizeof(node_id_t));
    rank->path_len++;
  }

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
      uint64_t now_ts = (uint64_t)time(NULL) * 1000;
      find->message_id = now_ts + rank->count;
      memcpy(find->block_hash, rank->block_hash, 32);
      find->ttl = FIND_BLOCK_FORWARD_FANOUT;
      memset(find->visited_bloom, 0, WIRE_MAX_VISITED_BLOOM);
      find->visited_count = 0;
      find->path_len = 0;
      find->start_time = now_ts;
      memcpy(&find->original_source, &network->authority->local_id, sizeof(node_id_t));

      message_t find_msg;
      find_msg.type = NETWORK_FIND_BLOCK;
      find_msg.payload = find;
      find_msg.payload_destroy = free;
      network_handle_find_block(network, &find_msg);
    }
  }

  // If hop_count < RANK_BLOCK_MAX_HOPS → forward to random subset of peers
  if (rank->hop_count < RANK_BLOCK_MAX_HOPS) {
    // Select up to 3 random peers from ring table, filtering by visited bloom and path
    net_node_t* candidates[RING_K * RING_MAX_RINGS];
    size_t candidate_count = 0;

    for (size_t ring_index = 0; ring_index < network->rings->ring_count && candidate_count < RANK_BLOCK_FORWARD_FANOUT; ring_index++) {
      ring_t* ring = &network->rings->rings[ring_index];
      for (int node_index = 0; node_index < ring->primary.length && candidate_count < RANK_BLOCK_FORWARD_FANOUT; node_index++) {
        net_node_t* node = ring->primary.data[node_index];
        if (node == NULL) continue;
        if (node->flags & NET_NODE_FLAG_RENDEZVOUS) continue;

        // Skip nodes already in the path
        bool in_path = false;
        for (uint8_t path_index = 0; path_index < rank->path_len; path_index++) {
          if (node_id_equals(&rank->path[path_index], &node->id)) {
            in_path = true;
            break;
          }
        }
        if (in_path) continue;

        // Skip nodes already in visited bloom
        if (find_block_is_visited(rank->visited_bloom, rank->visited_count, node->id.hash)) {
          continue;
        }

        candidates[candidate_count] = node;
        candidate_count++;
      }
    }

    // Increment hop count for forwarded messages
    rank->hop_count++;

    for (size_t hop = 0; hop < candidate_count; hop++) {
      wire_rank_block_t* forward = get_clear_memory(sizeof(wire_rank_block_t));
      if (forward == NULL) continue;
      memcpy(forward->block_hash, rank->block_hash, 32);
      forward->fib = rank->fib;
      forward->count = rank->count;
      memcpy(&forward->origin, &rank->origin, sizeof(node_id_t));
      forward->hop_count = rank->hop_count;
      memcpy(forward->visited_bloom, rank->visited_bloom, WIRE_MAX_VISITED_BLOOM);
      forward->visited_count = rank->visited_count;
      memcpy(forward->path, rank->path, rank->path_len * sizeof(node_id_t));
      forward->path_len = rank->path_len;

      // Encode and send RankBlock to candidate peer
      cbor_item_t* cbor = wire_rank_block_encode(forward);
      peer_connection_t* candidate_peer = connection_manager_lookup(
          &network->conn_mgr, &candidates[hop]->id);
      if (candidate_peer != NULL) {
        conn_state_send(network, candidate_peer, cbor);
      }
      cbor_decref(&cbor);
      if (network->log != NULL) {
        message_log_record(network->log, WIRE_RANK_BLOCK, MSG_DIRECTION_FORWARDED,
                           &candidates[hop]->id, 0, rank->block_hash,
                           1, &network->hebbian);
      }
      free(forward);
    }
  }
}

// --- RecallBlock handler ---

static void network_handle_recall_block(network_t* network, message_t* msg) {
  wire_recall_block_t* recall = (wire_recall_block_t*)msg->payload;
  if (recall == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_RECALL_BLOCK, MSG_DIRECTION_RECEIVED,
                       &recall->sender_id, recall->message_id, recall->block_hash,
                       0, &network->hebbian);
  }

  float local_capacity = atomic_load(&network->authority->capacity);

  // If capacity < 50% -> accept recall, otherwise decline
  if (local_capacity < RESPIRATION_INHALE_THRESHOLD) {
    // Send RecallAccept
    wire_recall_accept_t response;
    memset(&response, 0, sizeof(response));
    response.message_id = recall->message_id;
    memcpy(response.block_hash, recall->block_hash, 32);
    memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &recall->sender_id);
    if (peer != NULL) {
      cbor_item_t* cbor = wire_recall_accept_encode(&response);
      conn_state_send(network, peer, cbor);
      cbor_decref(&cbor);
    }
  } else {
    // Send RecallDecline
    wire_recall_decline_t response;
    memset(&response, 0, sizeof(response));
    response.message_id = recall->message_id;
    memcpy(response.block_hash, recall->block_hash, 32);
    memcpy(&response.sender_id, &network->authority->local_id, sizeof(node_id_t));

    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &recall->sender_id);
    if (peer != NULL) {
      cbor_item_t* cbor = wire_recall_decline_encode(&response);
      conn_state_send(network, peer, cbor);
      cbor_decref(&cbor);
    }
  }
}

static void network_handle_recall_accept(network_t* network, message_t* msg) {
  wire_recall_accept_t* accept = (wire_recall_accept_t*)msg->payload;
  if (accept == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_RECALL_ACCEPT, MSG_DIRECTION_RECEIVED,
                       &accept->sender_id, accept->message_id, accept->block_hash,
                       0, &network->hebbian);
  }

  // If the accept message carries block data, build a block_t and best-effort
  // store it in the local block_cache. The block is also attached directly to
  // the NETWORK_FIND_BLOCK_RESULT payload below so the consumer can use it
  // without re-fetching from the cache (cache-full no longer stalls GET).
  // The block_t* is hoisted to the function scope so it is visible at the
  // wanted_list notification below; DESTROY(block, block) is deferred until
  // after the result is constructed so the payload's reference keeps the
  // block alive.
  block_t* block = NULL;
  if (accept->block_data != NULL && accept->block_data_len > 0 && network->block_cache != NULL) {
    buffer_t* data_buf = buffer_create_from_pointer_copy(accept->block_data, accept->block_data_len);
    buffer_t* block_hash_buf = buffer_create_from_pointer_copy(accept->block_hash, 32);
    if (data_buf != NULL && block_hash_buf != NULL) {
      block_size_e block_type = network->block_cache->type;
      if (accept->block_data_len == mega) block_type = mega;
      else if (accept->block_data_len == standard) block_type = standard;
      else if (accept->block_data_len == mini) block_type = mini;
      else if (accept->block_data_len == nano) block_type = nano;
      block = block_create_existing_data_hash_by_type(data_buf, block_hash_buf, block_type);
    }
    if (block != NULL) {
      block_cache_put(network->block_cache, block, accept->block_fib, &network->actor);
      DESTROY(data_buf, buffer);
      DESTROY(block_hash_buf, buffer);
    } else {
      if (data_buf != NULL) DESTROY(data_buf, buffer);
      if (block_hash_buf != NULL) DESTROY(block_hash_buf, buffer);
    }
  }

  // Look up the block by hash in the local block_cache index
  buffer_t* hash_buf = buffer_create_from_pointer_copy(accept->block_hash, 32);
  if (hash_buf == NULL) {
    if (block != NULL) {
      DESTROY(block, block);
    }
    return;
  }

  index_entry_t* entry = index_peek(network->block_cache->index, hash_buf);
  if (entry != NULL) {
    // Block found locally — send StoreBlock with the block data to the requesting peer
    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &accept->sender_id);

    if (peer != NULL) {
      wire_store_block_t store_msg;
      memset(&store_msg, 0, sizeof(store_msg));
      store_msg.message_id = accept->message_id;
      memcpy(store_msg.block_hash, accept->block_hash, 32);
      store_msg.block_fib = entry->counter.fib;
      store_msg.replicas_needed = 0;  // RECALL — no further replication needed
      store_msg.max_hops = 1;  // Direct delivery, no forwarding
      store_msg.carry_data = 1;
      // If the accept message carries block data, forward it
      if (accept->block_data != NULL && accept->block_data_len > 0) {
        store_msg.block_data = accept->block_data;
        store_msg.block_data_len = accept->block_data_len;
        store_msg.block_size = (uint32_t)accept->block_data_len;
      }
      store_msg.start_time = (uint64_t)time(NULL) * 1000;

      cbor_item_t* cbor = wire_store_block_encode(&store_msg);
      conn_state_send(network, peer, cbor);
      cbor_decref(&cbor);
    }
  }
  buffer_destroy(hash_buf);

  // Check wanted_list — notify any local requesters waiting for this block.
  // The block is attached directly to the result so the consumer can use it
  // without re-fetching from the cache. block_cache_put already added a
  // reference for the cache actor; the result gets its own reference via
  // refcounter_reference. DESTROY(block, block) is deferred until after the
  // result is constructed so the payload's reference keeps the block alive.
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
          result->block = (block != NULL) ? REFERENCE(block, block_t) : NULL;
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

  if (block != NULL) {
    DESTROY(block, block);
  }
}

static void network_handle_recall_decline(network_t* network, message_t* msg) {
  wire_recall_decline_t* decline = (wire_recall_decline_t*)msg->payload;
  if (decline == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_RECALL_DECLINE, MSG_DIRECTION_RECEIVED,
                       &decline->sender_id, decline->message_id, decline->block_hash,
                       3, &network->hebbian);
  }

  // Remove the declined block hash from the sender's EABF to stop
  // sending recall requests for blocks the sender doesn't want.
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &decline->sender_id);
  if (peer != NULL && peer->eabf != NULL) {
    // Unsubscribe the block hash from the peer's EABF so we stop
    // pushing this block to them in future exhale cycles.
    eabf_unsubscribe(peer->eabf, decline->block_hash, 32);
  }

  // Also remove from our own global EABFs as negative signal
  for (size_t index = 0; index < network->eabf_table.count; index++) {
    eabf_t* eabf = network->eabf_table.entries[index].eabf;
    if (eabf != NULL) {
      eabf_unsubscribe(eabf, decline->block_hash, 32);
    }
  }
}

// --- RateLimited handler ---

static void network_handle_rate_limited(network_t* network, message_t* msg) {
  wire_rate_limited_t* limited = (wire_rate_limited_t*)msg->payload;
  if (limited == NULL) return;

  if (network->log != NULL) {
    message_log_record(network->log, WIRE_RATE_LIMITED, MSG_DIRECTION_RECEIVED,
                       &limited->sender_id, limited->message_id, NULL,
                       0, &network->hebbian);
  }

  // Look up the peer's rate limit entry and drain tokens for the specified RPC type.
  // This ensures we respect the peer's backoff signal and don't immediately retry.
  peer_rate_limits_t* limits = rate_limit_table_get(&network->rate_limits, &limited->sender_id);
  if (limits != NULL && limited->type < RPC_TYPE_COUNT) {
    token_bucket_t* bucket = &limits->buckets[limited->type];
    // Drain all tokens to enforce the backoff — the peer told us to stop sending
    bucket->tokens = 0;
    bucket->last_refill = (uint64_t)time(NULL) * 1000;
  }
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
  uint64_t now_ts = (uint64_t)time(NULL) * 1000;
  state.message_id = now_ts;
  memcpy(state.block_hash, payload->hash->data, 32);
  state.ttl = FIND_BLOCK_FORWARD_FANOUT;
  state.start_time_ms = now_ts;
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

        // Encode and send FindBlock to next_hops[hop] via QUIC
        cbor_item_t* cbor = wire_find_block_encode(forward);
        peer_connection_t* next_peer = connection_manager_lookup(
            &network->conn_mgr, &next_hops[hop]->id);
        if (next_peer != NULL) {
          conn_state_send(network, next_peer, cbor);
        }
        cbor_decref(&cbor);
        free(forward);
      }
      break;
    }
  }
}

// --- Friend reconnect tick handler ---
static void network_handle_friend_reconnect_tick(network_t* network, message_t* msg) {
  (void)msg;
  if (network->authority == NULL || network->authority->friend_peers == NULL) return;

  for (size_t index = 0; index < network->authority->friend_peer_count; index++) {
    peer_info_t* friend_info = network->authority->friend_peers[index];
    peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &friend_info->node_id);
    if (peer != NULL && peer->connected) continue;  // Already connected

    // Attempt reconnect on first direct address
    for (size_t addr_index = 0; addr_index < friend_info->address_count; addr_index++) {
      peer_address_t* addr = &friend_info->addresses[addr_index];
      if (addr->type == PEER_ADDR_DIRECT) {
        if (peer == NULL) {
          peer = connection_manager_add_friend(&network->conn_mgr,
              &friend_info->node_id, NULL, network->pool);
        }
        if (peer != NULL && !peer->connected) {
          network_connect_peer(network, addr->host, addr->port);
        }
        break;
      }
    }
  }
}

// --- Start connections to bootstrap and friend peers ---
void network_start_connections(network_t* network) {
  if (network == NULL) return;

  // Connect to bootstrap peers (fire-and-forget)
  if (network->authority != NULL && network->authority->bootstrap_peers != NULL) {
    for (size_t index = 0; index < network->authority->bootstrap_peer_count; index++) {
      char* peer_str = network->authority->bootstrap_peers[index];
      // Parse host:port from string
      char* colon = strchr(peer_str, ':');
      if (colon != NULL) {
        *colon = '\0';
        uint16_t port = (uint16_t)atoi(colon + 1);
        network_connect_peer(network, peer_str, port);
        *colon = ':';
      }
    }
  }

  // Connect to friend peers
  if (network->authority != NULL && network->authority->friend_peers != NULL) {
    for (size_t index = 0; index < network->authority->friend_peer_count; index++) {
      peer_info_t* friend_info = network->authority->friend_peers[index];
      peer_connection_t* existing = connection_manager_lookup(&network->conn_mgr, &friend_info->node_id);
      if (existing != NULL && existing->connected) continue;
      for (size_t addr_index = 0; addr_index < friend_info->address_count; addr_index++) {
        peer_address_t* addr = &friend_info->addresses[addr_index];
        if (addr->type == PEER_ADDR_DIRECT) {
          connection_manager_add_friend(&network->conn_mgr, &friend_info->node_id, NULL, network->pool);
          network_connect_peer(network, addr->host, addr->port);
          break;  // Use first direct address
        }
      }
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
    case NETWORK_PING_CAPACITY_TICK:
      network_handle_ping_capacity_tick(network, msg);
      break;
    case NETWORK_FRIEND_RECONNECT_TICK:
      network_handle_friend_reconnect_tick(network, msg);
      break;
    case NETWORK_GOSSIP_RECEIVED:
      network_handle_gossip_received(network, msg);
      break;
    case NETWORK_GOSSIP_PULL_RECEIVED:
      network_handle_gossip_pull_received(network, msg);
      break;
    case NETWORK_METRICS_PUSH: {
      size_t peer_count = connection_manager_count(&network->conn_mgr);
      rate_limit_table_set_peer_count(&network->rate_limits, peer_count);
      if (peer_count > 0) {
        peer_metrics_snapshot_t* snapshots = get_clear_memory(peer_count * sizeof(peer_metrics_snapshot_t));
        if (snapshots != NULL) {
          size_t index = 0;
          for (size_t peer_index = 0; peer_index < network->conn_mgr.peer_capacity && index < peer_count; peer_index++) {
            peer_connection_t* peer = network->conn_mgr.peers[peer_index];
            if (peer != NULL && peer->connected) {
              peer_get_metrics(peer, &network->rate_limits, &snapshots[index]);
              index++;
            }
          }
          topology_metrics_update_peers(network->topology_metrics, snapshots, index);
          free(snapshots);
        }
      }

      /* Collect ring topology entries */
      if (network->rings != NULL) {
        size_t ring_capacity = network->topology_metrics->ring_entry_capacity;
        if (ring_capacity == 0) {
          ring_capacity = 64;
          network->topology_metrics->ring_entries = get_clear_memory(
            ring_capacity * sizeof(ring_topology_entry_t));
          network->topology_metrics->ring_entry_capacity = ring_capacity;
        }
        size_t ring_count = ring_set_collect_topology(
          network->rings,
          network->topology_metrics->ring_entries,
          ring_capacity);
        network->topology_metrics->ring_entry_count = ring_count;
      }

      /* Push to metrics server if configured */
      if (network->authority != NULL &&
          network->authority->metrics_server_url != NULL) {
        uint64_t timestamp_ms = (uint64_t)time(NULL) * 1000;
        cbor_item_t* report = topology_report_encode(
          &network->authority->local_id,
          timestamp_ms,
          network->topology_metrics);
        if (report != NULL) {
          topology_report_post(
            network->authority->metrics_server_url, report);
          cbor_decref(&report);
        }
      }
      break;
    }
    case NETWORK_QUIC_DATA: {
      quic_data_payload_t* quic_data = (quic_data_payload_t*)msg->payload;
      if (quic_data == NULL) break;
      if (quic_data->data == NULL || quic_data->length == 0) break;
      if (quic_data->length > 2097152) { /* 2MB — accommodates mega blocks + CBOR overhead */
        break;
      }
      // Decode CBOR wire message and re-dispatch
      struct cbor_load_result load_result;
      cbor_item_t* wire_msg = cbor_load(quic_data->data, quic_data->length, &load_result);
      if (wire_msg == NULL) {
        log_error("QUIC_DATA: cbor_load failed, error=%d read=%zu",
                  load_result.error.code, load_result.read);
        break;
      }
      if (!cbor_isa_array(wire_msg) || cbor_array_size(wire_msg) < 1) {
        log_error("QUIC_DATA: not a CBOR array, type=%d size=%zu",
                  cbor_typeof(wire_msg), cbor_refcount(wire_msg));
        cbor_decref(&wire_msg);
        break;
      }
      uint8_t type = wire_get_type(wire_msg);
      // Drop non-salutation messages from unauthenticated QUIC connections
      if (type != WIRE_SALUTATION &&
          pending_quic_find(network, quic_data->quic_connection) != NULL) {
        cbor_decref(&wire_msg);
        break;
      }
      message_t dispatch_msg;
      memset(&dispatch_msg, 0, sizeof(dispatch_msg));
      dispatch_msg.type = type;
      dispatch_msg.payload_destroy = free;
      switch (type) {
        case WIRE_SALUTATION: {
          wire_salutation_t* payload = get_clear_memory(sizeof(wire_salutation_t));
          if (wire_salutation_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            dispatch_msg.payload_destroy = (void (*)(void*))wire_salutation_destroy;
            network_handle_salutation(network, &dispatch_msg, quic_data->quic_connection);
          } else {
            wire_salutation_destroy(payload);
          }
          break;
        }
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
        case WIRE_PING_RESPONSE: {
          wire_ping_response_t* payload = get_clear_memory(sizeof(wire_ping_response_t));
          if (wire_ping_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            network_handle_ping_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_PING_CAPACITY_RESPONSE: {
          wire_ping_capacity_response_t* payload = get_clear_memory(sizeof(wire_ping_capacity_response_t));
          if (wire_ping_capacity_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            network_handle_ping_capacity_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_PING_BLOCK_RESPONSE: {
          wire_ping_block_response_t* payload = get_clear_memory(sizeof(wire_ping_block_response_t));
          if (wire_ping_block_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            network_handle_ping_block_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_FIND_BLOCK_RESPONSE: {
          wire_find_block_response_t* payload = get_clear_memory(sizeof(wire_find_block_response_t));
          if (wire_find_block_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            dispatch_msg.payload_destroy = (void (*)(void*))wire_find_block_response_destroy;
            network_handle_find_block_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_FIND_NODE_RESPONSE: {
          wire_find_node_response_t* payload = get_clear_memory(sizeof(wire_find_node_response_t));
          if (wire_find_node_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            network_handle_find_node_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_STORE_BLOCK_RESPONSE: {
          wire_store_block_response_t* payload = get_clear_memory(sizeof(wire_store_block_response_t));
          if (wire_store_block_response_decode(wire_msg, payload) == 0) {
            dispatch_msg.payload = payload;
            network_handle_store_block_response(network, &dispatch_msg);
          } else {
            free(payload);
          }
          break;
        }
        case WIRE_SEEKING_BLOCKS_RESPONSE: {
          wire_seeking_blocks_response_t* payload = get_clear_memory(sizeof(wire_seeking_blocks_response_t));
          if (payload != NULL) {
            if (wire_seeking_blocks_response_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_seeking_blocks_response(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_RECALL_ACCEPT: {
          wire_recall_accept_t* payload = get_clear_memory(sizeof(wire_recall_accept_t));
          if (payload != NULL) {
            if (wire_recall_accept_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              dispatch_msg.payload_destroy = (void (*)(void*))wire_recall_accept_destroy;
              network_handle_recall_accept(network, &dispatch_msg);
            } else {
              wire_recall_accept_destroy(payload);
            }
          }
          break;
        }
        case WIRE_RECALL_DECLINE: {
          wire_recall_decline_t* payload = get_clear_memory(sizeof(wire_recall_decline_t));
          if (payload != NULL) {
            if (wire_recall_decline_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_recall_decline(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_RATE_LIMITED: {
          wire_rate_limited_t* payload = get_clear_memory(sizeof(wire_rate_limited_t));
          if (payload != NULL) {
            if (wire_rate_limited_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_rate_limited(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_GOSSIP: {
          wire_gossip_t* payload = get_clear_memory(sizeof(wire_gossip_t));
          if (payload != NULL) {
            if (wire_gossip_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_gossip_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_GOSSIP_PULL: {
          wire_gossip_pull_t* payload = get_clear_memory(sizeof(wire_gossip_pull_t));
          if (payload != NULL) {
            if (wire_gossip_pull_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_gossip_pull_received(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_CLOSEST_NODES: {
          wire_closest_nodes_t* payload = get_clear_memory(sizeof(wire_closest_nodes_t));
          if (payload != NULL) {
            if (wire_closest_nodes_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_closest_nodes(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_CLOSEST_NODES_RESPONSE: {
          wire_closest_nodes_response_t* payload = get_clear_memory(sizeof(wire_closest_nodes_response_t));
          if (payload != NULL) {
            if (wire_closest_nodes_response_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_closest_nodes_response(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_MEASURE_NODES: {
          wire_measure_nodes_t* payload = get_clear_memory(sizeof(wire_measure_nodes_t));
          if (payload != NULL) {
            if (wire_measure_nodes_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_measure_nodes(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_MEASURE_NODES_RESPONSE: {
          wire_measure_nodes_response_t* payload = get_clear_memory(sizeof(wire_measure_nodes_response_t));
          if (payload != NULL) {
            if (wire_measure_nodes_response_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_measure_nodes_response(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        case WIRE_CLOSEST_NODES_PROGRESS: {
          wire_closest_nodes_progress_t* payload = get_clear_memory(sizeof(wire_closest_nodes_progress_t));
          if (payload != NULL) {
            if (wire_closest_nodes_progress_decode(wire_msg, payload) == 0) {
              dispatch_msg.payload = payload;
              network_handle_closest_nodes_progress(network, &dispatch_msg);
            } else {
              free(payload);
            }
          }
          break;
        }
        default:
          break;
      }
      /* The synchronous dispatch bypasses actor_run, so the framework does
         not free the payload. Handlers that CONSUME the payload set
         dispatch_msg.payload = NULL; respect that. See audit #2. */
      if (dispatch_msg.payload != NULL && dispatch_msg.payload_destroy != NULL) {
        dispatch_msg.payload_destroy(dispatch_msg.payload);
        dispatch_msg.payload = NULL;
      }
      cbor_decref(&wire_msg);
      break;
    }
    case NETWORK_QUIC_CONNECTED: {
      // New QUIC connection — add to pending (salutation deferred via actor message)
      quic_connected_payload_t* quic_conn = (quic_connected_payload_t*)msg->payload;
      if (quic_conn != NULL) {
        pending_quic_add(network, quic_conn->connection, quic_conn->stream, &quic_conn->peer_addr);
      }
      break;
    }
    case NETWORK_QUIC_DISCONNECTED: {
      quic_connected_payload_t* quic_conn = (quic_connected_payload_t*)msg->payload;
      if (quic_conn != NULL) {
        // Check if this is an authenticated peer
        peer_connection_t* peer = connection_manager_lookup_by_quic(
            &network->conn_mgr, quic_conn->connection);
        if (peer != NULL) {
          peer->connected = false;
#ifdef HAS_MSQUIC
          peer->quic_connection = NULL;
          peer->quic_stream = NULL;
#endif
          connection_manager_remove(&network->conn_mgr, &peer->remote_node_id);
        } else {
          // Unauthenticated — remove from pending list
          pending_quic_t* pending = pending_quic_remove(network, quic_conn->connection);
          if (pending != NULL) {
            free(pending);
          }
        }
#ifdef HAS_MSQUIC
        // ConnectionClose must be called after SHUTDOWN_COMPLETE to release
        // the HQUIC connection handle. We defer it from the msquic callback
        // to here so the handle stays valid for peer lookup above.
        if (quic_conn->connection != NULL && network->msquic != NULL) {
          network->msquic->ConnectionClose(quic_conn->connection);
        }
#endif
      }
      break;
    }
    case NETWORK_SHUTDOWN_CONNECTIONS: {
      network_shutdown_payload_t* payload = (network_shutdown_payload_t*)msg->payload;
#ifdef HAS_MSQUIC
      if (network->msquic != NULL) {
        /* We're on the network actor — the sole owner of conn_mgr + the HQUIC
           lifecycle. ConnectionShutdown is safe here because
           connection_manager_remove + ConnectionClose also run on this actor
           (in the NETWORK_QUIC_DISCONNECTED handler). The subsequent
           SHUTDOWN_COMPLETE callbacks will actor_send NETWORK_QUIC_DISCONNECT
           back to this actor's mailbox and be processed as the worker drains. */
        for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
          peer_connection_t* peer = network->conn_mgr.peers[index];
          if (peer != NULL && peer->quic_connection != NULL) {
            network->msquic->ConnectionShutdown(
                peer->quic_connection,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
            peer->quic_connection = NULL;
            peer->quic_stream = NULL;
          }
        }
      }
#endif
      /* Signal the main thread that the shutdown loop is done. The
         SHUTDOWN_COMPLETE -> remove -> ConnectionClose sequence continues
         on this actor as the worker drains its mailbox; the main thread
         proceeds to phase 6 and the workers join in phase 7. The payload
         is owned (and freed) by the main thread after this wait. */
      if (payload != NULL && payload->lock != NULL && payload->done != NULL) {
        platform_mutex_lock(payload->lock);
        atomic_store(&payload->done_flag, true);
        platform_condvar_signal(payload->done);
        platform_mutex_unlock(payload->lock);
      }
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
        // Build store_block_state for local block announcement
        store_block_state_t state;
        memset(&state, 0, sizeof(state));
        uint64_t now_ts = (uint64_t)time(NULL) * 1000;
        state.message_id = now_ts;
        memcpy(state.block_hash, payload->hash->data, 32);
        state.block_fib = payload->fib;
        state.replicas_needed = STORE_BLOCK_FORWARD_FANOUT;
        state.max_hops = STORE_BLOCK_MAX_HOPS;
        state.visited_count = 0;
        memcpy(&state.path[0], &network->authority->local_id, sizeof(node_id_t));
        state.path_len = 1;
        state.start_time_ms = now_ts;
        state.carry_data = 0;

        float local_capacity = atomic_load(&network->authority->capacity);
        node_phase_e local_phase = atomic_load(&network->authority->phase);

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

        if (result == STORE_BLOCK_ACCEPTED || result == STORE_BLOCK_FORWARDING) {
          for (size_t hop_idx = 0; hop_idx < next_hop_count; hop_idx++) {
            peer_connection_t* peer = connection_manager_lookup(
                &network->conn_mgr, &next_hops[hop_idx]->id);
            if (peer != NULL) {
              wire_store_block_t forward;
              memset(&forward, 0, sizeof(forward));
              forward.message_id = state.message_id;
              memcpy(forward.block_hash, state.block_hash, 32);
              forward.block_fib = state.block_fib;
              forward.replicas_needed = state.replicas_needed;
              forward.max_hops = state.max_hops;
              memcpy(forward.visited_bloom, state.visited_bloom, STORE_BLOCK_MAX_VISITED_BLOOM);
              forward.visited_count = state.visited_count;
              memcpy(forward.path, state.path, state.path_len * sizeof(node_id_t));
              forward.path_len = state.path_len;
              forward.start_time = state.start_time_ms;
              forward.carry_data = 0;

              cbor_item_t* cbor = wire_store_block_encode(&forward);
              conn_state_send(network, peer, cbor);
              cbor_decref(&cbor);
            }
          }
        }

        // Notify the stream actor if a reply_to was provided
        if (payload->reply_to != NULL) {
          network_store_block_result_payload_t* result_payload =
              get_clear_memory(sizeof(network_store_block_result_payload_t));
          if (result_payload != NULL) {
            result_payload->accepted = (result == STORE_BLOCK_ACCEPTED) ? 1 : 0;
            result_payload->replicas = (result == STORE_BLOCK_ACCEPTED) ? (uint32_t)next_hop_count : 0;
            result_payload->hash = REFERENCE(payload->hash, buffer_t);
            result_payload->reply_to = NULL;
            message_t result_msg = {0};
            result_msg.type = NETWORK_STORE_BLOCK_RESULT;
            result_msg.payload = result_payload;
            result_msg.payload_destroy = free;
            actor_send(payload->reply_to, &result_msg);
          }
        }
      }
      break;
    }
    case NETWORK_STORE_BLOCK_RESULT: {
      // Network returns StoreBlock result to stream
      // Handled by stream actor — dispatch routes to reply_to
      break;
    }
    case NETWORK_LOCAL_CLOSEST_NODES: {
      network_handle_local_closest_nodes(network, msg);
      break;
    }
    case NETWORK_LOCAL_FIND_NODE: {
      network_local_find_node_payload_t* payload =
          (network_local_find_node_payload_t*)msg->payload;
      if (payload == NULL) break;

      /* Build a FindNode wire message and send to each connected peer */
      wire_find_node_t find;
      memset(&find, 0, sizeof(find));
      find.message_id = (uint64_t)time(NULL) ^ ((uint64_t)rand() << 32);
      memcpy(&find.sender_id, &network->authority->local_id, sizeof(node_id_t));
      memcpy(&find.target_id, &payload->target_id, sizeof(node_id_t));

      for (size_t peer_idx = 0; peer_idx < network->conn_mgr.peer_count; peer_idx++) {
        peer_connection_t* peer = network->conn_mgr.peers[peer_idx];
        if (peer != NULL && peer->connected) {
          cbor_item_t* cbor = wire_find_node_encode(&find);
          conn_state_send(network, peer, cbor);
          cbor_decref(&cbor);
        }
      }
      break;
    }
    case NETWORK_CLOSEST_NODES_RESULT: {
      // Network returns ClosestNodes result to stream
      // Handled by stream actor — dispatch routes to reply_to
      break;
    }
    case NETWORK_RELAY_RECEIVED: {
      // Message received via relay — decode CBOR wire message and re-dispatch
      wire_relay_received_t* relay_payload = (wire_relay_received_t*)msg->payload;
      if (relay_payload == NULL || relay_payload->payload == NULL || relay_payload->payload_len == 0) break;

      struct cbor_load_result load_result;
      cbor_item_t* wire_msg = cbor_load(relay_payload->payload, relay_payload->payload_len, &load_result);
      if (wire_msg == NULL) {
        log_error("RELAY_RECEIVED: cbor_load failed, error=%d read=%zu",
                  load_result.error.code, load_result.read);
        break;
      }

      if (cbor_isa_array(wire_msg) && cbor_array_size(wire_msg) >= 1) {
        // Extract sender_id from wire message and add to connection_manager
        // and ring set if not already known — relay peers must be reachable
        // for find_block_execute routing.
        node_id_t sender_id;
        memset(&sender_id, 0, sizeof(node_id_t));
        if (wire_extract_sender_id(wire_msg, &sender_id) == 0) {
          peer_connection_t* existing = connection_manager_lookup(&network->conn_mgr, &sender_id);
          if (existing == NULL) {
            existing = connection_manager_add(&network->conn_mgr, &sender_id, NULL, network->pool);
          }
          // Store the relay endpoint ID so we can route messages back
          if (existing != NULL && relay_payload->src_endpoint_id != 0) {
            existing->relay_endpoint_id = relay_payload->src_endpoint_id;
          }
          // Also ensure the sender is in the ring set for routing
          net_node_t* ring_node = ring_set_find_by_id(network->rings, &sender_id);
          if (ring_node == NULL) {
            net_node_t* node = net_node_create(&sender_id, 0, 0);
            if (node != NULL) {
              node->weight = FIND_BLOCK_MIN_WEIGHT;
              ring_set_insert(network->rings, node, 0);
            }
          }
        }

        uint8_t type = wire_get_type(wire_msg);
        message_t dispatch_msg;
        memset(&dispatch_msg, 0, sizeof(dispatch_msg));
        dispatch_msg.type = type;
        dispatch_msg.payload_destroy = free;
        switch (type) {
          case WIRE_PING: {
            wire_ping_t* payload = get_clear_memory(sizeof(wire_ping_t));
            if (payload != NULL) {
              if (wire_ping_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_PING_RESPONSE: {
            wire_ping_response_t* payload = get_clear_memory(sizeof(wire_ping_response_t));
            if (payload != NULL) {
              if (wire_ping_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_PING_CAPACITY: {
            wire_ping_capacity_t* payload = get_clear_memory(sizeof(wire_ping_capacity_t));
            if (payload != NULL) {
              if (wire_ping_capacity_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping_capacity(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_PING_CAPACITY_RESPONSE: {
            wire_ping_capacity_response_t* payload = get_clear_memory(sizeof(wire_ping_capacity_response_t));
            if (payload != NULL) {
              if (wire_ping_capacity_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping_capacity_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_PING_BLOCK: {
            wire_ping_block_t* payload = get_clear_memory(sizeof(wire_ping_block_t));
            if (payload != NULL) {
              if (wire_ping_block_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping_block(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_PING_BLOCK_RESPONSE: {
            wire_ping_block_response_t* payload = get_clear_memory(sizeof(wire_ping_block_response_t));
            if (payload != NULL) {
              if (wire_ping_block_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_ping_block_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_FIND_BLOCK: {
            wire_find_block_t* payload = get_clear_memory(sizeof(wire_find_block_t));
            if (payload != NULL) {
              if (wire_find_block_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                dispatch_msg.payload_destroy = free;
                network_handle_find_block(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_FIND_BLOCK_RESPONSE: {
            wire_find_block_response_t* payload = get_clear_memory(sizeof(wire_find_block_response_t));
            if (payload != NULL) {
              if (wire_find_block_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                dispatch_msg.payload_destroy = (void (*)(void*))wire_find_block_response_destroy;
                network_handle_find_block_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_FIND_NODE: {
            wire_find_node_t* payload = get_clear_memory(sizeof(wire_find_node_t));
            if (payload != NULL) {
              if (wire_find_node_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_find_node(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_FIND_NODE_RESPONSE: {
            wire_find_node_response_t* payload = get_clear_memory(sizeof(wire_find_node_response_t));
            if (payload != NULL) {
              if (wire_find_node_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_find_node_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_STORE_BLOCK: {
            wire_store_block_t* payload = get_clear_memory(sizeof(wire_store_block_t));
            if (payload != NULL) {
              if (wire_store_block_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                dispatch_msg.payload_destroy = (void (*)(void*))wire_store_block_destroy;
                network_handle_store_block(network, &dispatch_msg);
              } else {
                wire_store_block_destroy(payload);
              }
            }
            break;
          }
          case WIRE_STORE_BLOCK_RESPONSE: {
            wire_store_block_response_t* payload = get_clear_memory(sizeof(wire_store_block_response_t));
            if (payload != NULL) {
              if (wire_store_block_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_store_block_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_SEEKING_BLOCKS: {
            wire_seeking_blocks_t* payload = get_clear_memory(sizeof(wire_seeking_blocks_t));
            if (payload != NULL) {
              if (wire_seeking_blocks_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                dispatch_msg.payload_destroy = (void (*)(void*))wire_seeking_blocks_destroy;
                network_handle_seeking_blocks(network, &dispatch_msg);
              } else {
                wire_seeking_blocks_destroy(payload);
              }
            }
            break;
          }
          case WIRE_RANK_BLOCK: {
            wire_rank_block_t* payload = get_clear_memory(sizeof(wire_rank_block_t));
            if (payload != NULL) {
              if (wire_rank_block_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_rank_block(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_RECALL_BLOCK: {
            wire_recall_block_t* payload = get_clear_memory(sizeof(wire_recall_block_t));
            if (payload != NULL) {
              if (wire_recall_block_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_recall_block(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_SEEKING_BLOCKS_RESPONSE: {
            wire_seeking_blocks_response_t* payload = get_clear_memory(sizeof(wire_seeking_blocks_response_t));
            if (payload != NULL) {
              if (wire_seeking_blocks_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_seeking_blocks_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_RECALL_ACCEPT: {
            wire_recall_accept_t* payload = get_clear_memory(sizeof(wire_recall_accept_t));
            if (payload != NULL) {
              if (wire_recall_accept_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                dispatch_msg.payload_destroy = (void (*)(void*))wire_recall_accept_destroy;
                network_handle_recall_accept(network, &dispatch_msg);
              } else {
                wire_recall_accept_destroy(payload);
              }
            }
            break;
          }
          case WIRE_RECALL_DECLINE: {
            wire_recall_decline_t* payload = get_clear_memory(sizeof(wire_recall_decline_t));
            if (payload != NULL) {
              if (wire_recall_decline_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_recall_decline(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_RATE_LIMITED: {
            wire_rate_limited_t* payload = get_clear_memory(sizeof(wire_rate_limited_t));
            if (payload != NULL) {
              if (wire_rate_limited_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_rate_limited(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_GOSSIP: {
            wire_gossip_t* payload = get_clear_memory(sizeof(wire_gossip_t));
            if (payload != NULL) {
              if (wire_gossip_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_gossip_received(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_GOSSIP_PULL: {
            wire_gossip_pull_t* payload = get_clear_memory(sizeof(wire_gossip_pull_t));
            if (payload != NULL) {
              if (wire_gossip_pull_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_gossip_pull_received(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_CLOSEST_NODES: {
            wire_closest_nodes_t* payload = get_clear_memory(sizeof(wire_closest_nodes_t));
            if (payload != NULL) {
              if (wire_closest_nodes_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_closest_nodes(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_CLOSEST_NODES_RESPONSE: {
            wire_closest_nodes_response_t* payload = get_clear_memory(sizeof(wire_closest_nodes_response_t));
            if (payload != NULL) {
              if (wire_closest_nodes_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_closest_nodes_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_MEASURE_NODES: {
            wire_measure_nodes_t* payload = get_clear_memory(sizeof(wire_measure_nodes_t));
            if (payload != NULL) {
              if (wire_measure_nodes_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_measure_nodes(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_MEASURE_NODES_RESPONSE: {
            wire_measure_nodes_response_t* payload = get_clear_memory(sizeof(wire_measure_nodes_response_t));
            if (payload != NULL) {
              if (wire_measure_nodes_response_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_measure_nodes_response(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          case WIRE_CLOSEST_NODES_PROGRESS: {
            wire_closest_nodes_progress_t* payload = get_clear_memory(sizeof(wire_closest_nodes_progress_t));
            if (payload != NULL) {
              if (wire_closest_nodes_progress_decode(wire_msg, payload) == 0) {
                dispatch_msg.payload = payload;
                network_handle_closest_nodes_progress(network, &dispatch_msg);
              } else {
                free(payload);
              }
            }
            break;
          }
          default:
            break;
        }
        /* The synchronous dispatch bypasses actor_run, so the framework does
           not free the payload. Handlers that CONSUME the payload set
           dispatch_msg.payload = NULL; respect that. See audit #2. */
        if (dispatch_msg.payload != NULL && dispatch_msg.payload_destroy != NULL) {
          dispatch_msg.payload_destroy(dispatch_msg.payload);
          dispatch_msg.payload = NULL;
        }
      }
      cbor_decref(&wire_msg);
      break;
    }
    case CONN_STATE_DIRECT_CONNECTED: {
      peer_connection_t* peer = (peer_connection_t*)msg->payload;
      if (peer != NULL) {
        conn_state_on_direct_connected(peer);
      }
      break;
    }
    case CONN_STATE_DIRECT_FAILED: {
      peer_connection_t* peer = (peer_connection_t*)msg->payload;
      if (peer != NULL) {
        conn_state_on_direct_failed(peer);
      }
      break;
    }
    case CONN_STATE_TRY_DIRECT: {
      peer_connection_t* peer = (peer_connection_t*)msg->payload;
      if (peer != NULL) {
        conn_state_upgrade_to_direct(peer);
      }
      break;
    }
    case RELAY_CLIENT_SEND: {
      // Forward relay messages from conn_state_send to the relay client actor.
      // Transfer payload ownership to the relay client by nulling out
      // msg's payload/payload_destroy so network_dispatch doesn't double-free.
      if (network->relay != NULL) {
        message_t relay_msg;
        memset(&relay_msg, 0, sizeof(relay_msg));
        relay_msg.type = msg->type;
        relay_msg.payload = msg->payload;
        relay_msg.payload_destroy = msg->payload_destroy;
        msg->payload = NULL;
        msg->payload_destroy = NULL;
        actor_send(&network->relay->actor, &relay_msg);
      }
      break;
    }
    case CACHE_PUT_RESULT: {
      cache_put_result_payload_t* result = (cache_put_result_payload_t*)msg->payload;
      if (result->result == CACHE_PUT_ERROR || result->result == CACHE_PUT_FULL) {
        /* Best-effort cache store failed. The block was already delivered
         * directly to the requesting stream via NETWORK_FIND_BLOCK_RESULT,
         * so this failure does not affect the current GET. No action needed. */
      }
      /* CACHE_PUT_NEW / CACHE_PUT_EXISTS: the block is in the cache for
       * future GETs. Network announce for CACHE_PUT_NEW is handled by the
       * writable_off_stream path during PUT, not here. */
      break;
    }
    default:
      break;
  }
}