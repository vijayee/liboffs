//
// Created by victor on 5/6/25.
//

#ifndef OFFS_MESSAGE_QUEUE_H
#define OFFS_MESSAGE_QUEUE_H

#include "message.h"
#include "../Util/atomic_compat.h"
#include <stdbool.h>
#include <stddef.h>

#define MAILBOX_MUTE_THRESHOLD 256

typedef struct message_node_t message_node_t;
struct message_node_t {
  message_t msg;
  ATOMIC(message_node_t*) next;
};

typedef struct message_queue_t {
  ATOMIC(message_node_t*) head;
  message_node_t* tail;
  ATOMIC(size_t) size;
  /* Optional back-pointer to the owning pool's pending-messages counter.
     When non-NULL, push increments and pop decrements it, so the pool always
     knows how many messages are queued across all of its actors. This is what
     scheduler_pool_wait_for_idle waits on (in addition to all workers being
     idle) so it never returns with an undelivered message still stranded in a
     mailbox. NULL for queues not owned by a scheduler pool. */
  ATOMIC(size_t)* pending_counter;
} message_queue_t;

void message_queue_init(message_queue_t* queue);
bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last);
// Single-producer fast path. Only safe when no concurrent producers exist.
bool message_queue_push_single(message_queue_t* queue, message_node_t* first, message_node_t* last);
message_node_t* message_queue_pop(message_queue_t* queue);
bool message_queue_markempty(message_queue_t* queue);
bool message_queue_isempty(message_queue_t* queue);
void message_queue_destroy(message_queue_t* queue);

#endif // OFFS_MESSAGE_QUEUE_H
