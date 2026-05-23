//
// Created by victor on 5/7/26.
//
#include "http_request.h"
#include "../../Util/allocator.h"
#include <string.h>

void _http_request_dispatch(void* state, message_t* msg);

http_request_t* http_request_create(scheduler_pool_t* pool) {
  http_request_t* request = get_clear_memory(sizeof(http_request_t));
  refcounter_init((refcounter_t*)request);
  stream_init((stream_t*)request, push, readable_stream, 0, pool,
              (void (*)(stream_t*))http_request_destroy);
  request->stream.actor.state = request;
  request->stream.actor.dispatch = _http_request_dispatch;
  http_headers_init(&request->headers);
  vec_init(&request->params);
  request->body = NULL;
  request->content_length = 0;
  request->keep_alive = 0;
  request->headers_complete = 0;
  request->message_complete = 0;
  request->is_authenticated = 0;
  request->method = HTTP_GET;
  request->url = NULL;
  request->path = NULL;
  request->query_string = NULL;
  return request;
}

void http_request_destroy(http_request_t* request) {
  if (request == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)request)) {
    if (request->url != NULL) {
      free(request->url);
    }
    if (request->path != NULL) {
      free(request->path);
    }
    if (request->query_string != NULL) {
      free(request->query_string);
    }
    http_headers_deinit(&request->headers);
    if (request->body != NULL) {
      DESTROY(request->body, buffer);
    }
    vec_capture_deinit(&request->params);
    stream_deinit((stream_t*)request);
    free(request);
  }
}

void _http_request_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
  switch (msg->type) {
    default:
      break;
  }
}

int http_request_method(http_request_t* request) {
  return request->method;
}

const char* http_request_header(http_request_t* request, const char* name) {
  return http_headers_get(&request->headers, name);
}

const char* http_request_param(http_request_t* request, size_t index) {
  if (index < (size_t)request->params.length) {
    return request->params.data[index].match;
  }
  return NULL;
}