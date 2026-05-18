//
// Created by victor on 5/18/25.
//

#ifndef OFFS_MESSAGE_LOG_H
#define OFFS_MESSAGE_LOG_H

#include "node_id.h"
#include "hebbian.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
  MSG_DIRECTION_SENT = 0,
  MSG_DIRECTION_RECEIVED = 1,
  MSG_DIRECTION_FORWARDED = 2
} msg_direction_e;

typedef struct {
  uint8_t type;              // WIRE_FIND_BLOCK, etc.
  uint8_t direction;         // msg_direction_e
  uint64_t timestamp_ms;     // wall-clock timestamp (ms since epoch)
  node_id_t peer_id;         // who we sent to / received from
  uint64_t message_id;       // correlation ID from wire message
  uint8_t block_hash[32];    // zeroed if not applicable
  uint8_t result;            // 0=success, 1=forwarded, 2=not_found, 3=declined
  float hebbian_weight;      // peer's Hebbian weight AFTER this event
} message_event_t;

#define MESSAGE_LOG_CAPACITY 256

typedef struct {
  message_event_t events[MESSAGE_LOG_CAPACITY];
  size_t count;              // total events written (may exceed capacity, wraps via modulo)
} message_log_t;

// Functions are always declared. In release builds the log pointer is NULL,
// so these are never called (guarded by if (network->log != NULL)).
void message_log_init(message_log_t* log);
void message_log_record(message_log_t* log, uint8_t type, uint8_t direction,
                         const node_id_t* peer, uint64_t message_id,
                         const uint8_t* block_hash, uint8_t result,
                         const hebbian_table_t* hebbian);
size_t message_log_query(const message_log_t* log, size_t after_cursor,
                          message_event_t* out, size_t out_cap);
void message_log_clear(message_log_t* log);

#endif // OFFS_MESSAGE_LOG_H