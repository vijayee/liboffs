//
// Created by victor on 5/16/26.
//

#ifndef OFFS_CONN_STATE_H
#define OFFS_CONN_STATE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations to avoid circular includes */
typedef struct peer_connection_t peer_connection_t;
typedef struct network_t network_t;
typedef struct cbor_item_t cbor_item_t;

typedef enum {
  CONN_STATE_DIRECT,           /* Direct QUIC connection active */
  CONN_STATE_TRYING_DIRECT,    /* Attempting direct, relay as backup */
  CONN_STATE_RELAY,            /* Relay only */
  CONN_STATE_RELAY_ONLY        /* Symmetric NAT — never try direct */
} conn_state_e;

typedef enum {
  NAT_TYPE_UNKNOWN,
  NAT_TYPE_OPEN,                  /* Not behind NAT */
  NAT_TYPE_FULL_CONE,
  NAT_TYPE_RESTRICTED_CONE,
  NAT_TYPE_PORT_RESTRICTED_CONE,
  NAT_TYPE_SYMMETRIC
} nat_type_e;

typedef struct conn_path_t {
  uint32_t addr;               /* IPv4 address (network byte order) */
  uint16_t port;               /* Port number */
  uint32_t reflexive_addr;     /* Reflexive address from relay */
  uint16_t reflexive_port;     /* Reflexive port from relay */
  uint32_t rtt_ms;             /* Round-trip time in milliseconds */
  uint8_t active;              /* Whether this path is currently active */
} conn_path_t;

/* Initialize connection state based on detected NAT type */
conn_state_e conn_state_init(nat_type_e local_nat_type);

/* Determine which path to use for sending */
conn_state_e conn_state_get(const peer_connection_t* peer);

/* Send data using the appropriate path (direct or relay).
 * For direct path: passes the CBOR item to quic_peer_send for framing.
 * For relay path: encodes the CBOR item and dispatches via relay actor. */
int conn_state_send(network_t* network, peer_connection_t* peer,
                    cbor_item_t* cbor_msg);

/* State transitions */
void conn_state_on_direct_connected(peer_connection_t* peer);
void conn_state_on_direct_failed(peer_connection_t* peer);
void conn_state_set_peer_nat_type(peer_connection_t* peer, nat_type_e peer_nat_type);
void conn_state_upgrade_to_direct(peer_connection_t* peer);

/* Check if direct connection should be attempted */
int conn_state_should_try_direct(peer_connection_t* peer);

#endif // OFFS_CONN_STATE_H