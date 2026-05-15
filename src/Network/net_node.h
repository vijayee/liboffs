//
// Created by victor on 5/14/25.
//

#ifndef OFFS_NET_NODE_H
#define OFFS_NET_NODE_H

#include "node_id.h"
#include "authority.h"
#include "../Util/atomic_compat.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum net_node_flags_e {
  NET_NODE_FLAG_NONE = 0,
  NET_NODE_FLAG_RENDEZVOUS = (1 << 0)
} net_node_flags_e;

typedef struct net_node_t {
  node_id_t id;
  uint32_t addr;                // IPv4 address, network byte order
  uint16_t port;                // port, network byte order
  uint32_t rendv_addr;         // rendezvous point address (NAT traversal)
  uint16_t rendv_port;         // rendezvous point port
  net_node_flags_e flags;

  // Meridian ring membership state
  float latency_ms;            // EWMA-smoothed latency to this node
  float weight;                // Hebbian w_{self→this_node}
  float capacity;              // cached capacity [0,1]
  node_phase_e phase;          // cached phase (INHALE/NEUTRAL/EXHALE)
  float availability;          // EWMA availability [0,1]
  uint32_t consecutive_fails;  // consecutive failed requests
  uint64_t last_gossip_time;   // last PingCapacity exchange time (ms)
} net_node_t;

net_node_t* net_node_create(const node_id_t* id, uint32_t addr, uint16_t port);
net_node_t* net_node_create_rendv(const node_id_t* id, uint32_t addr, uint16_t port,
                                   uint32_t rendv_addr, uint16_t rendv_port);
net_node_t* net_node_create_unidentified(uint32_t addr, uint16_t port);
void net_node_destroy(net_node_t* node);

bool net_node_equals_by_id(const net_node_t* left, const net_node_t* right);
bool net_node_id_equals(const node_id_t* left, const node_id_t* right);
bool net_node_matches_id(const net_node_t* node, const node_id_t* id);
bool net_node_equals_by_addr(const net_node_t* left, const net_node_t* right);
int net_node_latency_cmp(const void* left, const void* right);

void net_node_update_latency(net_node_t* node, float latency_ms);
void net_node_record_success(net_node_t* node);
void net_node_record_fail(net_node_t* node);

#endif // OFFS_NET_NODE_H