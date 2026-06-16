//
// Created by victor on 5/8/26.
//

#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/off_routes.h"
#include "ClientAPI/HTTP/block_routes.h"
#include "ClientAPI/HTTP/cors.h"
#ifndef _WIN32
  #include "ClientAPI/Unix/unix_transport.h"
#endif
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include "../../src/ClientAPI/HTTP/peer_routes.h"
#include "../../src/ClientAPI/HTTP/config_routes.h"
#include "../../src/Node/node.h"
#include "../../src/Network/authority.h"
#include "../../src/Network/network.h"
#include "OFFStreams/tuple_cache.h"
#include "BlockCache/block_cache.h"
#include "OFFStreams/ofd_cache.h"
#include "Scheduler/scheduler.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"
#include "Platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t g_stop = 0;

static void _signal_handler(int sig) {
  (void)sig;
  g_stop = 1;
}

static void _print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [options]\n", program);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --host <addr>         Bind address (default: 0.0.0.0)\n");
  fprintf(stderr, "  --port <port>         HTTP port (default: 23402)\n");
  fprintf(stderr, "  --unix <path>        Unix socket path (default: off)\n");
  fprintf(stderr, "  --cache-dir <dir>    Block cache directory (default: ./offs_cache)\n");
  fprintf(stderr, "  --workers <n>        Worker thread count (default: 4)\n");
  fprintf(stderr, "  --help               Show this help\n");
}

int main(int argc, char** argv) {
  platform_thread_setup_stack();

  const char* host = "0.0.0.0";
  uint16_t port = 23402;
  const char* unix_path = NULL;
  const char* cache_dir = "./offs_cache";
  int worker_count = 4;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
      unix_path = argv[++i];
    } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
      cache_dir = argv[++i];
    } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
      worker_count = atoi(argv[++i]);
      if (worker_count < 1) worker_count = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      _print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      _print_usage(argv[0]);
      return 1;
    }
  }

  printf("OFF System Server\n");
  printf("  Host: %s\n", host);
  printf("  Port: %u\n", port);
  printf("  Cache: %s\n", cache_dir);
  printf("  Workers: %d\n", worker_count);
  if (unix_path != NULL) {
    printf("  Unix: %s\n", unix_path);
  }

  scheduler_pool_t* pool = scheduler_pool_create(worker_count);
  scheduler_pool_start(pool);

  timer_actor_t* timer = timer_actor_create(pool);

  config_t config = config_default();
  block_cache_t* bc = block_cache_create(config, (char*)cache_dir, standard, timer, pool, NULL, 0);

  ofd_cache_t* ofd_cache = ofd_cache_create(pool, bc, 300000);
  tuple_cache_t* tc = tuple_cache_create(100, pool);

  http_server_t* server = http_server_create(pool, host, port);
  if (server == NULL) {
    fprintf(stderr, "Failed to create HTTP server on %s:%u\n", host, port);
    tuple_cache_destroy(tc);
    ofd_cache_destroy(ofd_cache);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  uint64_t server_start_ms = platform_monotonic_ns() / 1000000ULL;
  uint8_t running_val = 1;
  uint8_t draining_val = 0;

  health_context_t health_ctx;
  memset(&health_ctx, 0, sizeof(health_ctx));
  health_ctx.block_cache = bc;
  health_ctx.start_time_ms = &server_start_ms;
  health_ctx.running = &running_val;
  health_ctx.draining = &draining_val;

  authority_t* authority = authority_create(&config);
  authority_init_local_id(authority);

  network_t* network = network_create(authority, bc, timer, pool, &config);
  if (network == NULL) {
    fprintf(stderr, "Failed to create network\n");
    authority_destroy(authority);
    http_server_destroy(server);
    tuple_cache_destroy(tc);
    ofd_cache_destroy(ofd_cache);
    block_cache_destroy(bc);
    timer_actor_destroy(timer);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  offs_node_t node_obj;
  memset(&node_obj, 0, sizeof(node_obj));
  node_obj.config = &config;
  node_obj.authority = authority;
  node_obj.network = network;
  node_obj.block_cache = bc;
  node_obj.http_server = server;
  node_obj.scheduler = pool;
  node_obj.timer = timer;

  off_routes_register(server, pool, bc, ofd_cache, tc, NULL, NULL, NULL);
  block_routes_register(server, pool, bc, NULL, NULL);
  health_routes_register(server, &health_ctx);
  peer_routes_register(server, &node_obj, &config, NULL);
  config_routes_register(server, &node_obj, &config, ".");

#ifndef _WIN32
  unix_transport_t* unix_transport = NULL;
  if (unix_path != NULL) {
    unix_transport = unix_transport_create(pool, bc, ofd_cache, tc, unix_path, NULL, &health_ctx);
    if (unix_transport == NULL) {
      fprintf(stderr, "Failed to create Unix transport on %s\n", unix_path);
      authority_save_peers(authority, network);
      network_destroy(network);
      http_server_destroy(server);
      tuple_cache_destroy(tc);
      ofd_cache_destroy(ofd_cache);
      block_cache_destroy(bc);
      timer_actor_destroy(timer);
      scheduler_pool_stop(pool);
      scheduler_pool_destroy(pool);
      authority_destroy(authority);
      return 1;
    }
  }
#else
  if (unix_path != NULL) {
    fprintf(stderr, "Warning: --unix is not supported on Windows, ignoring %s\n", unix_path);
  }
#endif

#ifndef _WIN32
  struct sigaction signal_action;
  memset(&signal_action, 0, sizeof(signal_action));
  signal_action.sa_handler = _signal_handler;
  sigaction(SIGINT, &signal_action, NULL);
  sigaction(SIGTERM, &signal_action, NULL);
#else
  signal(SIGINT, _signal_handler);
  signal(SIGTERM, _signal_handler);
#endif

  http_server_listen(server);
#ifndef _WIN32
  if (unix_transport != NULL) {
    unix_transport_start(unix_transport);
    printf("Listening on unix://%s\n", unix_path);
  }
#endif

  authority_load_peers(authority, network);
  network_start_connections(network);

  printf("Listening on http://%s:%u\n", host, port);
  printf("Press Ctrl+C to stop\n");

  while (!g_stop) {
    platform_sleep_ms(200);
  }

#ifndef _WIN32
  if (unix_transport != NULL) {
    unix_transport_stop(unix_transport);
    unix_transport_destroy(unix_transport);
  }
#endif
  ATOMIC_STORE(&network->running, 0);
  network_shutdown_connections(network);
  http_server_stop(server);
  scheduler_pool_stop(pool);
  http_server_destroy(server);
  network_destroy(network);
  tuple_cache_destroy(tc);
  ofd_cache_destroy(ofd_cache);
  block_cache_destroy(bc);
  timer_actor_destroy(timer);
  scheduler_pool_destroy(pool);
  authority_destroy(authority);

  printf("Server stopped\n");
  return 0;
}