//
// Created by victor on 5/8/25.
//

#ifndef OFFS_OFF_ROUTES_H
#define OFFS_OFF_ROUTES_H

#include "http_server.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../../BlockCache/block_cache.h"

typedef struct {
    scheduler_pool_t* pool;
    block_cache_t* bc;
    ofd_cache_t* ofd_cache;
    tuple_cache_t* tc;
} off_routes_context_t;

void off_routes_register(http_server_t* server, scheduler_pool_t* pool,
                         block_cache_t* bc, ofd_cache_t* ofd_cache);

off_routes_context_t* off_routes_context_create(scheduler_pool_t* pool,
                                                  block_cache_t* bc,
                                                  ofd_cache_t* ofd_cache);
void off_routes_context_destroy(off_routes_context_t* ctx);

#endif //OFFS_OFF_ROUTES_H