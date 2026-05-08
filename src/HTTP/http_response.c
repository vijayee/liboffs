//
// Created by victor on 5/7/26.
//
#include "http_response.h"
#include "http_connection.h"
#include "../Util/allocator.h"
#include "../Buffer/buffer.h"
#include <stdio.h>
#include <string.h>

void _http_response_dispatch(void* state, message_t* msg);

static void _send_headers(http_response_t* response) {
  if (response->headers_sent) {
    return;
  }
  response->headers_sent = 1;

  if (http_headers_get(&response->headers, "Content-Length") == NULL) {
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%zu", response->body_length);
    http_headers_set(&response->headers, "Content-Length", content_length_str);
  }

  if (http_headers_get(&response->headers, "Connection") == NULL) {
    http_headers_set(&response->headers, "Connection", "close");
  }

  const char* phrase = http_status_str((enum http_status)response->status_code);
  char status_line[256];
  int line_len = snprintf(status_line, sizeof(status_line),
                           "HTTP/1.1 %d %s\r\n",
                           response->status_code, phrase);
  http_connection_write(response->connection, status_line, (size_t)line_len);

  for (int i = 0; i < response->headers.headers.length; i++) {
    http_header_t* header = &response->headers.headers.data[i];
    char header_line[1024];
    int header_len = snprintf(header_line, sizeof(header_line),
                              "%s: %s\r\n",
                              header->name, header->value);
    http_connection_write(response->connection, header_line, (size_t)header_len);
  }

  http_connection_write(response->connection, "\r\n", 2);
}

http_response_t* http_response_create(scheduler_pool_t* pool, http_connection_t* connection) {
  http_response_t* response = get_clear_memory(sizeof(http_response_t));
  refcounter_init((refcounter_t*)response);
  stream_init((stream_t*)response, push, writeable_stream, 0, pool,
              (void (*)(stream_t*))http_response_destroy);
  response->stream.actor.state = response;
  response->stream.actor.dispatch = _http_response_dispatch;
  response->status_code = HTTP_STATUS_OK;
  http_headers_init(&response->headers);
  response->headers_sent = 0;
  response->connection = connection;
  return response;
}

void http_response_destroy(http_response_t* response) {
  if (response == NULL) {
    return;
  }
  refcounter_dereference((refcounter_t*)response);
  if (refcounter_count((refcounter_t*)response) == 0) {
    http_headers_deinit(&response->headers);
    stream_deinit((stream_t*)response);
    free(response);
  }
}

void _http_response_dispatch(void* state, message_t* msg) {
  http_response_t* response = (http_response_t*)state;
  switch (msg->type) {
    default:
      break;
  }
}

void http_response_set_status(http_response_t* response, uint16_t status) {
  response->status_code = status;
}

void http_response_set_header(http_response_t* response, const char* name, const char* value) {
  http_headers_set(&response->headers, name, value);
}

void http_response_write(http_response_t* response, const char* data, size_t length) {
  if (response->connection == NULL) {
    return;
  }
  response->body_length += length;
  _send_headers(response);
  http_connection_write(response->connection, data, length);
}

void http_response_end(http_response_t* response) {
  if (response->connection == NULL) {
    return;
  }
  _send_headers(response);
  http_connection_close(response->connection);
}

static void _pipe_on_data(void* ctx, void* chunk) {
    http_response_t* response = (http_response_t*)ctx;
    buffer_t* buf = (buffer_t*)chunk;
    http_response_write(response, (const char*)buf->data, buf->size);
}

static void _pipe_on_close(void* ctx, void* unused) {
    (void)unused;
    http_response_t* response = (http_response_t*)ctx;
    http_response_end(response);
    http_response_destroy(response);
}

static void _pipe_on_error(void* ctx, async_error_t* error) {
    (void)error;
    http_response_t* response = (http_response_t*)ctx;
    http_response_set_status(response, 500);
    http_response_end(response);
    http_response_destroy(response);
}

void http_response_pipe(http_response_t* response, stream_t* source) {
    if (!response || !source) return;
    response->is_piped = 1;
    refcounter_reference((refcounter_t*)response);
    stream_subscribe(source, data_event, response,
                     (void(*)(void*, void*))_pipe_on_data, NULL);
    stream_once(source, close_event, response,
                (void(*)(void*, void*))_pipe_on_close, NULL);
    stream_once(source, error_event, response,
                (void(*)(void*, void*))_pipe_on_error, NULL);
}

static const char* _mime_map[][2] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"txt", "text/plain"},
    {"xml", "application/xml"},
    {"md", "text/markdown"},
    {"ofd", "application/cbor"},
    {NULL, NULL}
};

const char* mime_type_from_extension(const char* filename) {
    if (!filename) return "application/octet-stream";
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "application/octet-stream";
    const char* ext = dot + 1;
    for (int i = 0; _mime_map[i][0] != NULL; i++) {
        if (strcasecmp(ext, _mime_map[i][0]) == 0) {
            return _mime_map[i][1];
        }
    }
    return "application/octet-stream";
}