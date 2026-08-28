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
#include "../Network/peer_info.h"
#include <stddef.h>

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

/* Decode a peer_info payload by wire format byte: 0 = raw CBOR peer_info
   map, 1 = base58 text, 2 = PPM QR image (decoded via src/QR, then parsed
   as CBOR peer_info). Returns 0 on success, -1 if the payload is not
   decodable in the given format. Shared by the socket handlers and the
   HTTP routes so both transports accept exactly the same inputs. */
int peer_info_from_payload(uint8_t format, const uint8_t* data,
                           size_t data_size, peer_info_t* info);

#endif // OFFS_PEER_HANDLERS_H
