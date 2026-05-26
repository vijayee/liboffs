//
// Created by victor on 5/26/25.
//
#ifndef OFFS_HEALTH_ROUTES_H
#define OFFS_HEALTH_ROUTES_H

#include "../health_handler.h"
#include "http_server.h"

void health_routes_register(http_server_t* server, const health_context_t* ctx);

#endif
