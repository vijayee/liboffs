//
// HTTP/3 WebTransport-compatible server endpoint for browser-capable
// WebTransport clients.
//
// This implementation is intentionally lightweight: it uses MsQuic with ALPN
// "h3" and speaks length-prefixed CBOR frames over the first client-initiated
// bidirectional stream. It does not implement the full HTTP/3 control-stream
// SETTINGS/HEADERS exchange or QPACK; it is meant as a development/testing
// endpoint that matches the framing used by the browser JS client.
//
#ifndef OFFS_WEBTRANSPORT_H3_H
#define OFFS_WEBTRANSPORT_H3_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef HAS_MSQUIC
#include "../../Platform/platform.h"
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../BlockCache/block_cache.h"
#include "../../OFFStreams/ofd_cache.h"
#include "../../OFFStreams/tuple_cache.h"
#include "../health_handler.h"

typedef struct webtransport_h3_t webtransport_h3_t;

webtransport_h3_t* webtransport_h3_create(scheduler_pool_t* pool,
                                           block_cache_t* bc,
                                           ofd_cache_t* ofd_cache,
                                           tuple_cache_t* tc,
                                           const char* host,
                                           uint16_t port,
                                           const char* cert_path,
                                           const char* key_path,
                                           const char* ca_path,
                                           bool allow_secure,
                                           const char* api_key_hash,
                                           health_context_t* health_ctx);
void webtransport_h3_destroy(webtransport_h3_t* transport);
void webtransport_h3_start(webtransport_h3_t* transport);
void webtransport_h3_stop(webtransport_h3_t* transport);

#endif /* HAS_MSQUIC */
#endif /* OFFS_WEBTRANSPORT_H3_H */
