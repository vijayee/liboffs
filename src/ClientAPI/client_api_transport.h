#ifndef OFFS_CLIENT_API_TRANSPORT_H
#define OFFS_CLIENT_API_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
  CLIENT_API_TRANSPORT_UNIX,
  CLIENT_API_TRANSPORT_TCP,
  CLIENT_API_TRANSPORT_WS,
  CLIENT_API_TRANSPORT_WT
} client_api_transport_type_e;

typedef struct {
  const char* host;
  uint16_t port;
  const char* socket_path;
  const char* cert_path;
  const char* key_path;
  size_t max_connections;
} client_api_transport_config_t;

#endif