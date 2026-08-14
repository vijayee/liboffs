//
// Created by victor on 5/8/26.
//

#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/off_routes.h"
#include "ClientAPI/HTTP/block_routes.h"
#include "ClientAPI/HTTP/cors.h"
#include "ClientAPI/Unix/unix_transport.h"
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include "ClientAPI/WS/ws_transport.h"
#include "ClientAPI/WT/wt_transport.h"
#include "ClientAPI/WT/webtransport_h3.h"
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
#include "Network/peer_verify.h"
#include "Util/path_join.h"
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
  fprintf(stderr, "  --ws-port <port>    WebSocket port (0 to disable, default: 0)\n");
  fprintf(stderr, "  --wt-port <port>    WebTransport (custom QUIC) port (0 to disable, default: 0)\n");
  fprintf(stderr, "  --wt-h3-port <port> HTTP/3 WebTransport port (0 to disable, default: 0)\n");
  fprintf(stderr, "  --cert <path>        TLS certificate for WebSocket/WebTransport\n");
  fprintf(stderr, "  --key <path>         TLS private key for WebSocket/WebTransport\n");
  fprintf(stderr, "  --ca-cert <path>    CA certificate for client validation\n");
  fprintf(stderr, "  --relay-url <url>    Relay server URL (host:port or offs://host:port)\n");
  fprintf(stderr, "  --allow-secure       Require CA validation for TLS transports\n");
  fprintf(stderr, "  --help               Show this help\n");
}

int main(int argc, char** argv) {
  platform_thread_setup_stack();

  const char* host = "0.0.0.0";
  uint16_t port = 23402;
  const char* unix_path = NULL;
  const char* cache_dir = "./offs_cache";
  int worker_count = 4;
  uint16_t ws_port = 0;
  uint16_t wt_port = 0;
  uint16_t wt_h3_port = 0;
  const char* cert_path = NULL;
  const char* key_path = NULL;
  const char* ca_path = NULL;
  const char* relay_url = NULL;
  const char* node_cert_path = NULL;
  const char* node_key_path = NULL;
  uint16_t quic_port = 0;
  uint8_t allow_secure = 0;

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
    } else if (strcmp(argv[i], "--ws-port") == 0 && i + 1 < argc) {
      ws_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--wt-port") == 0 && i + 1 < argc) {
      wt_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--wt-h3-port") == 0 && i + 1 < argc) {
      wt_h3_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
      cert_path = argv[++i];
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(argv[i], "--ca-cert") == 0 && i + 1 < argc) {
      ca_path = argv[++i];
    } else if (strcmp(argv[i], "--relay-url") == 0 && i + 1 < argc) {
      relay_url = argv[++i];
    } else if (strcmp(argv[i], "--quic-port") == 0 && i + 1 < argc) {
      quic_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--node-cert") == 0 && i + 1 < argc) {
      node_cert_path = argv[++i];
    } else if (strcmp(argv[i], "--node-key") == 0 && i + 1 < argc) {
      node_key_path = argv[++i];
    } else if (strcmp(argv[i], "--allow-secure") == 0) {
      allow_secure = 1;
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
  printf("  HTTP Port: %u\n", port);
  printf("  WebSocket Port: %u\n", ws_port);
  printf("  WebTransport Port: %u\n", wt_port);
  printf("  WebTransport H3 Port: %u\n", wt_h3_port);
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
  authority->peer_store_path = path_join(cache_dir, "peer_store.cbor");
  if (node_cert_path != NULL) authority->node_cert_path = strdup(node_cert_path);
  if (node_key_path != NULL) authority->node_key_path = strdup(node_key_path);
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

  off_routes_register(server, pool, bc, ofd_cache, tc, network, NULL, NULL, NULL);
  block_routes_register(server, pool, bc, NULL, NULL);
  health_routes_register(server, &health_ctx);
  peer_routes_register(server, &node_obj, &config, NULL);
  config_routes_register(server, &node_obj, &config, ".", NULL, NULL);

  ws_transport_t* ws_transport = NULL;
  if (ws_port != 0) {
    ws_transport = ws_transport_create(pool, bc, ofd_cache, tc, host, ws_port,
                                       cert_path, key_path, 0, NULL, &health_ctx);
    if (ws_transport == NULL) {
      fprintf(stderr, "Failed to create WebSocket transport on %s:%u\n", host, ws_port);
      authority_save_peers(authority, network);
      network_destroy(network);
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
  }

  wt_transport_t* wt_transport = NULL;
  if (wt_port != 0) {
    peer_verify_ctx_t* peer_verify = NULL;
    if (ca_path != NULL) {
      peer_verify = peer_verify_ctx_create_from_pem_file(ca_path);
      if (peer_verify == NULL) {
        fprintf(stderr, "Failed to load CA certificate from %s\n", ca_path);
        if (ws_transport != NULL) {
          ws_transport_destroy(ws_transport);
        }
        authority_save_peers(authority, network);
        network_destroy(network);
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
    }

    wt_transport = wt_transport_create(pool, bc, ofd_cache, tc, host, wt_port,
                                       cert_path, key_path, ca_path,
                                       allow_secure != 0, 0, NULL, &health_ctx);
    if (wt_transport == NULL) {
      fprintf(stderr, "Failed to create WebTransport transport on %s:%u\n", host, wt_port);
      if (peer_verify != NULL) {
        peer_verify_ctx_destroy(peer_verify);
      }
      if (ws_transport != NULL) {
        ws_transport_destroy(ws_transport);
      }
      authority_save_peers(authority, network);
      network_destroy(network);
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

    if (peer_verify != NULL) {
      peer_verify_ctx_destroy(peer_verify);
    }
  }

  webtransport_h3_t* wt_h3_transport = NULL;
  if (wt_h3_port != 0) {
    wt_h3_transport = webtransport_h3_create(pool, bc, ofd_cache, tc, host, wt_h3_port,
                                                cert_path, key_path, ca_path,
                                                allow_secure != 0, NULL, &health_ctx);
    if (wt_h3_transport == NULL) {
      fprintf(stderr, "Failed to create WebTransport H3 transport on %s:%u\n", host, wt_h3_port);
      if (wt_transport != NULL) {
        wt_transport_destroy(wt_transport);
      }
      if (ws_transport != NULL) {
        ws_transport_destroy(ws_transport);
      }
      authority_save_peers(authority, network);
      network_destroy(network);
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
  }

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
    /* Wire config management (show/set/reload) onto the Unix transport so
       offs config * over the local socket mirrors the HTTP /config routes.
       The peer/friend handlers are wired via the same config_node borrow
       (unix_connection_create initializes peer_ctx from config_node). */
    unix_transport_set_config_ctx(unix_transport, &node_obj, ".", NULL, NULL);
  }

#ifndef _WIN32
  struct sigaction signal_action;
  memset(&signal_action, 0, sizeof(signal_action));
  signal_action.sa_handler = _signal_handler;
  sigaction(SIGINT, &signal_action, NULL);
  sigaction(SIGTERM, &signal_action, NULL);
  signal(SIGPIPE, SIG_IGN);
#else
  signal(SIGINT, _signal_handler);
  signal(SIGTERM, _signal_handler);
  signal(SIGPIPE, SIG_IGN);
#endif

  http_server_listen(server);
  if (ws_transport != NULL) {
    ws_transport_start(ws_transport);
    printf("Listening on ws://%s:%u\n", host, ws_port);
    if (cert_path != NULL && key_path != NULL) {
      printf("Listening on wss://%s:%u\n", host, ws_port);
    }
  }
  if (wt_transport != NULL) {
    wt_transport_start(wt_transport);
    printf("Listening on wt://%s:%u\n", host, wt_port);
    if (cert_path != NULL && key_path != NULL) {
      printf("Listening on wts://%s:%u\n", host, wt_port);
    }
  }
  if (wt_h3_transport != NULL) {
    webtransport_h3_start(wt_h3_transport);
    printf("Listening on wt://%s:%u (HTTP/3)\n", host, wt_h3_port);
    if (cert_path != NULL && key_path != NULL) {
      printf("Listening on wts://%s:%u (HTTP/3)\n", host, wt_h3_port);
    }
  }
  if (unix_transport != NULL) {
    unix_transport_start(unix_transport);
    printf("Listening on unix://%s\n", unix_path);
  }

  /* Start the QUIC/P2P listener so the node can accept direct peer
     connections. Without this, peer_info_from_node has no HOST candidates
     and incoming direct QUIC connections cannot be accepted. */
  if (quic_port > 0 && network != NULL && network->quic_listener != NULL) {
    if (quic_listener_start(network->quic_listener, host, quic_port) == 0) {
      printf("Listening on quic://%s:%u\n", host, quic_port);
    } else {
      fprintf(stderr, "Warning: failed to start QUIC listener on %s:%u\n",
              host, quic_port);
    }
  }

  /* Connect to the relay server for NAT traversal and server-reflexive
     address discovery. The relay_url is "host:port" (optionally "offs://"). */
  if (relay_url != NULL && network != NULL) {
    const char* url = relay_url;
    if (strncmp(url, "offs://", 7) == 0) url += 7;
    const char* colon = strrchr(url, ':');
    if (colon != NULL) {
      char* host_buf = strndup(url, (size_t)(colon - url));
      if (host_buf != NULL) {
        uint16_t relay_port = (uint16_t)atoi(colon + 1);
        if (relay_port > 0) {
          if (network_connect_relay(network, host_buf, relay_port) == 0) {
            printf("Connected to relay %s:%u\n", host_buf, relay_port);
          } else {
            fprintf(stderr, "Warning: failed to connect to relay %s:%u\n",
                    host_buf, relay_port);
          }
        }
        free(host_buf);
      }
    }
  }

  authority_load_peers(authority, network);
  network_start_connections(network);

  printf("Listening on http://%s:%u\n", host, port);
  printf("Press Ctrl+C to stop\n");

  while (!g_stop) {
    platform_sleep_ms(200);
  }

  if (unix_transport != NULL) {
    unix_transport_stop(unix_transport);
    unix_transport_destroy(unix_transport);
  }
  if (ws_transport != NULL) {
    ws_transport_stop(ws_transport);
    ws_transport_destroy(ws_transport);
  }
  if (wt_transport != NULL) {
    wt_transport_stop(wt_transport);
    wt_transport_destroy(wt_transport);
  }
  if (wt_h3_transport != NULL) {
    webtransport_h3_stop(wt_h3_transport);
    webtransport_h3_destroy(wt_h3_transport);
  }
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