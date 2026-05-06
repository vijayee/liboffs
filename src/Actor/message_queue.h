//
// Created by victor on 5/6/25.
//

#ifndef OFFS_MESSAGE_QUEUE_H
#define OFFS_MESSAGE_QUEUE_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

typedef struct message_node_t message_node_t;
struct message_node_t {
  message_t msg;
  _Atomic(message_node_t*) next;
};

typedef struct message_queue_t {
  _Atomic(message_node_t*) head;
  message_node_t* tail;
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