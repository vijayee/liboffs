//
// Created by victor on 5/14/25.
//

#ifndef OFFS_RING_SET_H
#define OFFS_RING_SET_H

#include "ring.h"
#include "net_node.h"
#include <stdint.h>
#include <stddef.h>

// Meridian ring constants (from Network Design spec)
#define RING_ALPHA 2       // log base for latency bucketing
#define RING_BETA_MS 5     // innermost ring radius in ms
#define RING_K 8           // primary members per ring
#define RING_M 4           // secondary members per ring

typedef struct ring_set_t {
  ring_t rings[RING_MAX_RINGS];
  size_t ring_count;
  size_t primary_ring_size;
  size_t secondary_ring_size;
  int32_t exponent_base;
} ring_set_t;

ring_set_t* ring_set_create(size_t primary_size, size_t secondary_size, int32_t exponent_base);
void ring_set_destroy(ring_set_t* set);

int ring_set_get_ring_index(const ring_set_t* set, uint32_t latency_us);
ring_t* ring_set_get_ring(ring_set_t* set, uint32_t latency_us);

int ring_set_insert(ring_set_t* set, net_node_t* node, uint32_t latency_us);
net_node_t* ring_set_find_by_id(ring_set_t* set, const node_id_t* id);
net_node_t* ring_set_find_closest(ring_set_t* set);
bool ring_set_erase(ring_set_t* set, const node_id_t* id);

bool ring_set_eligible_for_replacement(const ring_set_t* set, int ring_index);
net_node_t* ring_set_promote_secondary(ring_set_t* set, int ring_index);

size_t ring_set_total_nodes(const ring_set_t* set);
void ring_set_clear_nodes(ring_set_t* set);

#endif // OFFS_RING_SET_H