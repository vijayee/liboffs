//
// Created by victor on 5/16/25.
//

#ifndef OFFS_QUIC_PEER_SEND_H
#define OFFS_QUIC_PEER_SEND_H

#include "network.h"
#include "wire.h"

/* Encode and send a wire message to a specific peer via QUIC stream.
 * Encodes the CBOR item, frames it with 4-byte length prefix, and sends via StreamSend.
 * Returns 0 on success, -1 on failure.
 * The caller must still call cbor_decref on the cbor_msg after this call. */
int quic_peer_send(network_t* network, peer_connection_t* peer, cbor_item_t* cbor_msg);

/* Send a wire message to the next hop in a path.
 * Looks up the peer at path[0] via connection_manager_lookup and calls quic_peer_send.
 * Returns 0 on success, -1 on failure. */
int quic_peer_send_path(network_t* network, node_id_t* path, uint8_t path_len, cbor_item_t* cbor_msg);

/* Broadcast a wire message to all connected peers.
 * Sends to each peer in the connection manager. */
void quic_peer_broadcast(network_t* network, cbor_item_t* cbor_msg);

#endif // OFFS_QUIC_PEER_SEND_H