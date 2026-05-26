//
// Created by victor on 5/26/26.
//

#ifndef OFFS_BLOCK_ROUTES_H
#define OFFS_BLOCK_ROUTES_H

#include "http_server.h"
#include "../../BlockCache/block_cache.h"
#include "../../Configuration/config.h"
#include "../../Scheduler/scheduler.h"

/* Register block cache routes on the HTTP server.
   Only registers routes if config->api_key_hash is non-NULL (auth enabled).
   When api_key is NULL (auth disabled), no routes are registered. */
void block_routes_register(http_server_t* server, scheduler_pool_t* pool,
                           block_cache_t* bc, const config_t* config,
                           const char* api_key);

#endif // OFFS_BLOCK_ROUTES_H
