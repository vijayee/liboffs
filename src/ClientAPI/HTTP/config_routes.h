//
// Created by victor on 5/28/26.
//
#ifndef OFFS_CONFIG_ROUTES_H
#define OFFS_CONFIG_ROUTES_H

#include "http_server.h"
#include "../../Configuration/config.h"
#include "../config_handlers.h"  /* config_trigger_restart_fn */

typedef struct offs_node_t offs_node_t;

void config_routes_register(http_server_t* server, offs_node_t* node,
                            const config_t* config, const char* data_dir,
                            config_trigger_restart_fn trigger_restart,
                            void* restart_user_data);

#endif
