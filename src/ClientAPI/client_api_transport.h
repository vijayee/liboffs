//
// Created by victor on 5/20/26.
//

#ifndef OFFS_CLIENT_API_TRANSPORT_H
#define OFFS_CLIENT_API_TRANSPORT_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Util/threadding.h"
#include <poll-dancer/poll-dancer.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
  CLIENT_API_TRANSPORT_UNIX,
  CLIENT_API_TRANSPORT_TCP,
  CLIENT_API_TRANSPORT_WS,
  CLIENT_API_TRANSPORT_WT
} client_api_transport_type_e;

// Forward declaration
typedef struct client_api_transport_t client_api_transport_t;

struct client_api_transport_t {
  const char* name;
  client_api_transport_type_e type;
  scheduler_pool_t* pool;
  PLATFORMTHREADTYPE thread;
  pd_loop_t* loop;
  ATOMIC(uint8_t) running;
  PLATFORMLOCKTYPE(destroy_lock);

  int (*start)(client_api_transport_t* self);
  int (*stop)(client_api_transport_t* self);
  int (*send)(client_api_transport_t* self, int client_id,
              const uint8_t* data, size_t len);
  void (*destroy)(client_api_transport_t* self);
};

// Transport configuration
typedef struct {
  const char* host;
  uint16_t port;
  const char* socket_path;    // for unix transport
  const char* cert_path;      // for TLS transports
  const char* key_path;       // for TLS transports
  size_t max_connections;
} client_api_transport_config_t;

#endif // OFFS_CLIENT_API_TRANSPORT_H