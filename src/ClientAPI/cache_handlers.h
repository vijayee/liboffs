#ifndef OFFS_CACHE_HANDLERS_H
#define OFFS_CACHE_HANDLERS_H

#include "block_handlers.h"

/* Frame handler — called from each transport's dispatch_frame switch.
   Stages the new capacity into the pending config (so a restart keeps
   it), then dispatches a live resize to the cache actor; the response
   frame is sent from cache_handle_resize_result on the async reply. */
void cache_handle_resize_request(block_handler_ctx_t* ctx, cbor_item_t* frame);

/* Resize result handler — called from each transport's actor dispatch.
   Returns 1 if the message was handled, 0 if it should fall through. */
int cache_handle_resize_result(block_handler_ctx_t* ctx, message_t* msg);

/* Shared core: stage + dispatch. Used by the Unix frame handler and the
   HTTP /cache/resize route so the two paths cannot drift. reply_to NULL
   means fire-and-forget (HTTP cannot wait on the cache actor's
   completion reply; the Unix CLI transport is the interactive path that
   reports the applied footprint). Returns 0 on success; on failure -1
   with err_buf filled and nothing dispatched (staging failure refuses
   the whole operation — a live resize a restart would silently revert
   is worse than no resize). */
int cache_resize_stage_and_dispatch(block_cache_t* bc, const char* data_dir,
                                    uint64_t capacity_bytes, actor_t* reply_to,
                                    char* err_buf, size_t err_len);

#endif // OFFS_CACHE_HANDLERS_H