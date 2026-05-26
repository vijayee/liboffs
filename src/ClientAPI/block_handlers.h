//
// Created by victor on 5/26/26.
//

#ifndef OFFS_BLOCK_HANDLERS_H
#define OFFS_BLOCK_HANDLERS_H

#include "../BlockCache/block_cache.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "client_api_wire.h"
#include <cbor.h>
#include <stdint.h>

/* Opaque connection pointer — each transport casts its connection type */
typedef void block_connection_t;

/* Function pointer types for transport-specific I/O */
typedef void (*block_send_frame_fn)(block_connection_t* conn, cbor_item_t* frame);
typedef void (*block_send_error_fn)(block_connection_t* conn, uint8_t status, const char* msg);

typedef enum {
  BLOCK_OP_NONE = 0,
  BLOCK_OP_PUT  = 1,
  BLOCK_OP_GET  = 2,
  BLOCK_OP_DELETE = 3
} block_op_t;

typedef struct {
  block_connection_t* conn;
  block_cache_t* bc;
  actor_t* actor;
  uint8_t is_authenticated;
  block_send_frame_fn send_frame;
  block_send_error_fn send_error;

  /* Pending async state */
  block_op_t pending_op;
  uint8_t put_encoding;  /* 0=raw, 1=base58, for PUT responses */
} block_handler_ctx_t;

/* Frame handlers — called from each transport's dispatch_frame switch */
void block_handle_put_request(block_handler_ctx_t* ctx, cbor_item_t* frame);
void block_handle_get_request(block_handler_ctx_t* ctx, cbor_item_t* frame);
void block_handle_delete_request(block_handler_ctx_t* ctx, cbor_item_t* frame);

/* Cache result handler — called from each transport's actor dispatch.
   Returns 1 if the message was handled, 0 if it should fall through. */
int block_handle_cache_result(block_handler_ctx_t* ctx, message_t* msg);

#endif // OFFS_BLOCK_HANDLERS_H
