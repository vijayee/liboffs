//
// Created by victor on 5/27/26.
//

#ifndef OFFS_PEER_HANDLERS_H
#define OFFS_PEER_HANDLERS_H

#include "client_api_wire.h"
#include "../Network/network.h"
#include "../Network/authority.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include <cbor.h>
#include <stdint.h>

#include "block_handlers.h"

typedef struct {
  block_connection_t* conn;
  network_t* network;
  authority_t* authority;
  actor_t* actor;
  uint8_t is_authenticated;
  block_send_frame_fn send_frame;
  block_send_error_fn send_error;
} peer_handler_ctx_t;

void peer_handle_info_request(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_connect(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_friend_add(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_friend_remove(peer_handler_ctx_t* ctx, cbor_item_t* frame);
void peer_handle_friend_list_request(peer_handler_ctx_t* ctx, cbor_item_t* frame);

#endif // OFFS_PEER_HANDLERS_H
