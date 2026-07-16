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

#endif // OFFS_MDNS_H