//
// Created by victor on 10/12/25.
//

#ifndef OFFS_STREAM_H
#define OFFS_STREAM_H
#include "../RefCounter/refcounter.h"
#include "../Workers/priority.h"
#include "../Workers/pool.h"
#include "../Workers/error.h"

typedef enum {
  readable_stream = 0x0,
  writeable_stream = 0x1,
  duplex_stream = 0x2,
  transform_stream = 0x3
}  stream_type_e;

typedef enum {
  push = 0x0,
  pull = 0x1
} stream_force_e;

typedef enum {
  readable_event = 0x1,
  complete_event = 0x2,
  data_event = 0x3,
  overflow_event = 0x4,
  empty_event = 0x5,
  pause_event = 0x6,
  resume_event = 0x7,
  pipe_event = 0x8,
  unpipe_event = 0x9,
  close_event = 0x10,
  finished_event = 0x11,
  unpiped_event = 0x12,
  piped_event = 0x13,
  drain_event = 0x15,
  error_event = 0x16
} stream_event_e;

typedef struct {
  size_t id;
  void* ctx;
  void (* handler)(void*, void*);
  void (* ctx_destroy)(void*);
  uint8_t once;
} stream_event_handler_t;

typedef struct stream_event_handler_list_node_t stream_event_handler_list_node_t;

struct stream_event_handler_list_node_t {
  stream_event_handler_t* handler;
  stream_event_handler_list_node_t* next;
  stream_event_handler_list_node_t* previous;
};

stream_event_handler_list_node_t* stream_event_handler_list_node_create(stream_event_handler_t* event, stream_event_handler_list_node_t* next, stream_event_handler_list_node_t* previous);


typedef struct {
  stream_event_handler_list_node_t *first;
  stream_event_handler_list_node_t *last;
  size_t count;
} stream_event_handler_list_t;

stream_event_handler_list_t* stream_event_list_create();
void stream_event_list_destroy(stream_event_handler_list_t* list);
void stream_event_list_enqueue(stream_event_handler_list_t* list, stream_event_handler_t* handler);
stream_event_handler_t* stream_event_list_dequeue(stream_event_handler_list_t* list);
stream_event_handler_t* stream_event_list_remove(stream_event_handler_list_t* list, stream_event_handler_list_node_t* node);

typedef enum {
  readable_push = 0x1,
  readable_read = 0x2,
  writeable_write = 0x3,
  close_stream = 0x4
} message_type_e;
typedef struct {
  int type;
  void* payload;
  void* ctx;
} message_t;
typedef struct message_list_node_t message_list_node_t;


struct message_list_node_t {
  message_t* message;
  message_list_node_t* next;
  message_list_node_t* previous;
};

typedef struct {
  PLATFORMLOCKTYPE(lock);
  message_list_node_t* first;
  message_list_node_t* last;
  size_t count;
} message_queue_t;

message_queue_t* message_queue_create();
void message_queue_destroy(message_queue_t* queue);
void message_queue_enqueue(message_queue_t* queue, message_t* message);
message_t* message_queue_dequeue(message_queue_t* queue);
typedef struct {
  PLATFORMLOCKTYPE(lock);
  uint8_t is_working;
} stream_worker_status;

typedef struct {
  stream_event_e event;
  size_t id;
} stream_notifier_t;

typedef struct stream_t stream_t;
#define STREAM_HANDLER_COUNT 16
struct stream_t {
  refcounter_t refcounter;
  PLATFORMLOCKTYPE(lock);
  priority_t priority;
  message_queue_t* queue;
  work_pool_t* pool;
  stream_type_e type;
  stream_force_e force;
  size_t next_handler_id;
  stream_event_handler_list_t handlers[STREAM_HANDLER_COUNT];
  uint8_t readable;
  uint8_t is_piped;
  uint8_t auto_push;
  uint8_t is_deactivated;
  stream_notifier_t* pipe_notifiers;
  void (*on_data)(stream_t*, void*);
  void (*on_push)(stream_t*);
  void (*on_read)(stream_t*, size_t, void*, void (*)(void*, void*));
  void (*on_write)(stream_t*, void*);
  void (*on_close)(stream_t*);
  void (*on_deactivated)(stream_t*);
  void (*on_close_read)(stream_t*);
  void (*on_close_write)(stream_t*);
  void (*on_pipe)(stream_t*, stream_t*);
  void (*on_piped)(stream_t*, stream_t*);
  void (*destructor)(stream_t*);
  stream_worker_status worker_status;
};


typedef struct {
  void* ctx;
  void* readable;
  void* complete;
  void* data;
  void* overflow;
  void* empty;
  void* pause;
  void* resume;
  void* pipe;
  void* unpipe;
  void* close;
  void* error;
} readable_push_stream_events_t;

typedef struct {
  stream_t stream;
  uint8_t readable;
  uint8_t is_piped;
  uint8_t auto_push;
  uint8_t is_destroyed;
} readable_push_stream_t;

readable_push_stream_t* readable_push_stream_create();
void readable_push_stream_subscribe(readable_push_stream_events_t events);

typedef struct {
  void* ctx;
  void* finished;
  void* unpiped;
  void* piped;
  void* drain;
  void* close;
  void* error;
} writeable_push_stream_events_t;

typedef struct {
  size_t size;
  void (*cb)(void*, void*);
} read_payload;

void stream_init(stream_t* stream, stream_force_e force, stream_type_e type, void (*destructor)(stream_t*));
void stream_deinit(stream_t* stream);
void stream_deactivate(stream_t* stream, async_error_t* error);
void stream_close(stream_t* stream);
size_t stream_subscribe(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*));
size_t stream_once(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*));
void stream_unsubscribe(stream_t* stream, stream_event_e event, size_t id);
void writeable_stream_data_handler(stream_t* stream, void (*on_data)(stream_t*, void*));
void readable_stream_push_handler(stream_t* stream, void (*on_push)(stream_t*));
void readable_push_stream_pipe(stream_t* rs, stream_t* ws);
void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws);
void stream_notify(stream_t* stream, stream_event_e event, void* payload);
void readable_push_stream_push(stream_t* stream);
void readable_stream_read(stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*));
void writeable_stream_write_handler(stream_t* stream, void (*)(stream_t*, void*));
void writeable_stream_write(stream_t* stream, void* data);
void writeable_push_stream_piped(stream_t* ws, stream_t* rs);
void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs);
#endif //OFFS_STREAM_H
