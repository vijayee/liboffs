//
// Created by victor on 5/27/26.
//

#ifndef OFFS_PEER_ROUTES_H
#define OFFS_PEER_ROUTES_H

#include "http_server.h"
#include "../../Scheduler/scheduler.h"
#include "../../Configuration/config.h"

typedef struct offs_node_t offs_node_t;

void peer_routes_register(http_server_t* server, offs_node_t* node,
                          const config_t* config, const char* api_key);

#endif // OFFS_PEER_ROUTES_H
