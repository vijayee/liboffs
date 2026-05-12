//
// Created by victor on 5/7/26.
//
#include "http_connection.h"
#include "http_server.h"
#include "http_request.h"
#include "http_response.h"
#include "http_route.h"
#include "../Util/allocator.h"
#include "../Buffer/buffer.h"
#include "../Streams/stream.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include <stdio.h>

#define READ_BUFFER_SIZE 4096

static int _on_message_begin(http_parser* parser);
static int _on_url(http_parser* parser, const char* at, size_t length);
static int _on_header_field(http_parser* parser, const char* at, size_t length);
static int _on_header_value(http_parser* parser, const char* at, size_t length);
static int _on_headers_complete(http_parser* parser);
static int _on_body(http_parser* parser, const char* at, size_t length);
static int _on_message_complete(http_parser* parser);

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data);

static http_parser_settings _parser_settings = {
  .on_message_begin = _on_message_begin,
  .on_url = _on_url,
  .on_status = NULL,
  .on_header_field = _on_header_field,
  .on_header_value = _on_header_value,
  .on_headers_complete = _on_headers_complete,
  .on_body = _on_body,
  .on_message_complete = _on_message_complete,
  .on_chunk_header = NULL,
  .on_chunk_complete = NULL
};

static void _reset_header_accumulator(http_connection_t* connection) {
  if (connection->header_field != NULL) {
    connection->header_field[0] = '\0';
    connection->header_field_len = 0;
  }
  if (connection->header_value != NULL) {
    connection->header_value[0] = '\0';
    connection->header_value_len = 0;
  }
}

static void _accumulate_field(http_connection_t* connection, const char* at, size_t length) {
  if (connection->header_field == NULL) {
    connection->header_field_cap = length * 2 + 1;
    connection->header_field = get_memory(connection->header_field_cap);
  } else if (connection->header_field_len + length + 1 > connection->header_field_cap) {
    connection->header_field_cap = (connection->header_field_len + length) * 2 + 1;
    connection->header_field = realloc(connection->header_field, connection->header_field_cap);
  }
  memcpy(connection->header_field + connection->header_field_len, at, length);
  connection->header_field_len += length;
  connection->header_field[connection->header_field_len] = '\0';
}

static void _accumulate_value(http_connection_t* connection, const char* at, size_t length) {
  if (connection->header_value == NULL) {
    connection->header_value_cap = length * 2 + 1;
    connection->header_value = get_memory(connection->header_value_cap);
  } else if (connection->header_value_len + length + 1 > connection->header_value_cap) {
    connection->header_value_cap = (connection->header_value_len + length) * 2 + 1;
    connection->header_value = realloc(connection->header_value, connection->header_value_cap);
  }
  memcpy(connection->header_value + connection->header_value_len, at, length);
  connection->header_value_len += length;
  connection->header_value[connection->header_value_len] = '\0';
}

static char* _url_decode(const char* src, size_t length) {
  char* decoded = get_memory(length + 1);
  size_t decoded_len = 0;
  for (size_t i = 0; i < length; i++) {
    if (src[i] == '%' && i + 2 < length && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
      char hex[3] = {src[i + 1], src[i + 2], '\0'};
      decoded[decoded_len++] = (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (src[i] == '+') {
      decoded[decoded_len++] = ' ';
    } else {
      decoded[decoded_len++] = src[i];
    }
  }
  decoded[decoded_len] = '\0';
  return decoded;
}

static void _flush_header(http_connection_t* connection) {
  if (connection->header_field != NULL && connection->header_field_len > 0 &&
      connection->header_value != NULL && connection->header_value_len > 0) {
    http_headers_set(&connection->request->headers, connection->header_field, connection->header_value);
  }
  _reset_header_accumulator(connection);
}

static int _on_message_begin(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (connection->request != NULL) {
    DESTROY(connection->request, http_request);
  }
  connection->request = http_request_create(connection->server->pool);
  _reset_header_accumulator(connection);
  return 0;
}

static int _on_url(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (connection->request->url == NULL) {
    connection->request->url = get_memory(length + 1);
    memcpy(connection->request->url, at, length);
    connection->request->url[length] = '\0';
  } else {
    size_t current_len = strlen(connection->request->url);
    connection->request->url = realloc(connection->request->url, current_len + length + 1);
    memcpy(connection->request->url + current_len, at, length);
    connection->request->url[current_len + length] = '\0';
  }
  return 0;
}

static int _on_header_field(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (connection->header_field_len > 0 && connection->header_value_len > 0) {
    _flush_header(connection);
  }
  _accumulate_field(connection, at, length);
  return 0;
}

static int _on_header_value(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  _accumulate_value(connection, at, length);
  return 0;
}

static int _on_headers_complete(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  _flush_header(connection);
  connection->request->method = parser->method;
  connection->request->content_length = parser->content_length;
  connection->request->keep_alive = http_should_keep_alive(parser);
  connection->headers_complete = 1;

  if (connection->request->url != NULL) {
    char* url = connection->request->url;
    char* query = strchr(url, '?');
    if (query != NULL) {
      size_t path_len = query - url;
      char* raw_path = get_memory(path_len + 1);
      memcpy(raw_path, url, path_len);
      raw_path[path_len] = '\0';
      connection->request->path = _url_decode(raw_path, path_len);
      free(raw_path);
      connection->request->query_string = _url_decode(query + 1, strlen(query + 1));
    } else {
      connection->request->path = _url_decode(url, strlen(url));
    }
  }

  // Try early route match for streaming PUT
  if (connection->request->path != NULL && connection->request->method == HTTP_PUT) {
    http_route_t* route = http_server_match_route(connection->server,
                                                   connection->request->method,
                                                   connection->request->path);
    if (route != NULL && route->headers_complete_handler != NULL) {
      connection->streaming_route = route;
      http_response_t* response = http_response_create(connection->server->pool, connection);
      int streaming = route->headers_complete_handler(connection, connection->request, response);
      if (streaming) {
        // Pipeline owns the response via its extra ref.
        // Release the create-ref — pipeline's cleanup in
        // _put_on_descriptor_close will drop its ref, freeing the response.
        http_response_destroy(response);
      } else {
        // Handler declined streaming (e.g., multipart). Fall through to normal path.
        connection->streaming_route = NULL;
        DESTROY(response, http_response);
      }
    }
  }

  return 0;
}

static int _on_body(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;

  // Streaming mode: pipe body chunks directly into the request stream
  if (connection->streaming_route != NULL) {
    buffer_t* chunk = buffer_create_from_pointer_copy((uint8_t*)at, length);
    stream_notify((stream_t*)connection->request, data_event,
                  CONSUME(chunk, buffer_t), (void (*)(void*))buffer_destroy);
    return 0;
  }

  if (connection->request->body == NULL) {
    size_t initial_capacity = connection->request->content_length > 0
      ? (size_t)connection->request->content_length
      : (length < 8192 ? 8192 : length * 2);
    connection->request->body = buffer_create_with_capacity(0, initial_capacity);
    buffer_ensure_capacity(connection->request->body, length);
    memcpy(connection->request->body->data, at, length);
    connection->request->body->size = length;
  } else {
    size_t needed = connection->request->body->size + length;
    buffer_ensure_capacity(connection->request->body, needed);
    memcpy(connection->request->body->data + connection->request->body->size, at, length);
    connection->request->body->size = needed;
  }
  return 0;
}

static int _on_message_complete(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  connection->request_complete = 1;

  // Streaming mode: signal end of body data and skip normal dispatch
  if (connection->streaming_route != NULL) {
    stream_notify((stream_t*)connection->request, close_event, NULL, NULL);
    connection->streaming_route = NULL;
    DESTROY(connection->request, http_request);
    connection->request = NULL;
    http_parser_init(&connection->parser, HTTP_REQUEST);
    connection->headers_complete = 0;
    connection->request_complete = 0;
    return 0;
  }

  http_response_t* response = http_response_create(
    connection->server->pool, connection);

  http_server_dispatch(connection->server, connection->request, response);

  if (response->is_piped) {
    // Piped responses are async — the pipeline will call http_response_end
    // and http_response_destroy when the stream closes. Skip cleanup here.
    // Still release the initial ref from create; the pipe holds its own ref.
    http_response_destroy(response);
  } else {
    DESTROY(response, http_response);

    if (!connection->request->keep_alive) {
      shutdown(connection->fd, SHUT_WR);
    }
  }

  DESTROY(connection->request, http_request);
  connection->request = NULL;

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->headers_complete = 0;
  connection->request_complete = 0;
  return 0;
}

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                     pd_event_t events, void* user_data) {
  http_connection_t* connection = (http_connection_t*)user_data;
  char buffer[READ_BUFFER_SIZE];

  if (events & PD_EVENT_READ) {
    ssize_t bytes_read;
    if (connection->is_ssl && connection->ssl != NULL) {
      bytes_read = SSL_read(connection->ssl, buffer, sizeof(buffer));
      if (bytes_read <= 0) {
        int ssl_error = SSL_get_error(connection->ssl, (int)bytes_read);
        if (ssl_error == SSL_ERROR_WANT_READ) {
          return;
        }
        goto close;
      }
    } else {
      bytes_read = recv(connection->fd, buffer, sizeof(buffer), 0);
      if (bytes_read <= 0) {
        if (bytes_read == 0) {
          goto close;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        goto close;
      }
    }

    size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                          buffer, (size_t)bytes_read);
    if (connection->parser.http_errno != HPE_OK) {
      goto close;
    }
    (void)nparsed;
  }

  if (events & PD_EVENT_HANGUP) {
    goto close;
  }

  if (events & PD_EVENT_ERROR) {
    goto close;
  }

  return;

close:
  if (connection->piped_pending) {
    // A piped response or streaming upload is still in progress.
    // Emit error on request stream so the pipeline can clean up.
    if (connection->streaming_route != NULL && connection->request != NULL) {
      stream_deactivate((stream_t*)connection->request,
                        ERROR("Connection closed during streaming upload"));
      connection->streaming_route = NULL;
    }
    // Close the fd so the piped write path detects the broken connection.
    // The worker thread will get EPIPE on send() and stop writing.
    if (connection->fd >= 0) {
      close(connection->fd);
      connection->fd = -1;
    }
    // Stop and destroy the read watcher on the event loop thread
    // (safe here) and release our reference to the connection.
    // The worker thread holds the other reference and will free
    // the connection when it finishes.
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
    connection->watcher = NULL;
    http_connection_destroy(connection);
    return;
  }
  pd_watcher_stop(watcher);
  pd_watcher_destroy(watcher);
  connection->watcher = NULL;
  if (connection->fd >= 0) {
    close(connection->fd);
    connection->fd = -1;
  }
  DESTROY(connection, http_connection);
}

http_connection_t* http_connection_create(http_server_t* server, int fd) {
  http_connection_t* connection = get_clear_memory(sizeof(http_connection_t));
  refcounter_init((refcounter_t*)connection);
  connection->server = server;
  connection->fd = fd;
  connection->ssl = NULL;
  connection->is_ssl = 0;
  connection->headers_complete = 0;
  connection->request_complete = 0;
  connection->request = NULL;
  connection->write_buffer = NULL;
  connection->header_field = NULL;
  connection->header_field_len = 0;
  connection->header_field_cap = 0;
  connection->header_value = NULL;
  connection->header_value_len = 0;
  connection->header_value_cap = 0;
  connection->streaming_route = NULL;

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->parser.data = connection;

  connection->watcher = pd_watcher_create(server->loop, fd,
    PD_EVENT_READ, _connection_read_callback, connection);
  if (connection->watcher != NULL) {
    pd_watcher_start(connection->watcher);
  }

  return connection;
}

void http_connection_destroy(http_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)connection)) {
    if (connection->server != NULL) {
      atomic_fetch_sub(&connection->server->active_connections, 1);
      vec_remove(&connection->server->connections, connection);
    }
    if (connection->watcher != NULL) {
      pd_watcher_stop(connection->watcher);
      pd_watcher_destroy(connection->watcher);
    }
    if (connection->fd >= 0) {
      close(connection->fd);
    }
    if (connection->request != NULL) {
      DESTROY(connection->request, http_request);
    }
    if (connection->write_buffer != NULL) {
      DESTROY(connection->write_buffer, buffer);
    }
    if (connection->header_field != NULL) {
      free(connection->header_field);
    }
    if (connection->header_value != NULL) {
      free(connection->header_value);
    }
    if (connection->ssl != NULL) {
      SSL_free(connection->ssl);
    }
    free(connection);
  }
}

void http_connection_write(http_connection_t* connection, const char* data, size_t length) {
  if (connection == NULL || connection->fd < 0) {
    return;
  }

  size_t written = 0;
  while (written < length) {
    ssize_t result;
    if (connection->is_ssl && connection->ssl != NULL) {
      result = SSL_write(connection->ssl, data + written, (int)(length - written));
      if (result <= 0) {
        int ssl_error = SSL_get_error(connection->ssl, (int)result);
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
          continue;
        }
        break;
      }
    } else {
      result = send(connection->fd, data + written, length - written, MSG_NOSIGNAL);
      if (result <= 0) {
        if (result == 0) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          struct pollfd poll_fd;
          poll_fd.fd = connection->fd;
          poll_fd.events = POLLOUT;
          poll(&poll_fd, 1, 1000);
          continue;
        }
        break;
      }
    }
    written += (size_t)result;
  }
}

void http_connection_close(http_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  if (connection->fd >= 0) {
    shutdown(connection->fd, SHUT_WR);
  }
}