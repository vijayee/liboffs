//
// Created by victor on 5/28/26.
//
#ifndef OFFS_CONFIG_ROUTES_H
#define OFFS_CONFIG_ROUTES_H

#include "http_server.h"
#include "../../Configuration/config.h"

typedef struct offs_node_t offs_node_t;

void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir);

#endif
