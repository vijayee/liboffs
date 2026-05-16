//
// Created by victor on 5/16/26.
//

#ifndef OFFS_NAT_DETECT_H
#define OFFS_NAT_DETECT_H

#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "conn_state.h"
#include "relay_client.h"
#include <stdint.h>
#include <stddef.h>

typedef struct nat_detect_t {
  actor_t actor;
  network_t* network;
  scheduler_pool_t* pool;

  /* Two relay clients for dual comparison */
  relay_client_t* relay_a;
  relay_client_t* relay_b;

  /* Results from each relay */
  uint32_t reflexive_addr_a;
  uint16_t reflexive_port_a;
  uint32_t endpoint_id_a;
  uint8_t got_response_a;

  uint32_t reflexive_addr_b;
  uint16_t reflexive_port_b;
  uint32_t endpoint_id_b;
  uint8_t got_response_b;

  /* Local address for comparison */
  uint32_t local_addr;

  /* Detected NAT type */
  nat_type_e detected_type;
  uint8_t detection_complete;

  /* Address info for relay connections (owned strings) */
  char* relay_a_host;
  uint16_t relay_a_port;
  char* relay_b_host;
  uint16_t relay_b_port;
} nat_detect_t;

/* Create/destroy */
nat_detect_t* nat_detect_create(network_t* network, scheduler_pool_t* pool);
void nat_detect_destroy(nat_detect_t* detect);

/* Connect to relay servers and detect NAT type */
int nat_detect_start(nat_detect_t* detect, const char* relay_a_host, uint16_t relay_a_port,
                     const char* relay_b_host, uint16_t relay_b_port);

/* Get the detected NAT type (returns NAT_TYPE_UNKNOWN if detection not complete) */
nat_type_e nat_detect_get_type(nat_detect_t* detect);

/* Classify NAT type from reflexive addresses — exposed for unit testing */
nat_type_e nat_detect_classify(uint32_t local_addr,
                                uint32_t reflexive_addr_a, uint16_t reflexive_port_a, uint8_t got_response_a,
                                uint32_t reflexive_addr_b, uint16_t reflexive_port_b, uint8_t got_response_b);

/* Actor dispatch handler */
void nat_detect_dispatch(void* state, message_t* msg);

#endif // OFFS_NAT_DETECT_H