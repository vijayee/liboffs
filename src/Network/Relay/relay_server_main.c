//
// Created by victor on 5/16/26.
//

#include "relay_server.h"
#include "../../Scheduler/scheduler.h"
#include "../../Util/log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t _relay_running = 1;

static void _relay_signal_handler(int signum) {
  (void)signum;
  _relay_running = 0;
}

int main(int argc, char* argv[]) {
  uint16_t port = 14000;
  const char* host = NULL;

  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
      port = (uint16_t)atoi(argv[index + 1]);
      index++;
    } else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
      host = argv[index + 1];
      index++;
    } else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
      printf("Usage: meridian_relay [--port PORT] [--host HOST]\n");
      printf("  --port PORT   Listen port (default: 14000)\n");
      printf("  --host HOST   Bind host (default: all interfaces)\n");
      return 0;
    }
  }

  log_set_level(LOG_INFO);

  scheduler_pool_t* pool = scheduler_pool_create(4);
  if (pool == NULL) {
    log_error("relay: failed to create scheduler pool");
    return 1;
  }
  scheduler_pool_start(pool);

  relay_server_t* server = relay_server_create(pool);
  if (server == NULL) {
    log_error("relay: failed to create relay server (msquic not available?)");
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  if (relay_server_start(server, host, port) != 0) {
    log_error("relay: failed to start relay server on port %u", port);
    relay_server_destroy(server);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  printf("meridian_relay: started on port %u\n", port);

  struct sigaction signal_action;
  memset(&signal_action, 0, sizeof(signal_action));
  signal_action.sa_handler = _relay_signal_handler;
  sigaction(SIGINT, &signal_action, NULL);
  sigaction(SIGTERM, &signal_action, NULL);

  while (_relay_running) {
    struct timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;
    nanosleep(&timeout, NULL);
  }

  printf("meridian_relay: shutting down...\n");
  relay_server_stop(server);
  relay_server_destroy(server);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  printf("meridian_relay: stopped\n");
  return 0;
}