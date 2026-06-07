//
// Created by victor on 5/14/25.
//

#include "node.h"
#include <time.h>
#include "../BlockCache/block_cache.h"
#include "../ClientAPI/HTTP/http_server.h"
#include "../Configuration/config_pending.h"
#include "../Network/network.h"
#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <stdbool.h>
#include <stdlib.h>

offs_node_t* offs_node_create(config_t* config, authority_t* authority) {
  offs_node_t* node = get_clear_memory(sizeof(offs_node_t));
  node->config = config_deep_copy(config);
  node->authority = authority;
  node->running = ATOMIC_VAR_INIT(0);
  node->draining = ATOMIC_VAR_INIT(0);

  node->scheduler = scheduler_pool_create(config->scheduler_thread_count);
  if (node->scheduler == NULL) {
    config_free(node->config);
    free(node);
    return NULL;
  }

  node->timer = timer_actor_create(node->scheduler);
  if (node->timer == NULL) {
    scheduler_pool_destroy(node->scheduler);
    config_free(node->config);
    free(node);
    return NULL;
  }

  return node;
}

int offs_node_start(offs_node_t* node) {
  if (node == NULL) return -1;

  log_info("offs_node_start: node starting");

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  node->start_time_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;

  if (node->config == NULL || config_validate(node->config) != 0) {
    log_error("offs_node_start: invalid configuration");
    return -1;
  }

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
                                 node->timer, node->scheduler,
                                 node->config);
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

static uint64_t _shutdown_deadline_ms(offs_node_t* node) {
  if (node->config->shutdown_timeout_ms == 0) return UINT64_MAX;
  return platform_monotonic_ns() / 1000000 + node->config->shutdown_timeout_ms;
}

static bool _shutdown_deadline_exceeded(uint64_t deadline) {
  return platform_monotonic_ns() / 1000000 > deadline;
}

void offs_node_stop(offs_node_t* node) {
  if (node == NULL) return;

  log_info("offs_node_stop: graceful shutdown begin");

  uint64_t deadline = _shutdown_deadline_ms(node);

  /* Phase 1: Stop accepting new work. */
  ATOMIC_STORE(&node->draining, 1);
  ATOMIC_STORE(&node->running, 0);

  if (node->http_server != NULL) {
    http_server_drain(node->http_server);
  }

  /* Phase 1b: Stop timer actor so recurring timers stop firing. */
  timer_actor_stop(node->timer);

  /* Drain any in-flight timer completion messages from the scheduler. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    scheduler_pool_wait_for_idle(node->scheduler);
  }

  /* Phase 2: Notify peers — stop network event loop.
     QUIC connections will be closed with CONNECTION_CLOSE frames
     during network_destroy in offs_node_destroy(). */
  if (node->network != NULL) {
    ATOMIC_STORE(&node->network->running, 0);
  }

  /* Phase 3: Drain in-flight HTTP requests. */
  if (node->http_server != NULL && !_shutdown_deadline_exceeded(deadline)) {
    while (ATOMIC_LOAD(&node->http_server->active_connections) > 0) {
      if (_shutdown_deadline_exceeded(deadline)) break;
      platform_sleep_ms(50);
    }
  }

  /* Phase 4: Drain actor work queue. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    scheduler_pool_wait_for_idle(node->scheduler);
  }

  /* Phase 5: Close P2P connections.
     Actors are drained — no new messages can be generated.
     Shutdown each peer's QUIC connection with a CONNECTION_CLOSE frame
     (non-silent) so peers know this is an intentional departure, then
     disconnect the relay client. */
  if (node->network != NULL) {
    network_shutdown_connections(node->network);
  }

  /* Phase 6: Flush index/WAL and persist peer state. */
  if (!_shutdown_deadline_exceeded(deadline)) {
    if (node->block_cache != NULL) {
      block_cache_sync(node->block_cache);
    }
    if (node->authority != NULL && node->network != NULL) {
      authority_save_peers(node->authority, node->network);
    }
  }

  /* Phase 7: Stop scheduler — join all worker threads. */
  scheduler_pool_stop(node->scheduler);
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

  config_free(node->config);
  free(node);
}

void offs_node_restart(offs_node_t* node, const char* data_dir) {
  if (node == NULL || data_dir == NULL) return;

  log_info("offs_node_restart: beginning graceful restart");

  /* Phase 1: Stop everything */
  offs_node_stop(node);

  /* Phase 2: Tear down subsystems */
  if (node->network != NULL) {
    network_destroy(node->network);
    node->network = NULL;
  }
  if (node->block_cache != NULL) {
    block_cache_destroy(node->block_cache);
    node->block_cache = NULL;
  }
  if (node->http_server != NULL) {
    http_server_destroy(node->http_server);
    node->http_server = NULL;
  }
  if (node->timer != NULL) {
    timer_actor_destroy(node->timer);
    node->timer = NULL;
  }
  if (node->scheduler != NULL) {
    scheduler_pool_destroy(node->scheduler);
    node->scheduler = NULL;
  }

  /* Phase 3: Load pending config */
  config_t* new_config = config_pending_load(data_dir);
  if (new_config == NULL) {
    log_error("offs_node_restart: failed to load pending config, restarting with current config");
    new_config = config_deep_copy(node->config);
  }

  config_free(node->config);
  node->config = new_config;

  /* Phase 4: Re-create scheduler and timer */
  node->scheduler = scheduler_pool_create(new_config->scheduler_thread_count);
  if (node->scheduler == NULL) {
    log_error("offs_node_restart: failed to create scheduler pool");
    return;
  }

  node->timer = timer_actor_create(node->scheduler);
  if (node->timer == NULL) {
    log_error("offs_node_restart: failed to create timer actor");
    scheduler_pool_destroy(node->scheduler);
    node->scheduler = NULL;
    return;
  }

  /* Phase 5: Start with new config */
  if (offs_node_start(node) != 0) {
    log_error("offs_node_restart: offs_node_start failed");
    return;
  }

  /* Phase 6: Mark pending config as applied */
  config_pending_mark_applied(data_dir);

  log_info("offs_node_restart: restart complete");
}