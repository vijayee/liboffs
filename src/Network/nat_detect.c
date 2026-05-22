//
// Created by victor on 5/16/26.
//

#include "nat_detect.h"
#include "wire.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>
#include <stdlib.h>

/* --- NAT classification logic (no QUIC dependency) --- */

nat_type_e nat_detect_classify(uint32_t local_addr,
                                uint32_t reflexive_addr_a, uint16_t reflexive_port_a, uint8_t got_response_a,
                                uint32_t reflexive_addr_b, uint16_t reflexive_port_b, uint8_t got_response_b) {
  /* No responses at all */
  if (!got_response_a && !got_response_b) {
    return NAT_TYPE_UNKNOWN;
  }

  /* Only relay A responded */
  if (got_response_a && !got_response_b) {
    if (local_addr == reflexive_addr_a) {
      return NAT_TYPE_OPEN;
    }
    return NAT_TYPE_PORT_RESTRICTED_CONE;
  }

  /* Only relay B responded */
  if (!got_response_a && got_response_b) {
    if (local_addr == reflexive_addr_b) {
      return NAT_TYPE_OPEN;
    }
    return NAT_TYPE_PORT_RESTRICTED_CONE;
  }

  /* Both relays responded — compare reflexive addresses */
  if (local_addr == reflexive_addr_a) {
    return NAT_TYPE_OPEN;
  }

  if (reflexive_addr_a == reflexive_addr_b && reflexive_port_a == reflexive_port_b) {
    return NAT_TYPE_FULL_CONE;
  }

  /* Different addresses, or same address with different port — both are symmetric NAT.
     With two relays we cannot distinguish RESTRICTED_CONE from PORT_RESTRICTED_CONE,
     so anything that isn't FULL_CONE or OPEN falls into SYMMETRIC when both relays
     respond with different mappings. */
  return NAT_TYPE_SYMMETRIC;
}

#ifdef HAS_MSQUIC

/* --- Actor dispatch --- */

void nat_detect_dispatch(void* state, message_t* msg) {
  nat_detect_t* detect = (nat_detect_t*)state;
  if (msg == NULL) return;

  switch (msg->type) {
    case NETWORK_RELAY_RECEIVED: {
      wire_relay_received_t* received = (wire_relay_received_t*)msg->payload;
      if (received == NULL) break;

      /* Decode the inner wire message to check if it's an ADDR_RESPONSE */
      struct cbor_load_result load_result;
      cbor_item_t* cbor = cbor_load(received->payload, received->payload_len, &load_result);
      if (cbor == NULL) {
        log_error("nat_detect: failed to parse CBOR from relay received payload");
        break;
      }

      uint8_t msg_type = wire_get_type(cbor);
      if (msg_type == WIRE_ADDR_RESPONSE) {
        wire_addr_response_t response;
        memset(&response, 0, sizeof(response));
        if (wire_addr_response_decode(cbor, &response) == 0) {
          /* Assign first response to relay_a slot, second to relay_b */
          if (!detect->got_response_a) {
            detect->reflexive_addr_a = response.reflexive_addr;
            detect->reflexive_port_a = response.reflexive_port;
            detect->endpoint_id_a = response.endpoint_id;
            detect->got_response_a = 1;
            log_info("nat_detect: relay_a response — addr=0x%08x port=%u endpoint=%u",
                     response.reflexive_addr, response.reflexive_port, response.endpoint_id);
          } else if (!detect->got_response_b) {
            detect->reflexive_addr_b = response.reflexive_addr;
            detect->reflexive_port_b = response.reflexive_port;
            detect->endpoint_id_b = response.endpoint_id;
            detect->got_response_b = 1;
            log_info("nat_detect: relay_b response — addr=0x%08x port=%u endpoint=%u",
                     response.reflexive_addr, response.reflexive_port, response.endpoint_id);
          }

          /* Classify once both responses are in */
          if (detect->got_response_a && detect->got_response_b) {
            detect->detected_type = nat_detect_classify(
                detect->local_addr,
                detect->reflexive_addr_a, detect->reflexive_port_a, detect->got_response_a,
                detect->reflexive_addr_b, detect->reflexive_port_b, detect->got_response_b);
            detect->detection_complete = 1;
            log_info("nat_detect: classification complete — type=%d", detect->detected_type);
          }
        } else {
          log_error("nat_detect: failed to decode ADDR_RESPONSE");
        }
      }
      cbor_decref(&cbor);
      break;
    }
    default:
      break;
  }

  if (msg->payload_destroy != NULL && msg->payload != NULL) {
    msg->payload_destroy(msg->payload);
  }
}

/* --- Public API --- */

nat_detect_t* nat_detect_create(network_t* network, scheduler_pool_t* pool) {
  nat_detect_t* detect = get_clear_memory(sizeof(nat_detect_t));
  if (detect == NULL) return NULL;

  detect->network = network;
  detect->pool = pool;
  detect->detected_type = NAT_TYPE_UNKNOWN;

  actor_init(&detect->actor, detect, nat_detect_dispatch, pool);

  return detect;
}

void nat_detect_destroy(nat_detect_t* detect) {
  if (detect == NULL) return;

  if (detect->relay_a != NULL) {
    relay_client_destroy(detect->relay_a);
    detect->relay_a = NULL;
  }
  if (detect->relay_b != NULL) {
    relay_client_destroy(detect->relay_b);
    detect->relay_b = NULL;
  }

  free(detect->relay_a_host);
  detect->relay_a_host = NULL;
  free(detect->relay_b_host);
  detect->relay_b_host = NULL;

  actor_destroy(&detect->actor);
  free(detect);
}

int nat_detect_start(nat_detect_t* detect, const char* relay_a_host, uint16_t relay_a_port,
                     const char* relay_b_host, uint16_t relay_b_port) {
  if (detect == NULL) return -1;

  /* Store relay address info */
  if (relay_a_host != NULL) {
    detect->relay_a_host = strdup(relay_a_host);
    if (detect->relay_a_host == NULL) {
      log_error("nat_detect: failed to allocate relay_a_host");
      return -1;
    }
  }
  detect->relay_a_port = relay_a_port;

  if (relay_b_host != NULL) {
    detect->relay_b_host = strdup(relay_b_host);
    if (detect->relay_b_host == NULL) {
      log_error("nat_detect: failed to allocate relay_b_host");
      free(detect->relay_a_host);
      detect->relay_a_host = NULL;
      return -1;
    }
  }
  detect->relay_b_port = relay_b_port;

  /* Create and connect relay client A */
  detect->relay_a = relay_client_create(detect->network, detect->pool,
      detect->network->relay_max_retries, detect->network->relay_retry_delay_ms);
  if (detect->relay_a == NULL) {
    log_error("nat_detect: failed to create relay client A");
    free(detect->relay_a_host);
    detect->relay_a_host = NULL;
    free(detect->relay_b_host);
    detect->relay_b_host = NULL;
    return -1;
  }

  int result = relay_client_connect(detect->relay_a, relay_a_host, relay_a_port,
#ifdef HAS_MSQUIC
                                      NULL
#else
                                      NULL
#endif
                                      );
  if (result != 0) {
    log_error("nat_detect: failed to connect relay client A");
    relay_client_destroy(detect->relay_a);
    detect->relay_a = NULL;
    free(detect->relay_a_host);
    detect->relay_a_host = NULL;
    free(detect->relay_b_host);
    detect->relay_b_host = NULL;
    return -1;
  }

  /* Create and connect relay client B */
  detect->relay_b = relay_client_create(detect->network, detect->pool,
      detect->network->relay_max_retries, detect->network->relay_retry_delay_ms);
  if (detect->relay_b == NULL) {
    log_error("nat_detect: failed to create relay client B");
    relay_client_destroy(detect->relay_a);
    detect->relay_a = NULL;
    free(detect->relay_a_host);
    detect->relay_a_host = NULL;
    free(detect->relay_b_host);
    detect->relay_b_host = NULL;
    return -1;
  }

  result = relay_client_connect(detect->relay_b, relay_b_host, relay_b_port,
#ifdef HAS_MSQUIC
                                      NULL
#else
                                      NULL
#endif
                                      );
  if (result != 0) {
    log_error("nat_detect: failed to connect relay client B");
    relay_client_destroy(detect->relay_b);
    detect->relay_b = NULL;
    relay_client_destroy(detect->relay_a);
    detect->relay_a = NULL;
    free(detect->relay_a_host);
    detect->relay_a_host = NULL;
    free(detect->relay_b_host);
    detect->relay_b_host = NULL;
    return -1;
  }

  /* Send ADDR_REQUEST to both relays. The relay_client_dispatch handler
     checks connected status before sending on the stream, so messages
     queued before the QUIC connection completes will be rejected.
     The network layer should re-send ADDR_REQUEST once the relay client
     reports connected via NETWORK_QUIC_CONNECTED. For initial detection,
     the caller should send ADDR_REQUEST messages after observing
     successful connection establishment. */
  wire_addr_request_t addr_request_a;
  memset(&addr_request_a, 0, sizeof(addr_request_a));
  addr_request_a.message_id = 1;

  message_t msg_a;
  memset(&msg_a, 0, sizeof(msg_a));
  msg_a.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_a.payload = &addr_request_a;
  msg_a.payload_destroy = NULL;
  actor_send(&detect->relay_a->actor, &msg_a);

  wire_addr_request_t addr_request_b;
  memset(&addr_request_b, 0, sizeof(addr_request_b));
  addr_request_b.message_id = 2;

  message_t msg_b;
  memset(&msg_b, 0, sizeof(msg_b));
  msg_b.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_b.payload = &addr_request_b;
  msg_b.payload_destroy = NULL;
  actor_send(&detect->relay_b->actor, &msg_b);

  log_info("nat_detect: sent ADDR_REQUEST to both relays");
  return 0;
}

nat_type_e nat_detect_get_type(nat_detect_t* detect) {
  if (detect == NULL) return NAT_TYPE_UNKNOWN;
  return detect->detected_type;
}

#else /* !HAS_MSQUIC — stub implementations */

nat_detect_t* nat_detect_create(network_t* network, scheduler_pool_t* pool) {
  (void)network;
  (void)pool;
  return NULL;
}

void nat_detect_destroy(nat_detect_t* detect) {
  (void)detect;
}

int nat_detect_start(nat_detect_t* detect, const char* relay_a_host, uint16_t relay_a_port,
                     const char* relay_b_host, uint16_t relay_b_port) {
  (void)detect;
  (void)relay_a_host;
  (void)relay_a_port;
  (void)relay_b_host;
  (void)relay_b_port;
  return -1;
}

nat_type_e nat_detect_get_type(nat_detect_t* detect) {
  (void)detect;
  return NAT_TYPE_UNKNOWN;
}

void nat_detect_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
}

#endif /* HAS_MSQUIC */