//
// Created by victor on 10/12/25.
//
#include "stream.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Workers/error.h"
#include "../RefCounter/refcounter.h"

void _stream_start_message_worker(stream_t* stream);
void _stream_message_worker(stream_t* stream);
void _stream_message_worker_abort(stream_t* stream);
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
void _readable_pull_stream_error_notify(stream_t* stream, void* payload);;
void _readable_pull_stream_close_notify(stream_t* stream, void* payload);
void _readable_pull_stream_finish_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_close_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_data_notify(stream_t* stream, void* data);
void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws);
void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs);

stream_event_handler_t* stream_event_handler_create(size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once) {
  stream_event_handler_t* _handler = get_clear_memory(sizeof(stream_event_handler_t));
  refcounter_init((refcounter_t*) _handler);
  _handler->handler = handler;
  _handler->ctx = ctx;
  _handler->ctx_destroy = ctx_destroy;
  _handler->once = once;
  _handler->id= id;
  return _handler;
}

void stream_event_handler_destroy(stream_event_handler_t* handler) {
  refcounter_dereference((refcounter_t*) handler);
  if (refcounter_count((refcounter_t*) handler) == 0) {
    refcounter_destroy_lock((refcounter_t*) handler);
    if (handler->ctx_destroy != NULL) {
      handler->ctx_destroy(handler->ctx);
    }
    free(handler);
  }
}

stream_event_handler_list_t* stream_event_list_create() {
  stream_event_handler_list_t* list = get_clear_memory(sizeof(stream_event_handler_list_t));
  platform_lock_init(&list->lock);
  list->first = NULL;
  list->last = NULL;
  return list;
}

void stream_event_list_destroy(stream_event_handler_list_t* list) {
  platform_lock_destroy(&list->lock);
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL ) {
    next = current->next;
    stream_event_handler_destroy(current->handler);
    free(current);
    current = next;
  }
  free(list);
}

void stream_event_list_enqueue(stream_event_handler_list_t* list, stream_event_handler_t* event) {
  platform_lock(&list->lock);
  stream_event_handler_list_node_t* node = get_clear_memory(sizeof(stream_event_handler_list_node_t));
  node->handler = event;
  node->previous = NULL;
  node->next = NULL;
  if ((list->last == NULL) && (list->first == NULL)) {
    list->first = node;
    list->last = node;
  } else {
    node->previous = list->last;
    list->last->next= node;
    list->last = node;
  }
  list->count++;
  platform_unlock(&list->lock);
}

stream_event_handler_t* stream_event_list_dequeue(stream_event_handler_list_t* list) {
  platform_lock(&list->lock);
  if ((list->last == NULL) && (list->first == NULL)) {
    platform_unlock(&list->lock);
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
    platform_unlock(&list->lock);
    return event;
  }
}

void stream_event_list_remove(stream_event_handler_list_t* list, stream_event_handler_list_node_t* node) {
  platform_lock(&list->lock);
  if ((list->last == NULL) && (list->first == NULL)) {
    platform_unlock(&list->lock);
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
  platform_unlock(&list->lock);
}

void stream_event_list_remove_onces(stream_event_handler_list_t* list) {
  platform_lock(&list->lock);
  if ((list->last == NULL) && (list->first == NULL)) {
    platform_unlock(&list->lock);
    return;
  }
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL) {
    next= current->next;
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
  platform_unlock(&list->lock);
}

message_queue_t* message_create() {
  message_queue_t* queue = get_clear_memory(sizeof(message_queue_t));
  platform_lock_init(&queue->lock);
  queue->first = NULL;
  queue->last = NULL;
  return queue;
}

void message_queue_destroy(message_queue_t* queue) {
  message_list_node_t* current = queue->first;
  message_list_node_t* next = NULL;
  while (current != NULL ) {
    next = current->next;
    free(current->message);
    free(current);
    current = next;
  }
  platform_lock_destroy(&queue->lock);
  free(queue);
}


void message_queue_enqueue(message_queue_t* queue, message_t* message) {
  platform_lock(&queue->lock);
  message_list_node_t* node = get_clear_memory(sizeof(message_list_node_t));
  node->message = message;
  node->previous = NULL;
  node->next = NULL;
  if ((queue->last == NULL) && (queue->first == NULL)) {
    queue->first = node;
    queue->last = node;
  } else {
    node->previous = queue->last;
    queue->last->next= node;
    queue->last = node;
  }
  queue->count++;
  platform_unlock(&queue->lock);
}


message_t* message_queue_dequeue(message_queue_t* queue) {
  platform_lock(&queue->lock);
  if ((queue->last == NULL) && (queue->first == NULL)) {
    platform_unlock(&queue->lock);
    return NULL;
  } else {
    message_list_node_t* node = queue->first;
    queue->first = node->next;
    if (node->next != NULL) {
      queue->first->previous = NULL;
    }
    if (queue->last == node) {
      queue->last = NULL;
    }
    message_t* message = node->message;
    free(node);
    queue->count--;
    platform_unlock(&queue->lock);
    return message;
  }
}

void stream_init(stream_t* stream, stream_force_e force, stream_type_e type, priority_t* priority, uint8_t auto_push, work_pool_t* pool, void (*destructor)(stream_t*)) {
  refcounter_init(&stream->refcounter);
  platform_lock_init(&stream->lock);
  platform_lock_init(&stream->worker_status.lock);
  stream->force = force;
  stream->type = type;
  stream->priority = *priority;
  stream->pool = pool;
  stream->queue = message_create();
  stream->pipe_notifiers = NULL;
  stream->auto_push = auto_push;
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
}
void stream_deinit(stream_t* stream) {
  if (refcounter_count((refcounter_t*) stream) == 0) {
    platform_lock_destroy(&stream->lock);
    platform_lock_destroy(&stream->worker_status.lock);
    message_queue_destroy(stream->queue);
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
          count = 7;
          break;
        default:
          log_error("stream_deinit: unknown stream type");
          count = 0;
          break;
      }
    }
    _stream_purge_handlers(stream);
    if (stream->pullable_stream != NULL) {
      stream->pullable_stream->destructor(stream->pullable_stream);
      stream->pullable_stream = NULL;
    }
  }
}
void stream_destroy(stream_t* stream) {
  stream->destructor(stream);
}

void _stream_purge_handlers(stream_t* stream) {
  for (size_t i = 0; i < STREAM_HANDLER_COUNT; i++) {
    stream_event_list_destroy(stream->handlers[i]);
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

void readable_push_stream_pipe(stream_t* rs, stream_t* ws) {
  if (rs->type == writeable_stream || rs->force == pull) {
    stream_notify(rs, error_event, ERROR("Invalid read stream being piped"));
  } if (ws->type == readable_stream || ws->force == pull) {
    stream_notify(rs, error_event, ERROR("Invalid write stream being piped to"));
  } else {
    rs->on_pipe(rs,ws);
  }
}
void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws) {
  platform_lock(&rs->lock);
  if (rs->is_deactivated == 1) {
    platform_unlock(&rs->lock);
    stream_notify(rs, error_event, ERROR("Stream has been destroyed"));
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
        case duplex_stream :
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
    platform_unlock(&rs->lock);
    ws->on_piped(ws, rs);
  }
}
void _readable_push_stream_piped_notify(stream_t* stream, void* payload) {
  readable_push_stream_push(stream);
}
void _readable_push_stream_error_notify(stream_t* stream, void* payload) {
  stream_deactivate(stream, (async_error_t*) payload);
}

void _readable_push_stream_close_notify(stream_t* stream, void* payload) {
  stream_close(stream);
}

void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs) {
  platform_lock(&ws->lock);
  if(ws->type == readable_stream){
    log_error("Invalid writeable push stream being piped ");
    abort();
  }
  if (ws->is_deactivated == 1) {
    platform_unlock(&ws->lock);
    stream_notify(ws, error_event, ERROR("Stream has been destroyed"));
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
        case duplex_stream :
        case transform_stream:
          size = 7;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((ws->type == duplex_stream)||(ws->type == transform_stream)) {
      ws->pipe_notifiers[3].event = error_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[4].event = close_event;
      ws->pipe_notifiers[4].id = stream_subscribe(rs, close_event,REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
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
      ws->pipe_notifiers[1].id = stream_subscribe(rs, close_event,REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[2].event = complete_event;
      ws->pipe_notifiers[2].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[3].event = data_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_data_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    }
    platform_unlock(&ws->lock);
  }
}
void _writeable_push_stream_data_notify(stream_t* stream, void* data) {
  writeable_stream_write(stream, data);
}
void _writeable_push_stream_error_notify(stream_t* stream, void* payload) {
  stream_deactivate(stream, (async_error_t*) payload);
}

void _writeable_push_stream_close_notify(stream_t* stream, void* payload) {
  // Only close if not already deactivated
  if (!stream->is_deactivated) {
    stream_close(stream);
  }
}

void _writeable_push_stream_complete_notify(stream_t* stream, void* payload) {
  // Only close if not already deactivated
}

void stream_unsubscribe_pipe_notifiers(stream_t* stream) {
  platform_lock(&stream->lock);
  if ( stream->pipe_notifiers != NULL ) {
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
        count = 7;
        break;
      default:
        log_error("stream_unsubscribe_pipe_notifiers: unknown stream type");
        count = 0;
        break;
    }
    for (size_t i = 0; i < count; i++) {
      stream_notifier_t* notifier = &stream->pipe_notifiers[i];
      // Unsubscribe the event handler - this will decrement the reference
      // that was created when the handler was subscribed (via REFERENCE macro)
      // and call ctx_destroy if the handler is destroyed
      stream_unsubscribe(notifier->stream, notifier->event, notifier->id);
      // Dereference the stream stored in pipe_notifiers
      // This decrements the reference created by REFERENCE(ws/rs) when piping
      notifier->stream->destructor(notifier->stream);
      notifier->stream = NULL;
    }
    free(stream->pipe_notifiers);
    stream->pipe_notifiers = NULL;
  }
  platform_unlock(&stream->lock);
}

void stream_notify(stream_t* stream, stream_event_e event, void* payload) {
  stream_event_handler_list_t* list = stream->handlers[event];
  platform_lock(&list->lock);
  stream_event_handler_t* handlers[list->count];
  stream_event_handler_list_node_t* current = list->first;
  if ((event == error_event) && (current == NULL)) {
    log_error("No error event handler defined");
    async_error_t* error = (async_error_t*) payload;
    log_error(error->message);
    abort();
  }
  size_t i = 0;
  uint8_t has_onces = 0;
  while (current != NULL) {
    if (has_onces == 0) {
      has_onces = current->handler->once; ;
    }
    handlers[i++] = REFERENCE(current->handler, stream_event_handler_t);
    current = current->next;
  }
  platform_unlock(&list->lock);
  for (size_t c = 0; c < i; c++) {
    handlers[c]->handler(handlers[c]->ctx, payload);
    DESTROY(handlers[c], stream_event_handler);
  }
  if (has_onces == 1) {
    stream_event_list_remove_onces(list);
  }
}

void stream_deactivate(stream_t* stream, async_error_t* error) {
  platform_lock(&stream->lock);
  stream->is_deactivated = 1;
  platform_unlock(&stream->lock);
  stream_notify(stream, error_event, (void*) error);
}

void readable_push_stream_on_piped(stream_t* stream, void* payload) {
  readable_push_stream_push(stream);
}

void readable_push_stream_push(stream_t* stream) {
  message_t* message = get_memory(sizeof(message_t));
  message->ctx = NULL;
  message->payload= NULL;
  message->type = readable_push;
  message_queue_enqueue(stream->queue, message);
  _stream_start_message_worker(stream);
}
void _stream_start_message_worker(stream_t* stream) {
  platform_lock(&stream->worker_status.lock);
  if (stream->worker_status.is_working == 0) {
    stream->worker_status.is_working = 1;
    platform_unlock(&stream->worker_status.lock);
    REFERENCE(stream, stream_t);
    YIELD(stream);
    work_t *work = work_create(&stream->priority, (void *)stream, (void *) _stream_message_worker,
                               (void *) _stream_message_worker_abort);
    work_pool_enqueue(stream->pool, CONSUME(work, work_t));
  } else {
    platform_unlock(&stream->worker_status.lock);
  }
}
void _stream_message_worker(stream_t* stream) {
  REFERENCE(stream, stream_t);
  message_t* current = message_queue_dequeue(stream->queue);
  if (current != NULL) {
    switch(current->type) {
      case readable_push:
        if (stream->on_push != NULL) {
          stream->on_push(stream);
        } else {
          stream_notify(stream, error_event, ERROR("No Push Handler Defined"));
        }
        break;
      case readable_read:
        if (stream->on_read != NULL) {
          read_payload* payload = current->payload;
          stream->on_read(stream, payload->size, current->ctx, payload->cb);
          free(payload);
        } else {
          stream_notify(stream, error_event, ERROR("No Read Handler Defined"));
        }
        break;
      case writeable_write:
        if (stream->on_write != NULL) {
          stream->on_write(stream, current->payload);
        } else {
          stream_notify(stream, error_event, ERROR("No Write Handler Defined"));
        }
        break;
      case close_stream:
        if (stream->on_close != NULL) {
          stream->on_close(stream);
        } else {
          stream_notify(stream, error_event, ERROR("No Close Handler Defined"));
        }
        break;
      case readable_pull:
        if (stream->on_pull != NULL) {
          stream->on_pull(stream);
        } else {
          stream_notify(stream, error_event, ERROR("No Readable Pull Handler Defined"));
        }
        break;
    }
    free(current);
    YIELD(stream);
    work_t* work = work_create(&stream->priority, (void*) stream, (void*)_stream_message_worker, (void*)_stream_message_worker_abort);
    work_pool_enqueue(stream->pool, CONSUME(work, work_t));
  } else {
    platform_lock(&stream->worker_status.lock);
    stream->worker_status.is_working = 0;
    platform_unlock(&stream->worker_status.lock);
    DESTROY(stream, stream);
  }
}

void _stream_message_worker_abort(stream_t* stream) {
  REFERENCE(stream, stream_t);
  stream->destructor(stream);
}

size_t stream_subscribe(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  platform_lock(&stream->lock);
  size_t id = ++stream->next_handler_id;
  uint8_t push = 0;
  if((event == data_event) && (stream->handlers[event]->count == 0)) {
    push = ((!stream->is_piped) && stream->auto_push);
  }
  platform_unlock(&stream->lock);
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx,handler,ctx_destroy, 0);

  stream_event_list_enqueue(stream->handlers[event], _handler);
  if (push) {
    readable_push_stream_push(stream);
  }
  return id;
}

void stream_unsubscribe(stream_t* stream, stream_event_e event, size_t id) {
  stream_event_handler_list_t* list = stream->handlers[event];
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next= NULL;
  stream_event_handler_list_node_t* node= NULL;
  platform_lock(&list->lock);
  while (current != NULL) {
    next = current->next;
    if (current->handler->id == id) {
      node = current;
      break;
    }
    current = next;
  }
  platform_unlock(&list->lock);
  if (node != NULL) {
    stream_event_list_remove(list, node);
  }
}

size_t stream_once(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  platform_lock(&stream->lock);
  size_t id = ++stream->next_handler_id;
  uint8_t push = 0;
  if((event == data_event) && (stream->handlers[event]->count == 1)) {
    push = ((!stream->is_piped) && stream->auto_push);
  }
  platform_unlock(&stream->lock);
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx,handler,ctx_destroy, 1);

  stream_event_list_enqueue(stream->handlers[event], _handler);
  if (push) {
    readable_push_stream_push(stream);
  }
  return id;
}


void readable_stream_read(stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  message_t* message = get_memory(sizeof(message_t));
  read_payload* payload = get_clear_memory(sizeof(read_payload));
  message->ctx = ctx;
  message->payload = payload;
  message->type = readable_read;
  message_queue_enqueue(stream->queue, message);
  _stream_start_message_worker(stream);
}

void stream_close_handler(stream_t* stream, void (*on_close)(stream_t*)) {
  platform_lock(&stream->lock);
  stream->on_close = on_close;
  platform_unlock(&stream->lock);
}

void stream_close(stream_t* stream) {
  message_t* message = get_memory(sizeof(message_t));
  message->ctx = stream;
  message->payload = NULL;
  message->type = close_stream;
  message_queue_enqueue(stream->queue, message);
  _stream_start_message_worker(stream);
}

void writeable_stream_write_handler(stream_t* stream, void (*handler)(stream_t*, void*)) {
  if(stream->type == readable_stream) {
    stream_notify(stream, error_event, ERROR("Read Stream cannot set write hanlders"));
  } else {
    stream->on_write = handler;
  }
}

void writeable_stream_write(stream_t* stream, void* data) {
  message_t* message = get_memory(sizeof(message_t));
  message->ctx = NULL;
  message->payload = data;
  message->type = writeable_write;
  message_queue_enqueue(stream->queue, message);
  platform_lock(&stream->worker_status.lock);
  if (stream->worker_status.is_working == 0) {
    platform_unlock(&stream->worker_status.lock);
    _stream_start_message_worker(stream);
  } else {
    platform_unlock(&stream->worker_status.lock);
  }
}

void readable_pull_stream_pull(stream_t* stream) {
  platform_lock(&stream->lock);
  if (stream->type == writeable_stream  || stream->force == push) {
    platform_unlock(&stream->lock);
    stream_notify(stream, error_event, ERROR("Invalid Readable Pull Stream"));
    return;
  }
  platform_unlock(&stream->lock);
  message_t* message = get_memory(sizeof(message_t));
  message->ctx = NULL;
  message->type = readable_pull;
  message_queue_enqueue(stream->queue, message);
  platform_lock(&stream->worker_status.lock);
  if (stream->worker_status.is_working == 0) {
    platform_unlock(&stream->worker_status.lock);
    _stream_start_message_worker(stream);
  } else {
    platform_unlock(&stream->worker_status.lock);
  }
}
void writeable_pull_stream_pipe(stream_t* ws, stream_t* rs) {
  if (rs->type == writeable_stream || rs->force == push) {
    stream_notify(rs, error_event, ERROR("Invalid write stream being piped"));
  } if (ws->type == readable_stream || ws->force == push) {
    stream_notify(rs, error_event, ERROR("Invalid read stream being piped to"));
  } else {
    ws->on_pipe(ws,rs);
  }
}

void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs) {
  platform_lock(&ws->lock);
  if(ws->type == readable_stream || ws->force == push) {
    log_error("Invalid readable pull stream being piped");
    abort();
  }
  if (ws->is_deactivated == 1) {
    platform_unlock(&ws->lock);
    stream_notify(ws, error_event, ERROR("Stream has been destroyed"));
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
        case duplex_stream :
        case transform_stream:
          size = 7;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    ws->pipe_notifiers[0].event = piped_event;
    ws->pipe_notifiers[0].id = stream_subscribe(rs, piped_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_piped_notify, (void (*)(void*))rs->destructor);
    ws->pipe_notifiers[0].stream = REFERENCE(rs, stream_t);
    ws->pullable_stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[1].event = error_event;
    ws->pipe_notifiers[1].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
    ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[2].event = close_event;
    ws->pipe_notifiers[2].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
    ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[3].event = complete_event;
    ws->pipe_notifiers[3].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_complete_notify, (void (*)(void*))rs->destructor);
    ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[4].event = data_event;
    ws->pipe_notifiers[4].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_data_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[4].stream = REFERENCE(rs, stream_t);
    ws->is_piped = 1;
    platform_unlock(&ws->lock);
    rs->on_piped(rs, ws);
  }
}
void readable_stream_pull_handler(stream_t* stream, void (*on_pull)(stream_t*)) {
  if (stream->type == writeable_stream || stream->force == push) {
    log_error("Only Readable Stream can set data handlers");
    abort();
  }
  stream->on_pull = on_pull;
}

void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws) {
  platform_lock(&rs->lock);
  if(rs->type == writeable_stream){
    log_error("Invalid writeable push stream being piped ");
    abort();
  }
  if (rs->is_deactivated == 1) {
    platform_unlock(&rs->lock);
    stream_notify(rs, error_event, ERROR("Stream has been destroyed"));
  } else {
    if (rs->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (ws->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 5;
          break;
        case duplex_stream :
        case transform_stream:
          size = 7;
          break;
      }
      rs->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((rs->type == duplex_stream)||(rs->type == transform_stream)) {
      rs->pipe_notifiers[5].event = error_event;
      rs->pipe_notifiers[5].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[5].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[6].event = finished_event;
      rs->pipe_notifiers[6].id = stream_subscribe(ws, finished_event,REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[6].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[7].event = close_event;
      rs->pipe_notifiers[7].id = stream_subscribe(ws, close_event,REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[7].stream = REFERENCE(ws, stream_t);

    } else {
      rs->pipe_notifiers[0].event = error_event;
      rs->pipe_notifiers[0].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[0].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[1].event = finished_event;
      rs->pipe_notifiers[1].id = stream_subscribe(ws, finished_event,REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[1].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[2].event = close_event;
      rs->pipe_notifiers[2].id = stream_subscribe(ws, close_event,REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[2].stream = REFERENCE(ws, stream_t);
    }
    rs->is_piped = 1;
    platform_unlock(&rs->lock);
    stream_notify(rs, piped_event, NULL);
  }
}

void _readable_pull_stream_error_notify(stream_t* stream, void* payload) {
  stream_deactivate(stream, (async_error_t*) payload);
}

void _readable_pull_stream_close_notify(stream_t* stream, void* payload) {
  stream_close(stream);
}

void _readable_pull_stream_finish_notify(stream_t* stream, void* payload) {

}

void _writeable_pull_stream_error_notify(stream_t* stream, void* payload) {
  stream_deactivate(stream, (async_error_t*) payload);
}

void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload) {
  platform_lock(&stream->lock);
  stream->is_pulling = 1;
  platform_unlock(&stream->lock);
  readable_pull_stream_pull(stream->pullable_stream);
}

void _writeable_pull_stream_close_notify(stream_t* stream, void* payload) {
  stream_close(stream);
}
void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload) {
  platform_lock(&stream->lock);
  stream->is_pulling = 0;
  platform_unlock(&stream->lock);
}
void _writeable_pull_stream_data_notify(stream_t* stream, void* data) {
  writeable_stream_write(stream, data);
  platform_lock(&stream->lock);
  uint8_t is_pulling = stream->is_pulling;
  platform_unlock(&stream->lock);
  if (is_pulling) {
    readable_pull_stream_pull(stream->pullable_stream);
  }
}
/*
void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs) {
  platform_lock(&ws->lock);
  if (ws->is_deactivated == 1) {
    platform_unlock(&ws->lock);
    stream_notify(ws, error_event, ERROR("Stream has been destroyed"));
  } else {
    ws->pipe_notifiers[1].event = error_event;
    ws->pipe_notifiers[1].id = stream_subscribe(rs, error_event,REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[1].event = complete_event;
    ws->pipe_notifiers[1].id = stream_subscribe(rs, complete_event,REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[2].event = close_event;
    ws->pipe_notifiers[2].id = stream_subscribe(rs, close_event,REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
  }
}*/