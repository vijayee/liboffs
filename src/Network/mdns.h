//
// Created by victor on 7/16/26.
//

#ifndef OFFS_MDNS_H
#define OFFS_MDNS_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "node_id.h"
#include <stdint.h>
#include <stddef.h>

/* Forward declaration — network_t is defined in network.h. Avoids a circular
   include (network.h pulls in many headers; mdns.h stays lightweight). */
struct network_t;

/* mDNS responder for same-LAN auto-discovery. Broadcasts the node's
   presence on the 224.0.0.251:5353 multicast group so other liboffs nodes
   on the same LAN can find each other without an out-of-band string
   exchange. Receives other nodes' broadcasts and feeds them into
   network_connect_peer_candidates as HOST candidates. See audit #18.

   POSIX-only — on Windows the mDNS API is stubbed (the WSAStartup +
   IP_MULTICAST API differences are a separate Windows-only task). */
typedef struct mdns_t mdns_t;

/* Create an mDNS responder for the given network. The network's local
   node_id (authority->local_id) and QUIC listener port are read at
   broadcast time, so the responder picks up changes after a hot restart.
   Returns NULL on allocation failure or on Windows (stubbed). */
mdns_t* mdns_create(struct network_t* network, scheduler_pool_t* pool);

/* Destroy the responder. Joins the broadcast thread and closes the
   multicast socket. Safe to call on a NULL pointer or an unstarted
   responder. */
void mdns_destroy(mdns_t* responder);

/* Start broadcasting and listening. Returns 0 on success, -1 on error.
   Idempotent — calling start on an already-started responder is a no-op. */
int mdns_start(mdns_t* responder);

/* Stop broadcasting and listening. Joins the thread. Idempotent. */
void mdns_stop(mdns_t* responder);

/* Test-only wrappers around the static packet helpers in mdns.c (see
   test_mdns.cpp). Declared on every platform so the test binary links
   everywhere; on Windows the definitions are stubs that return -1. */

/* Build an A-record announce packet (same bytes the live broadcaster sends).
   Returns packet size, -1 on error. */
int mdns_build_announce_v4_for_test(const char* node_id_b58, uint32_t lan_ip,
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len);

/* Build an AAAA-record announce packet. Returns packet size, -1 on error. */
int mdns_build_announce_v6_for_test(const char* node_id_b58,
                                    const uint8_t addr6[16],
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len);

/* Parse a response packet: fills node_id_b58, lan_ip (host byte order, 0 for
   AAAA-only announces), quic_port, and — when an AAAA record is present —
   addr6_out with *have_v6 set to 1. have_v6 is required and must not be NULL;
   addr6_out may be NULL when the caller doesn't want the address copied
   (have_v6 still reports AAAA presence). Returns 0 on success, -1 on no
   match / malformed input / no usable address record. */
int mdns_parse_response_for_test(const uint8_t* pkt, size_t pkt_len,
                                 char* node_id_b58, size_t node_id_b58_len,
                                 uint32_t* lan_ip, uint16_t* quic_port,
                                 uint8_t addr6_out[16], int* have_v6);

/* The IPv6 mDNS multicast group bytes (ff02::fb) the live responder joins and
   announces to. Returns NULL on Windows (mdns stubbed). */
const uint8_t* mdns_multicast_group_v6_for_test(void);

#endif // OFFS_MDNS_H