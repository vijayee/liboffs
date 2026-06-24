//
// Created by victor on 6/20/26.
//

#ifndef OFFS_CONFIG_HANDLERS_H
#define OFFS_CONFIG_HANDLERS_H

#include "../Configuration/config.h"
#include "../Node/node.h"
#include "client_api_wire.h"
#include <cbor.h>
#include <stdint.h>

/* Opaque connection pointer — each transport casts its connection type. */
typedef void config_connection_t;

/* Function pointer types for transport-specific I/O. */
typedef void (*config_send_frame_fn)(config_connection_t* conn, cbor_item_t* frame);
typedef void (*config_send_error_fn)(config_connection_t* conn, uint8_t status, const char* msg);

/* Optional restart trigger. When set (by a daemon that owns the node lifecycle
   and runs the restart on a non-pool thread), the reload/restart handlers call
   it instead of offs_node_restart — offs_node_restart cannot run on a scheduler
   pool worker (offs_node_stop waits for/joins that same pool → self-deadlock)
   and, in offsd, the node's scheduler is a pool shared with the transport. When
   NULL (standalone libofs / tests calling from the main thread), the handlers
   fall back to offs_node_restart directly. */
typedef void (*config_trigger_restart_fn)(void* user_data);

typedef struct {
  config_connection_t* conn;
  offs_node_t* node;          /* config read via node->config; also restart target */
  const char* data_dir;      /* borrowed from the transport (owned by transport) */
  uint8_t is_authenticated;
  config_send_frame_fn send_frame;
  config_send_error_fn send_error;
  config_trigger_restart_fn trigger_restart; /* NULL → offs_node_restart fallback */
  void* restart_user_data;
} config_handler_ctx_t;

/* Frame handlers — called from each transport's dispatch_frame switch. */
void config_handle_show_request(config_handler_ctx_t* ctx, cbor_item_t* frame);
void config_handle_set_request(config_handler_ctx_t* ctx, cbor_item_t* frame);
void config_handle_reload_request(config_handler_ctx_t* ctx, cbor_item_t* frame);

#endif /* OFFS_CONFIG_HANDLERS_H */