//
// Representation-level ephemeral/pin wire ops shared by every transport.
//
// One entry point decodes the request frame, spins the representation actor
// (op requests) or the cache mirror (ephemeral list), and replies over the
// transport's own frame/error senders. Each socket transport's dispatch just
// forwards the five new op codes here after its auth guard — the off_routes.c
// HTTP handlers own the same ops' HTTP lifecycle.
//
#ifndef OFFS_CLIENT_API_REPRESENTATION_API_H
#define OFFS_CLIENT_API_REPRESENTATION_API_H

#include "../BlockCache/block_cache.h"
#include "../Scheduler/scheduler.h"
#include <cbor.h>
#include <stddef.h>

typedef struct network_t network_t;

/* Opaque connection pointer — each transport casts its connection type. */
typedef void rep_api_connection_t;

/* Frame/error senders — transport adapters with these shapes. */
typedef void (*rep_api_send_frame_fn)(rep_api_connection_t* conn, cbor_item_t* frame);
typedef void (*rep_api_send_error_fn)(rep_api_connection_t* conn, int status,
                                      const char* message);

/* Handle one representation op frame (op_code = CLIENT_API_REP_MARK_PERMANENT_
   REQUEST / CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST / CLIENT_API_REP_PIN_
   REQUEST / CLIENT_API_REP_UNPIN_REQUEST) or one ephemeral list frame
   (CLIENT_API_EPHEMERAL_LIST_REQUEST). frame is the request's CBOR item
   (not consumed). bc/pool come from the connection; network may be NULL
   (local-only: the MARK_PERMANENT announce is skipped). The reply is sent
   asynchronously — the calling dispatch returns before the response frame
   goes out. */
void client_api_representation_handle(block_cache_t* bc, scheduler_pool_t* pool,
                                     network_t* network, cbor_item_t* frame, int op_code,
                                     rep_api_send_frame_fn send_frame,
                                     rep_api_connection_t* conn,
                                     rep_api_send_error_fn send_error);

#endif // OFFS_CLIENT_API_REPRESENTATION_API_H
