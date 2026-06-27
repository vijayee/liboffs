//
// Created by victor on 10/12/25.
//
#include "stream.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/error.h"
#include "../RefCounter/refcounter.h"
#include <string.h>

void _readable_push_stream_piped_notify(stream_t* stream, void* payload);
void _readable_push_stream_error_notify(stream_t* stream, void* payload);
void _readable_push_stream_close_notify(stream_t* stream, void* payload);
void _writeable_push_stream_data_notify(stream_t* stream, void* data);
void _writeable_push_stream_error_notify(stream_t* stream, void* payload);
void _writeable_push_stream_close_notify(stream_t* stream, void* payload);
void _writeable_push_stream_complete_notify(stream_t* stream, void* payload);
void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs);
void _stream_purge_handlers(stream_t* stream);
void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws);
void _writeable_pull_stream_error_notify(stream_t* stream, void* payload);
void _readable_pull_stream_error_notify(stream_t* stream, void* payload);
void _readable_pull_stream_close_notify(stream_t* stream, void* payload);
void _readable_pull_stream_finish_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_close_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_data_notify(stream_t* stream, void* data);
void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws);
void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs);

/* ---- event handler ---- */

stream_event_handler_t* stream_event_handler_create(size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once) {
  stream_event_handler_t* _handler = get_clear_memory(sizeof(stream_event_handler_t));
  refcounter_init((refcounter_t*) _handler);
  _handler->handler = handler;
  _handler->ctx = ctx;
  _handler->ctx_destroy = ctx_destroy;
  _handler->once = once;
  _handler->id = id;
  return _handler;
}

void stream_event_handler_destroy(stream_event_handler_t* handler) {
  if (refcounter_dereference_is_zero((refcounter_t*) handler)) {
    refcounter_destroy_lock((refcounter_t*) handler);
    if (handler->ctx_destroy != NULL) {
      handler->ctx_destroy(handler->ctx);
    }
    free(handler);
  }
}

/* ---- event handler list ---- */

stream_event_handler_list_t* stream_event_list_create() {
  stream_event_handler_list_t* list = get_clear_memory(sizeof(stream_event_handler_list_t));
  list->first = NULL;
  list->last = NULL;
  return list;
}

void stream_event_list_destroy(stream_event_handler_list_t* list) {
  if (list == NULL) return;
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL) {
    next = current->next;
    stream_event_handler_destroy(current->handler);
    free(current);
    current = next;
  }
  free(list);
}

void stream_event_list_enqueue(stream_event_handler_list_t* list, stream_event_handler_t* event) {
  stream_event_handler_list_node_t* node = get_clear_memory(sizeof(stream_event_handler_list_node_t));
  node->handler = event;
  node->previous = NULL;
  node->next = NULL;
  if ((list->last == NULL) && (list->first == NULL)) {
    list->first = node;
    list->last = node;
  } else {
    node->previous = list->last;
    list->last->next = node;
    list->last = node;
  }
  list->count++;
}

stream_event_handler_t* stream_event_list_dequeue(stream_event_handler_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  } else {
    stream_event_handler_list_node_t* node = list->first;
    list->first = node->next;
    if (node->next != NULL) {
      list->first->previous = NULL;
    }
    if (list->last == node) {
      list->last = NULL;
    }
    stream_event_handler_t* event = node->handler;
    free(node);
    list->count--;
    return event;
  }
}

void stream_event_list_remove(stream_event_handler_list_t* list, stream_event_handler_list_node_t* node) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return;
  }
  if (list->last == node) {
    list->last = node->previous;
  }
  if (list->first == node) {
    list->first = node->next;
  }
  if (node->previous != NULL) {
    node->previous->next = node->next;
  }
  if (node->next != NULL) {
    node->next->previous = node->previous;
  }
  stream_event_handler_t* handler = node->handler;
  stream_event_handler_destroy(handler);
  list->count--;
  free(node);
}

void stream_event_list_remove_onces(stream_event_handler_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return;
  }
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL) {
    next = current->next;
    if (current->handler->once == 1) {
      if (list->last == current) {
        list->last = current->previous;
      }
      if (list->first == current) {
        list->first = current->next;
      }
      if (current->previous != NULL) {
        current->previous->next = current->next;
      }
      if (current->next != NULL) {
        current->next->previous = current->previous;
      }

      stream_event_handler_t* handler = current->handler;
      stream_event_handler_destroy(handler);
      list->count--;
      free(current);
    }
    current = next;
  }
}

/* Unlink a single node from its list and return the handler it held (the
   caller destroys the handler). Does NOT destroy the handler or decrement
   count-ownership — the caller owns the returned handler and must call
   stream_event_handler_destroy on it. Must be called with the owning
   stream's handlers_lock held (list mutation). */
static stream_event_handler_t* _stream_event_list_unlink_node(
    stream_event_handler_list_t* list, stream_event_handler_list_node_t* node) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  }
  if (list->last == node) {
    list->last = node->previous;
  }
  if (list->first == node) {
    list->first = node->next;
  }
  if (node->previous != NULL) {
    node->previous->next = node->next;
  }
  if (node->next != NULL) {
    node->next->previous = node->previous;
  }
  stream_event_handler_t* handler = node->handler;
  list->count--;
  free(node);
  return handler;
}

/* Unlink every once-handler node from the list and return the unlinked
   handlers as a freshly-allocated array (caller frees the array AND destroys
   each handler). *out_count receives the array length. Must be called with
   the owning stream's handlers_lock held (list mutation). Destroying the
   handlers is the caller's job so it can happen OUTSIDE the lock — a
   handler's ctx_destroy must not re-enter the lock. */
static stream_event_handler_t** _stream_event_list_unlink_onces(
    stream_event_handler_list_t* list, size_t* out_count) {
  *out_count = 0;
  if (list->first == NULL) {
    return NULL;
  }
  size_t n = 0;
  for (stream_event_handler_list_node_t* c = list->first; c != NULL; c = c->next) {
    if (c->handler->once == 1) {
      n++;
    }
  }
  if (n == 0) {
    return NULL;
  }
  stream_event_handler_t** removed = get_memory(n * sizeof(stream_event_handler_t*));
  size_t i = 0;
  stream_event_handler_list_node_t* current = list->first;
  while (current != NULL) {
    stream_event_handler_list_node_t* next = current->next;
    if (current->handler->once == 1) {
      removed[i++] = _stream_event_list_unlink_node(list, current);
    }
    current = next;
  }
  *out_count = i;
  return removed;
}

/* ---- stream lifecycle ---- */

void stream_init(stream_t* stream, stream_force_e force, stream_type_e type, uint8_t auto_push, scheduler_pool_t* pool, void (*destructor)(stream_t*)) {
  refcounter_init((refcounter_t*) stream);
  stream->force = force;
  stream->type = type;
  stream->pool = pool;
  stream->pipe_notifiers = NULL;
  stream->auto_push = auto_push;
  stream->handlers_lock = platform_mutex_create();
  if (force == push) {
    stream->on_pipe = _readable_push_stream_on_pipe;
    stream->on_piped = _writeable_push_stream_on_piped;
  } else {
    stream->on_pipe = _writeable_pull_stream_on_pipe;
    stream->on_piped = _readable_pull_stream_on_piped;
  }
  stream->destructor = destructor;
  for (size_t i = 0; i < STREAM_HANDLER_COUNT; i++) {
    stream->handlers[i] = stream_event_list_create();
  }
  actor_init(&stream->actor, stream, stream_dispatch, pool);
}

void stream_deinit(stream_t* stream) {
  if (refcounter_count((refcounter_t*) stream) == 0) {
    _stream_purge_handlers(stream);
    if (stream->pipe_notifiers != NULL) {
      free(stream->pipe_notifiers);
      stream->pipe_notifiers = NULL;
    }
    if (stream->pullable_stream != NULL) {
      if (refcounter_dereference_is_zero((refcounter_t*) stream->pullable_stream)) {
        stream_destroy(stream->pullable_stream);
      }
      stream->pullable_stream = NULL;
    }
    actor_destroy(&stream->actor);
    /* actor_destroy has waited for any in-flight dispatch (RUNNING cleared,
       or we are the self-destruct dispatch thread), so no stream_notify can
       still be holding handlers_lock. Safe to tear the lock down now. */
    if (stream->handlers_lock != NULL) {
      platform_mutex_destroy(stream->handlers_lock);
      stream->handlers_lock = NULL;
    }
  }
}

void stream_destroy(stream_t* stream) {
  stream->destructor(stream);
}

void _stream_purge_handlers(stream_t* stream) {
  for (size_t i = 0; i < STREAM_HANDLER_COUNT; i++) {
    stream_event_handler_list_t* list = stream->handlers[i];
    stream->handlers[i] = NULL;
    stream_event_list_destroy(list);
  }
}

void stream_dispatch(void* state, message_t* msg) {
  stream_t* stream = (stream_t*) state;
  switch (msg->type) {
    case READABLE_PUSH:
      if (stream->on_push != NULL) {
        stream->on_push(stream);
      } else {
        stream_notify(stream, error_event, OFFS_ERROR("No Push Handler Defined"), (void (*)(void*))error_destroy);
      }
      break;
    case READABLE_READ: {
      stream_read_message_payload_t* p = (stream_read_message_payload_t*) msg->payload;
      if (stream->on_read != NULL) {
        stream->on_read(stream, p->size, p->ctx, p->cb);
      } else {
        stream_notify(stream, error_event, OFFS_ERROR("No Read Handler Defined"), (void (*)(void*))error_destroy);
      }
      break;
    }
    case WRITEABLE_WRITE: {
      stream_write_payload_t* p = (stream_write_payload_t*) msg->payload;
      if (stream->on_write != NULL) {
        stream->on_write(stream, p->data);
      } else {
        stream_notify(stream, error_event, OFFS_ERROR("No Write Handler Defined"), (void (*)(void*))error_destroy);
      }
      if (stream->is_pulling && stream->pullable_stream != NULL) {
        readable_pull_stream_pull(stream->pullable_stream);
      }
      break;
    }
    case CLOSE_STREAM:
      if (stream->on_close != NULL) {
        stream->on_close(stream);
      } else {
        stream_notify(stream, error_event, OFFS_ERROR("No Close Handler Defined"), (void (*)(void*))error_destroy);
      }
      break;
    case READABLE_PULL:
      if (stream->on_pull != NULL) {
        stream->on_pull(stream);
      } else {
        stream_notify(stream, error_event, OFFS_ERROR("No Readable Pull Handler Defined"), (void (*)(void*))error_destroy);
      }
      break;
    case DEFERRED_DEREF:
      stream->destructor(stream);
      break;
    case STREAM_NOTIFY: {
      stream_notify_payload_t* notify_payload = (stream_notify_payload_t*) msg->payload;
      stream_event_e event = notify_payload->event;
      void* payload = notify_payload->payload;
      void (*payload_destroy)(void*) = notify_payload->payload_destroy;
      /* stream_notify returns 1 (consumed) in the no-handler path, where it
       * destroys the inner payload itself. In that case clear the wrapper's
       * inner pointers so the message's _stream_notify_payload_destroy does
       * not destroy the inner payload a second time (double-destroy /
       * use-after-free on the refcounted error_t). When stream_notify returns 0
       * (has-handler path) it only held a transient reference and the wrapper
       * still owns the inner payload, so leave the pointers for
       * _stream_notify_payload_destroy to release as before. */
      uint8_t consumed = stream_notify(stream, event, payload, payload_destroy);
      if (consumed) {
        notify_payload->payload = NULL;
        notify_payload->payload_destroy = NULL;
      }
      break;
    }
    case STREAM_SET_PULLING: {
      stream_set_pulling_payload_t* p = (stream_set_pulling_payload_t*) msg->payload;
      stream->is_pulling = p->is_pulling;
      if (stream->is_pulling && stream->pullable_stream != NULL) {
        readable_pull_stream_pull(stream->pullable_stream);
      }
      break;
    }
    default:
      break;
  }
}

/* ---- stream notify (replaces VLA with heap alloc) ---- */

uint8_t stream_notify(stream_t* stream, stream_event_e event, void* payload, void (*payload_destroy)(void*)) {
  stream_event_handler_list_t* list = stream->handlers[event];

  /* Snapshot the handler list under handlers_lock so the snapshot array size
     (list->count) and the node walk see a consistent list. A concurrent
     stream_subscribe could otherwise enqueue nodes past the snapshot count and
     the walk would write past the allocation (heap-buffer-overflow), and the
     concurrent linked-list mutation is a data race. Handler callbacks are
     dispatched OUTSIDE the lock (they may re-enter stream ops / send to other
     actors), so the lock is held only for the snapshot and the once-node
     unlink — never across handler dispatch. */
  platform_mutex_lock(stream->handlers_lock);
  size_t count = list->count;
  if (count == 0) {
    platform_mutex_unlock(stream->handlers_lock);
    if (event == error_event) {
      async_error_t* error = (async_error_t*) payload;
      if (error != NULL && error->message != NULL) {
        log_error("Unhandled stream error: %s", error->message);
      } else {
        log_error("Unhandled stream error: (null)");
      }
    }
    if (payload_destroy != NULL) {
      payload_destroy(payload);
    }
    return 1;
  }
  stream_event_handler_t** handlers = get_memory(count * sizeof(stream_event_handler_t*));
  stream_event_handler_list_node_t* current = list->first;
  if ((event == error_event) && (current == NULL)) {
    /* count > 0 but the node list is empty — a list desync; treat as
       no-handler so the error is logged and the payload is released. */
    platform_mutex_unlock(stream->handlers_lock);
    free(handlers);
    async_error_t* error = (async_error_t*) payload;
    if (error != NULL && error->message != NULL) {
      log_error("Unhandled stream error: %s", error->message);
    } else {
      log_error("Unhandled stream error: (null)");
    }
    if (payload_destroy != NULL) {
      payload_destroy(payload);
    }
    return 1;
  }
  /* Hold a reference to the payload so no handler can free it mid-dispatch */
  if (payload != NULL) {
    refcounter_reference((refcounter_t*) payload);
  }
  size_t i = 0;
  uint8_t has_onces = 0;
  while (current != NULL) {
    if (has_onces == 0) {
      has_onces = current->handler->once;
    }
    handlers[i++] = REFERENCE(current->handler, stream_event_handler_t);
    current = current->next;
  }
  platform_mutex_unlock(stream->handlers_lock);

  for (size_t c = 0; c < i; c++) {
    handlers[c]->handler(handlers[c]->ctx, payload);
    DESTROY(handlers[c], stream_event_handler);
  }
  free(handlers);

  if (has_onces == 1) {
    /* Unlink the once-handler nodes under the lock (list mutation), then
       destroy the unlinked handlers OUTSIDE the lock — a handler's
       ctx_destroy may recurse into stream ops and must not re-enter
       handlers_lock. */
    size_t removed_count = 0;
    platform_mutex_lock(stream->handlers_lock);
    stream_event_handler_t** removed = _stream_event_list_unlink_onces(list, &removed_count);
    platform_mutex_unlock(stream->handlers_lock);
    for (size_t c = 0; c < removed_count; c++) {
      stream_event_handler_destroy(removed[c]);
    }
    free(removed);
  }
  /* Release our transient hold on the payload; the caller's reference is still
   * intact, so the caller (or the message wrapper) remains the owner. */
  if (payload_destroy != NULL) {
    payload_destroy(payload);
  }
  return 0;
}

/* ---- stream operations ---- */

static void _stream_notify_payload_destroy(void* ptr) {
  stream_notify_payload_t* payload = (stream_notify_payload_t*)ptr;
  if (payload->payload_destroy != NULL && payload->payload != NULL) {
    payload->payload_destroy(payload->payload);
  }
  free(payload);
}

void stream_deactivate(stream_t* stream, async_error_t* error) {
  stream->is_deactivated = 1;
  stream_notify_payload_t* error_payload = get_clear_memory(sizeof(stream_notify_payload_t));
  error_payload->event = error_event;
  error_payload->payload = (void*) error;
  error_payload->payload_destroy = (void (*)(void*)) error_destroy;
  message_t error_msg;
  error_msg.type = STREAM_NOTIFY;
  error_msg.payload = error_payload;
  error_msg.payload_destroy = _stream_notify_payload_destroy;
  actor_send(&stream->actor, &error_msg);

  stream_notify_payload_t* close_payload = get_clear_memory(sizeof(stream_notify_payload_t));
  close_payload->event = close_event;
  close_payload->payload = NULL;
  close_payload->payload_destroy = NULL;
  message_t close_msg;
  close_msg.type = STREAM_NOTIFY;
  close_msg.payload = close_payload;
  close_msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &close_msg);
}

void stream_deferred_deref(stream_t* stream) {
  // Reference first to prevent premature free, then dereference so the
  // drain's refcounter_dereference_is_zero brings the net count down by one.
  scheduler_pool_defer_cleanup(stream->pool, stream, (void (*)(void*))stream->destructor);
  refcounter_dereference((refcounter_t*)stream);
}

void stream_unsubscribe_pipe_notifiers(stream_t* stream) {
  if (stream->pipe_notifiers != NULL) {
    size_t count = 0;
    switch (stream->type) {
      case readable_stream:
        count = 3;
        break;
      case writeable_stream:
        count = stream->force == push ? 4 : 5;
        break;
      case duplex_stream:
      case transform_stream:
        count = 8;
        break;
      default:
        log_error("stream_unsubscribe_pipe_notifiers: unknown stream type");
        count = 0;
        break;
    }
    for (size_t i = 0; i < count; i++) {
      stream_notifier_t* notifier = &stream->pipe_notifiers[i];
      if (notifier->stream != NULL) {
        stream_unsubscribe(notifier->stream, notifier->event, notifier->id);
        DEREFERENCE(notifier->stream);
      }
    }
    free(stream->pipe_notifiers);
    stream->pipe_notifiers = NULL;
  }
}

void writeable_stream_data_handler(stream_t* stream, void (*on_data)(stream_t*, void*)) {
  if (stream->type == readable_stream) {
    log_error("Only Writeable Stream can set data handlers");
    abort();
  }
  stream->on_data = on_data;
}

void readable_stream_push_handler(stream_t* stream, void (*on_push)(stream_t*)) {
  if (stream->type == writeable_stream || stream->force == pull) {
    log_error("Only Readable Stream can set data handlers");
    abort();
  }
  stream->on_push = on_push;
}

void readable_stream_pull_handler(stream_t* stream, void (*on_pull)(stream_t*)) {
  if (stream->type == writeable_stream || stream->force == push) {
    log_error("Only Readable Stream can set data handlers");
    abort();
  }
  stream->on_pull = on_pull;
}

void writeable_stream_write_handler(stream_t* stream, void (*handler)(stream_t*, void*)) {
  if (stream->type == readable_stream) {
    stream_notify(stream, error_event, OFFS_ERROR("Read Stream cannot set write handlers"), (void (*)(void*))error_destroy);
  } else {
    stream->on_write = handler;
  }
}

void stream_close_handler(stream_t* stream, void (*on_close)(stream_t*)) {
  stream->on_close = on_close;
}

void readable_push_stream_push(stream_t* stream) {
  message_t msg;
  msg.type = READABLE_PUSH;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&stream->actor, &msg);
}

void readable_stream_read(stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  stream_read_message_payload_t* payload = get_clear_memory(sizeof(stream_read_message_payload_t));
  payload->stream = stream;
  payload->size = size;
  payload->ctx = ctx;
  payload->cb = cb;

  message_t msg;
  msg.type = READABLE_READ;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*)) free;

  actor_send(&stream->actor, &msg);
}

void stream_close(stream_t* stream) {
  message_t msg;
  msg.type = CLOSE_STREAM;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&stream->actor, &msg);
}

void writeable_stream_write(stream_t* stream, void* data) {
  stream_write_payload_t* payload = get_clear_memory(sizeof(stream_write_payload_t));
  payload->stream = stream;
  payload->data = data;

  message_t msg;
  msg.type = WRITEABLE_WRITE;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*)) free;

  actor_send(&stream->actor, &msg);
}

void readable_pull_stream_pull(stream_t* stream) {
  if (stream->type == writeable_stream || stream->force == push) {
    stream_notify(stream, error_event, OFFS_ERROR("Invalid Readable Pull Stream"), (void (*)(void*))error_destroy);
    return;
  }

  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&stream->actor, &msg);
}

/* ---- subscribe / unsubscribe ---- */

size_t stream_subscribe(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  size_t id = ++stream->next_handler_id;
  uint8_t push = 0;
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx, handler, ctx_destroy, 0);

  platform_mutex_lock(stream->handlers_lock);
  if ((event == data_event) && (stream->handlers[event]->count == 0)) {
    push = ((!stream->is_piped) && stream->auto_push);
  }
  stream_event_list_enqueue(stream->handlers[event], _handler);
  platform_mutex_unlock(stream->handlers_lock);
  if (push) {
    readable_push_stream_push(stream);
  }
  return id;
}

void stream_unsubscribe(stream_t* stream, stream_event_e event, size_t id) {
  if (stream == NULL) return;
  platform_mutex_lock(stream->handlers_lock);
  stream_event_handler_list_t* list = stream->handlers[event];
  if (list == NULL) {
    platform_mutex_unlock(stream->handlers_lock);
    return;
  }
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  stream_event_handler_list_node_t* node = NULL;
  while (current != NULL) {
    next = current->next;
    if (current->handler->id == id) {
      node = current;
      break;
    }
    current = next;
  }
  stream_event_handler_t* removed = NULL;
  if (node != NULL) {
    /* Unlink under the lock; destroy the handler outside it so the handler's
       ctx_destroy (which may recurse into stream ops) cannot re-enter
       handlers_lock. */
    removed = _stream_event_list_unlink_node(list, node);
  }
  platform_mutex_unlock(stream->handlers_lock);
  if (removed != NULL) {
    stream_event_handler_destroy(removed);
  }
}

size_t stream_once(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  size_t id = ++stream->next_handler_id;
  uint8_t push = 0;
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx, handler, ctx_destroy, 1);

  platform_mutex_lock(stream->handlers_lock);
  if ((event == data_event) && (stream->handlers[event]->count == 1)) {
    push = ((!stream->is_piped) && stream->auto_push);
  }
  stream_event_list_enqueue(stream->handlers[event], _handler);
  platform_mutex_unlock(stream->handlers_lock);
  if (push) {
    readable_push_stream_push(stream);
  }
  return id;
}

void stream_subscribe_internal(stream_t* stream, stream_event_e event, size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once) {
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx, handler, ctx_destroy, once);
  uint8_t push = 0;
  platform_mutex_lock(stream->handlers_lock);
  stream_event_list_enqueue(stream->handlers[event], _handler);
  if ((event == data_event) && (stream->handlers[event]->count == 1) && stream->auto_push && stream->on_push != NULL) {
    push = 1;
  }
  platform_mutex_unlock(stream->handlers_lock);
  if (push) {
    readable_push_stream_push(stream);
  }
}

/* ---- push stream piping ---- */

void readable_push_stream_pipe(stream_t* rs, stream_t* ws) {
  if (rs->type == writeable_stream || rs->force == pull) {
    stream_notify(rs, error_event, OFFS_ERROR("Invalid read stream being piped"), (void (*)(void*))error_destroy);
  } else if (ws->type == readable_stream || ws->force == pull) {
    stream_notify(rs, error_event, OFFS_ERROR("Invalid write stream being piped to"), (void (*)(void*))error_destroy);
  } else {
    rs->on_pipe(rs, ws);
  }
}

void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws) {
  if (rs->is_deactivated == 1) {
    stream_notify(rs, error_event, OFFS_ERROR("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (rs->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (rs->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 4;
          break;
        case duplex_stream:
        case transform_stream:
          size = 7;
          break;
      }
      rs->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    rs->pipe_notifiers[0].event = piped_event;
    rs->pipe_notifiers[0].id = stream_subscribe(ws, piped_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_piped_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[0].stream = REFERENCE(ws, stream_t);
    rs->pipe_notifiers[1].event = error_event;
    rs->pipe_notifiers[1].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_error_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[1].stream = REFERENCE(ws, stream_t);
    rs->pipe_notifiers[2].event = close_event;
    rs->pipe_notifiers[2].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_close_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[2].stream = REFERENCE(ws, stream_t);
    ws->on_piped(ws, rs);
  }
}

void _readable_push_stream_piped_notify(stream_t* stream, void* payload) {
  (void)payload;
  if (stream->on_push != NULL) {
    readable_push_stream_push(stream);
  }
}

void _readable_push_stream_error_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_deactivate(stream, (async_error_t*) payload);
}

void _readable_push_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs) {
  if (ws->type == readable_stream) {
    log_error("Invalid writeable push stream being piped");
    abort();
  }
  if (ws->is_deactivated == 1) {
    stream_notify(ws, error_event, OFFS_ERROR("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (ws->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (ws->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 4;
          break;
        case duplex_stream:
        case transform_stream:
          size = 7;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((ws->type == duplex_stream) || (ws->type == transform_stream)) {
      ws->pipe_notifiers[3].event = error_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[4].event = close_event;
      ws->pipe_notifiers[4].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[4].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[5].event = complete_event;
      ws->pipe_notifiers[5].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[5].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[6].event = data_event;
      ws->pipe_notifiers[6].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_data_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[6].stream = REFERENCE(rs, stream_t);
    } else {
      ws->pipe_notifiers[0].event = error_event;
      ws->pipe_notifiers[0].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[0].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[1].event = close_event;
      ws->pipe_notifiers[1].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[2].event = complete_event;
      ws->pipe_notifiers[2].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[3].event = data_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_data_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    }
  }
}

void _writeable_push_stream_data_notify(stream_t* stream, void* data) {
  REFERENCE((buffer_t*)data, buffer_t);
  writeable_stream_write(stream, data);
}

void _writeable_push_stream_error_notify(stream_t* stream, void* payload) {
  stream_deactivate(stream, (async_error_t*) payload);
}

void _writeable_push_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  if (!stream->is_deactivated) {
    stream_close(stream);
  }
}

void _writeable_push_stream_complete_notify(stream_t* stream, void* payload) {
  (void)stream;
  (void)payload;
}

/* ---- pull stream piping ---- */

void writeable_pull_stream_pipe(stream_t* ws, stream_t* rs) {
  if (rs->type == writeable_stream || rs->force == push) {
    stream_notify(rs, error_event, OFFS_ERROR("Invalid write stream being piped"), (void (*)(void*))error_destroy);
  } else if (ws->type == readable_stream || ws->force == push) {
    stream_notify(rs, error_event, OFFS_ERROR("Invalid read stream being piped to"), (void (*)(void*))error_destroy);
  } else {
    ws->on_pipe(ws, rs);
  }
}

void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs) {
  if (ws->type == readable_stream || ws->force == push) {
    log_error("Invalid readable pull stream being piped");
    abort();
  }
  if (ws->is_deactivated == 1) {
    stream_notify(ws, error_event, OFFS_ERROR("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (ws->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (ws->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 5;
          break;
        case duplex_stream:
        case transform_stream:
          size = 8;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    ws->pipe_notifiers[0].event = piped_event;
    ws->pipe_notifiers[0].id = stream_subscribe(rs, piped_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_piped_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[0].stream = REFERENCE(rs, stream_t);
    ws->pullable_stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[1].event = error_event;
    ws->pipe_notifiers[1].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_error_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[2].event = close_event;
    ws->pipe_notifiers[2].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_close_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[3].event = complete_event;
    ws->pipe_notifiers[3].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_complete_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[4].event = data_event;
    ws->pipe_notifiers[4].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_data_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[4].stream = REFERENCE(rs, stream_t);
    ws->is_piped = 1;
    rs->on_piped(rs, ws);
  }
}

void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws) {
  if (rs->type == writeable_stream) {
    log_error("Invalid writeable push stream being piped");
    abort();
  }
  if (rs->is_deactivated == 1) {
    stream_notify(rs, error_event, OFFS_ERROR("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (rs->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (rs->type) {
        case duplex_stream:
        case transform_stream:
          size = 8;
          break;
        default:
          size = 3;
          break;
      }
      rs->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((rs->type == duplex_stream) || (rs->type == transform_stream)) {
      rs->pipe_notifiers[5].event = error_event;
      rs->pipe_notifiers[5].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[5].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[6].event = finished_event;
      rs->pipe_notifiers[6].id = stream_subscribe(ws, finished_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[6].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[7].event = close_event;
      rs->pipe_notifiers[7].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[7].stream = REFERENCE(ws, stream_t);
    } else {
      rs->pipe_notifiers[0].event = error_event;
      rs->pipe_notifiers[0].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[0].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[1].event = finished_event;
      rs->pipe_notifiers[1].id = stream_subscribe(ws, finished_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[1].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[2].event = close_event;
      rs->pipe_notifiers[2].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[2].stream = REFERENCE(ws, stream_t);
    }
    rs->is_piped = 1;
    stream_notify(rs, piped_event, NULL, NULL);
  }
}

void _readable_pull_stream_error_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_deactivate(stream, (async_error_t*) payload);
}

void _readable_pull_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _readable_pull_stream_finish_notify(stream_t* stream, void* payload) {
  (void)stream;
  (void)payload;
}

void _writeable_pull_stream_error_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_deactivate(stream, (async_error_t*) payload);
}

void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_set_pulling_payload_t* pulling_payload = get_clear_memory(sizeof(stream_set_pulling_payload_t));
  pulling_payload->is_pulling = 1;
  message_t msg;
  msg.type = STREAM_SET_PULLING;
  msg.payload = pulling_payload;
  msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &msg);
}

void _writeable_pull_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_set_pulling_payload_t* pulling_payload = get_clear_memory(sizeof(stream_set_pulling_payload_t));
  pulling_payload->is_pulling = 0;
  message_t msg;
  msg.type = STREAM_SET_PULLING;
  msg.payload = pulling_payload;
  msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &msg);
}

void _writeable_pull_stream_data_notify(stream_t* stream, void* data) {
  REFERENCE((buffer_t*)data, buffer_t);
  writeable_stream_write(stream, data);
}