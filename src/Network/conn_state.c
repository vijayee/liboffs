//
// Created by victor on 5/16/26.
//

#include "conn_state.h"
#include "peer_connection.h"
#include "relay_client.h"
#include "quic_peer_send.h"
#include "wire.h"
#include "network.h"
#include "../Actor/actor.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cbor.h>
#include <string.h>

conn_state_e conn_state_init(nat_type_e local_nat_type) {
  switch (local_nat_type) {
    case NAT_TYPE_OPEN:
    case NAT_TYPE_FULL_CONE:
    case NAT_TYPE_RESTRICTED_CONE:
    case NAT_TYPE_PORT_RESTRICTED_CONE:
      return CONN_STATE_TRYING_DIRECT;
    case NAT_TYPE_SYMMETRIC:
      return CONN_STATE_RELAY_ONLY;
    case NAT_TYPE_UNKNOWN:
    default:
      return CONN_STATE_RELAY;
  }
}

conn_state_e conn_state_get(const peer_connection_t* peer) {
  if (peer == NULL) {
    return CONN_STATE_RELAY;
  }
  return peer->conn_state;
}

int conn_state_send(network_t* network, peer_connection_t* peer,
                    cbor_item_t* cbor_msg) {
  if (network == NULL || peer == NULL || cbor_msg == NULL) {
    return -1;
  }

  switch (peer->conn_state) {
    case CONN_STATE_DIRECT: {
      /* Direct QUIC path active — send via peer's QUIC stream.
       * quic_peer_send handles serialization and framing. The caller
       * retains ownership of cbor_msg and must call cbor_decref. */
#ifdef HAS_MSQUIC
      if (peer->quic_stream == NULL) {
        log_error("conn_state_send: DIRECT state but no QUIC stream");
        return -1;
      }
      return quic_peer_send(network, peer, cbor_msg);
#else
      log_error("conn_state_send: DIRECT state but no QUIC support compiled");
      return -1;
#endif
    }

    case CONN_STATE_TRYING_DIRECT:
    case CONN_STATE_RELAY:
    case CONN_STATE_RELAY_ONLY: {
      /* Relay path — serialize CBOR and dispatch via actor message.
       * The relay client will frame and send the data over its QUIC stream. */
      size_t cbor_len = 0;
      unsigned char* cbor_data = NULL;
      size_t serialized = cbor_serialize_alloc(cbor_msg, &cbor_data, &cbor_len);
      if (cbor_data == NULL || serialized == 0) {
        log_error("conn_state_send: failed to serialize CBOR for relay send");
        return -1;
      }

      wire_relay_send_t* relay_send = get_clear_memory(sizeof(wire_relay_send_t));
      if (relay_send == NULL) {
        log_error("conn_state_send: failed to allocate relay send payload");
        free(cbor_data);
        return -1;
      }

      relay_send->src_endpoint_id = 0;
      relay_send->dest_endpoint_id = 0;
      relay_send->payload = cbor_data;
      relay_send->payload_len = cbor_len;

      message_t msg;
      msg.type = RELAY_CLIENT_SEND;
      msg.payload = relay_send;
      msg.payload_destroy = (void (*)(void*))wire_relay_send_destroy;

      /* Send the message to the network actor which will forward it
       * to the appropriate relay client.
       * Note: actor_send takes ownership of the payload. On failure
       * (actor destroyed), actor_send already calls payload_destroy,
       * so we must NOT free it again. */
      bool sent = actor_send(&network->actor, &msg);
      if (!sent) {
        log_error("conn_state_send: failed to send relay message to network actor");
        return -1;
      }
      return 0;
    }

    default:
      return -1;
  }
}

void conn_state_on_direct_connected(peer_connection_t* peer) {
  if (peer == NULL) {
    return;
  }
  peer->conn_state = CONN_STATE_DIRECT;
  peer->direct_path.active = 1;
}

void conn_state_on_direct_failed(peer_connection_t* peer) {
  if (peer == NULL) {
    return;
  }

  /* If we were in RELAY_ONLY, a direct failure doesn't change state */
  if (peer->conn_state == CONN_STATE_RELAY_ONLY) {
    return;
  }

  peer->conn_state = CONN_STATE_RELAY;
  peer->direct_path.active = 0;
}

void conn_state_set_peer_nat_type(peer_connection_t* peer, nat_type_e peer_nat_type) {
  if (peer == NULL) {
    return;
  }

  peer->peer_nat_type = peer_nat_type;

  /* If the remote peer is behind symmetric NAT, direct connection
   * is impossible regardless of our own NAT type. */
  if (peer_nat_type == NAT_TYPE_SYMMETRIC) {
    peer->conn_state = CONN_STATE_RELAY_ONLY;
    peer->direct_path.active = 0;
  }
}

void conn_state_upgrade_to_direct(peer_connection_t* peer) {
  if (peer == NULL) {
    return;
  }

  /* RELAY_ONLY means at least one side has symmetric NAT —
   * direct is impossible, do not upgrade. */
  if (peer->conn_state == CONN_STATE_RELAY_ONLY) {
    return;
  }

  /* Upgrade from RELAY to TRYING_DIRECT — we'll attempt a direct
   * connection while still using the relay as backup. */
  if (peer->conn_state == CONN_STATE_RELAY) {
    peer->conn_state = CONN_STATE_TRYING_DIRECT;
  }
}

int conn_state_should_try_direct(peer_connection_t* peer) {
  if (peer == NULL) {
    return 0;
  }
  return peer->conn_state == CONN_STATE_TRYING_DIRECT ? 1 : 0;
}