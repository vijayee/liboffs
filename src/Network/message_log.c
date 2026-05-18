//
// Created by victor on 5/18/25.
//

#ifdef OFFS_TEST

#include "message_log.h"
#include "hebbian.h"
#include <string.h>
#include <time.h>

void message_log_init(message_log_t* log) {
  if (log == NULL) return;
  memset(log, 0, sizeof(message_log_t));
}

void message_log_record(message_log_t* log, uint8_t type, uint8_t direction,
                         const node_id_t* peer, uint64_t message_id,
                         const uint8_t* block_hash, uint8_t result,
                         const hebbian_table_t* hebbian) {
  if (log == NULL) return;

  size_t index = log->count % MESSAGE_LOG_CAPACITY;
  message_event_t* event = &log->events[index];

  event->type = type;
  event->direction = direction;
  event->timestamp_ms = (uint64_t)(time(NULL) * 1000);

  if (peer != NULL) {
    memcpy(&event->peer_id, peer, sizeof(node_id_t));
  } else {
    memset(&event->peer_id, 0, sizeof(node_id_t));
  }

  event->message_id = message_id;

  if (block_hash != NULL) {
    memcpy(event->block_hash, block_hash, 32);
  } else {
    memset(event->block_hash, 0, 32);
  }

  event->result = result;

  // Snapshot the peer's Hebbian weight
  if (hebbian != NULL && peer != NULL) {
    event->hebbian_weight = hebbian_table_get(hebbian, peer);
  } else {
    event->hebbian_weight = 0.0f;
  }

  log->count++;
}

size_t message_log_query(const message_log_t* log, size_t after_cursor,
                          message_event_t* out, size_t out_cap) {
  if (log == NULL || out == NULL || out_cap == 0) return 0;

  size_t start;
  if (log->count <= MESSAGE_LOG_CAPACITY) {
    start = (after_cursor < log->count) ? after_cursor : log->count;
  } else {
    size_t earliest = log->count - MESSAGE_LOG_CAPACITY;
    start = (after_cursor >= earliest) ? (after_cursor + 1) : earliest;
    if (start >= log->count) return 0;
  }

  size_t copied = 0;
  for (size_t idx = start; idx < log->count && copied < out_cap; idx++) {
    size_t buf_idx = idx % MESSAGE_LOG_CAPACITY;
    out[copied] = log->events[buf_idx];
    copied++;
  }

  return copied;
}

void message_log_clear(message_log_t* log) {
  if (log == NULL) return;
  log->count = 0;
  log->cursor = 0;
}

#endif // OFFS_TEST