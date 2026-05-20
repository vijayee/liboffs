//
// Created by victor on 5/14/25.
//

#include "node.h"
#include "../BlockCache/block_cache.h"
#include "../ClientAPI/HTTP/http_server.h"
#include "../Network/network.h"
#include "../Util/allocator.h"
#include <stdlib.h>

offs_node_t* offs_node_create(config_t* config, authority_t* authority) {
  offs_node_t* node = get_clear_memory(sizeof(offs_node_t));
  node->config = config;
  node->authority = authority;
  node->running = ATOMIC_VAR_INIT(0);

  node->scheduler = scheduler_pool_create(4);
  if (node->scheduler == NULL) {
    free(node);
    return NULL;
  }

  node->timer = timer_actor_create();
  if (node->timer == NULL) {
    scheduler_pool_destroy(node->scheduler);
    free(node);
    return NULL;
  }

  return node;
}

int offs_node_start(offs_node_t* node) {
  if (node == NULL) return -1;

  scheduler_pool_start(node->scheduler);

  node->block_cache = block_cache_create(
    *node->config,
    "sections",
    standard,
    node->timer,
    node->scheduler,
    node->authority,
    node->config->max_capacity_bytes
  );
  if (node->block_cache == NULL) return -1;

  node->network = network_create(node->authority, node->block_cache,
                                 node->timer, node->scheduler);
  if (node->network == NULL) {
    block_cache_destroy(node->block_cache);
    return -1;
  }

  // Load persisted peer state (Hebbian weights, ring nodes) on startup
  if (node->authority != NULL) {
    authority_load_peers(node->authority, node->network);
  }

  ATOMIC_STORE(&node->running, 1);
  return 0;
}

void offs_node_stop(offs_node_t* node) {
  if (node == NULL) return;
  ATOMIC_STORE(&node->running, 0);

  if (node->http_server != NULL) {
    http_server_stop(node->http_server);
  }

  if (node->network != NULL) {
    ATOMIC_STORE(&node->network->running, 0);
  }

  scheduler_pool_stop(node->scheduler);

  // Persist peer state (Hebbian weights, ring nodes) before shutdown
  if (node->authority != NULL && node->network != NULL) {
    authority_save_peers(node->authority, node->network);
  }
}

void offs_node_stop_http(offs_node_t* node) {
  if (node == NULL || node->http_server == NULL) return;
  http_server_stop(node->http_server);
}

void offs_node_stop_network(offs_node_t* node) {
  if (node == NULL || node->network == NULL) return;
  ATOMIC_STORE(&node->network->running, 0);
}

void offs_node_destroy(offs_node_t* node) {
  if (node == NULL) return;

  if (node->network != NULL) {
    network_destroy(node->network);
  }

  if (node->block_cache != NULL) {
    block_cache_destroy(node->block_cache);
  }

  if (node->http_server != NULL) {
    http_server_destroy(node->http_server);
  }

  if (node->timer != NULL) {
    timer_actor_destroy(node->timer);
  }

  if (node->scheduler != NULL) {
    scheduler_pool_destroy(node->scheduler);
  }

  free(node);
}