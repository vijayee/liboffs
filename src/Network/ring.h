//
// Created by victor on 5/14/25.
//

#ifndef OFFS_RING_H
#define OFFS_RING_H

#include "net_node.h"
#include "../Util/vec.h"
#include <stdint.h>
#include <stdbool.h>

#define RING_MAX_RINGS 10

typedef vec_t(net_node_t*) vec_node_t;

typedef struct ring_t {
  vec_node_t primary;
  vec_node_t secondary;
  bool frozen;
  uint32_t latency_min_us;
  uint32_t latency_max_us;
} ring_t;

ring_t* ring_create(uint32_t latency_min_us, uint32_t latency_max_us);
void ring_destroy(ring_t* ring);

int ring_insert_primary(ring_t* ring, net_node_t* node, size_t max_primary);
int ring_insert_secondary(ring_t* ring, net_node_t* node, size_t max_secondary);
net_node_t* ring_find_by_id(ring_t* ring, const node_id_t* id);
bool ring_erase(ring_t* ring, const node_id_t* id);

#endif // OFFS_RING_H