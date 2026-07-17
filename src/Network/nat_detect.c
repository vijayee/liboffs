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
    case NETWORK_ADDR_RESPONSE: {
      network_addr_response_payload_t* response =
          (network_addr_response_payload_t*)msg->payload;
      if (response == NULL) break;

      /* Match the source relay_client against detect->relay_a / detect->relay_b
         to record the reflexive address in the right slot. If source_client
         doesn't match either (e.g. the network's primary relay when
         nat_detect_start wasn't called), fall back to first-free-slot
         assignment so a single-relay deployment still records a reflexive
         address for SRFLX candidates. Never overwrite an already-filled
         slot — a slow duplicate response is ignored. */
      int slot = 0;  /* 0 = a, 1 = b, -1 = unknown */
      if (response->source_client != NULL) {
        if (response->source_client == detect->relay_a) {
          slot = 0;
        } else if (response->source_client == detect->relay_b) {
          slot = 1;
        } else {
          slot = -1;
        }
      }
      if (slot == 0) {
        if (detect->got_response_a) break;
        detect->reflexive_addr_a = response->reflexive_addr;
        detect->reflexive_port_a = response->reflexive_port;
        detect->endpoint_id_a = response->endpoint_id;
        detect->got_response_a = 1;
        log_info("nat_detect: relay_a response — addr=0x%08x port=%u endpoint=%u",
                 response->reflexive_addr, response->reflexive_port, response->endpoint_id);
      } else if (slot == 1) {
        if (detect->got_response_b) break;
        detect->reflexive_addr_b = response->reflexive_addr;
        detect->reflexive_port_b = response->reflexive_port;
        detect->endpoint_id_b = response->endpoint_id;
        detect->got_response_b = 1;
        log_info("nat_detect: relay_b response — addr=0x%08x port=%u endpoint=%u",
                 response->reflexive_addr, response->reflexive_port, response->endpoint_id);
      } else if (slot == -1) {
        if (!detect->got_response_a) {
          detect->reflexive_addr_a = response->reflexive_addr;
          detect->reflexive_port_a = response->reflexive_port;
          detect->endpoint_id_a = response->endpoint_id;
          detect->got_response_a = 1;
          log_info("nat_detect: primary-relay response — addr=0x%08x port=%u endpoint=%u",
                   response->reflexive_addr, response->reflexive_port, response->endpoint_id);
        } else if (!detect->got_response_b) {
          detect->reflexive_addr_b = response->reflexive_addr;
          detect->reflexive_port_b = response->reflexive_port;
          detect->endpoint_id_b = response->endpoint_id;
          detect->got_response_b = 1;
          log_info("nat_detect: secondary-relay response — addr=0x%08x port=%u endpoint=%u",
                   response->reflexive_addr, response->reflexive_port, response->endpoint_id);
        } else {
          break;  /* both slots filled */
        }
      } else {
        break;
      }

      /* Classify once both responses are in. */
      if (detect->got_response_a && detect->got_response_b &&
          !detect->detection_complete) {
        detect->detected_type = nat_detect_classify(
            detect->local_addr,
            detect->reflexive_addr_a, detect->reflexive_port_a, detect->got_response_a,
            detect->reflexive_addr_b, detect->reflexive_port_b, detect->got_response_b);
        detect->detection_complete = 1;
        log_info("nat_detect: classification complete — type=%d",
                 (int)detect->detected_type);

        /* Notify the network actor so it can update network->local_nat_type on
           its own thread. The payload is small and owned by the message
           (payload_destroy = free). */
        if (detect->network != NULL) {
          network_nat_type_detected_payload_t* result =
              get_clear_memory(sizeof(network_nat_type_detected_payload_t));
          if (result != NULL) {
            result->nat_type = (int)detect->detected_type;
            message_t result_msg;
            memset(&result_msg, 0, sizeof(result_msg));
            result_msg.type = NETWORK_NAT_TYPE_DETECTED;
            result_msg.payload = result;
            result_msg.payload_destroy = free;
            actor_send(&detect->network->actor, &result_msg);
          }
        }
      }
      break;
    }
    default:
      break;
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
  /* Heap-allocate each payload with a freeing payload_destroy. The struct
     must outlive this stack frame — actor_send shallow-copies the message_t
     (keeping the pointer) and enqueues for another thread; the relay client
     actor dereferences it long after nat_detect_start has returned. A stack
     payload here is use-after-return. See docs/liboffs-audit-report.md #7. */
  wire_addr_request_t* addr_request_a = get_clear_memory(sizeof(wire_addr_request_t));
  if (addr_request_a == NULL) {
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
  addr_request_a->message_id = 1;

  message_t msg_a;
  memset(&msg_a, 0, sizeof(msg_a));
  msg_a.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_a.payload = addr_request_a;
  msg_a.payload_destroy = free;
  actor_send(&detect->relay_a->actor, &msg_a);

  wire_addr_request_t* addr_request_b = get_clear_memory(sizeof(wire_addr_request_t));
  if (addr_request_b == NULL) {
    /* addr_request_a is now owned by relay_a's mailbox (actor_send
       transferred ownership via payload_destroy = free); do not free it
       here — relay_a's dispatch (or mailbox destroy) will. */
    log_error("nat_detect: failed to allocate addr_request_b");
    return -1;
  }
  addr_request_b->message_id = 2;

  message_t msg_b;
  memset(&msg_b, 0, sizeof(msg_b));
  msg_b.type = RELAY_CLIENT_ADDR_REQUEST;
  msg_b.payload = addr_request_b;
  msg_b.payload_destroy = free;
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