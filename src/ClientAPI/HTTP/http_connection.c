//
// Created by victor on 5/7/26.
//
#include "http_connection.h"
#include "http_server.h"
#include "http_request.h"
#include "http_response.h"
#include "http_route.h"
#include "../../Util/allocator.h"
#include "../../Buffer/buffer.h"
#include "../../Streams/stream.h"
#include "../../Util/validation.h"
#include "../../Actor/actor.h"
#include "../../Actor/message.h"
#include "../../Scheduler/scheduler.h"
#include <string.h>
#ifdef _WIN32
  #include "../../Platform/platform_posix_compat.h"
#else
  #include <unistd.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <stdio.h>

#define READ_BUFFER_SIZE 4096
#define WRITE_BUFFER_BACKPRESSURE_THRESHOLD (256 * 1024)  /* 256KB */

static int _on_message_begin(http_parser* parser);
static int _on_url(http_parser* parser, const char* at, size_t length);
static int _on_header_field(http_parser* parser, const char* at, size_t length);
static int _on_header_value(http_parser* parser, const char* at, size_t length);
static int _on_headers_complete(http_parser* parser);
static int _on_body(http_parser* parser, const char* at, size_t length);
static int _on_message_complete(http_parser* parser);

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data);
static void _connection_do_reads(http_connection_t* connection);

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
        http_response_destroy(response);
      } else {
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

  if (connection->request->content_length > OFFS_MAX_BUFFERED_BODY_SIZE) {
    return 1;
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
    http_response_destroy(response);
  } else {
    DESTROY(response, http_response);
  }

  DESTROY(connection->request, http_request);
  connection->request = NULL;

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->headers_complete = 0;
  connection->request_complete = 0;
  return 0;
}

/* Helper: send a watcher update to the server actor (runs on scheduler thread).
 * pd_watcher_update is thread-safe (epoll_ctl MOD). No-op when server is NULL
 * (shutdown) since the I/O thread is already stopped. */
static void _connection_update_watcher(http_connection_t* connection, pd_event_t events) {
  if (connection->server == NULL) return;
  pd_watcher_t* watcher = ATOMIC_LOAD(&connection->watcher);
  if (watcher == NULL) return;
  watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
  payload->watcher = watcher;
  payload->events = events;
  message_t msg;
  msg.type = HTTP_SERVER_UPDATE_WATCHER;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&connection->server->actor, &msg);
}

/* Helper: stop the connection's watcher via the server actor (runs on scheduler thread).
 * Uses atomic exchange to claim the watcher — only one caller will succeed,
 * preventing double-free when both the I/O callback and a dispatch handler
 * try to stop the same watcher. The server actor calls pd_watcher_stop (thread-safe)
 * and defers pd_watcher_destroy to the I/O thread. When server is NULL (shutdown),
 * the I/O thread is already stopped so we stop+destroy directly. */
static void _connection_stop_watcher(http_connection_t* connection) {
  pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
  if (watcher == NULL) return;
  if (connection->server != NULL) {
    watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
    payload->watcher = watcher;
    payload->events = 0;
    message_t msg;
    msg.type = HTTP_SERVER_STOP_WATCHER;
    msg.payload = payload;
    msg.payload_destroy = free;
    actor_send(&connection->server->actor, &msg);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

/* Close the fd and mark connection as closing. Used from dispatch (worker thread). */
static void _connection_close_fd(http_connection_t* connection) {
  if (connection->sock != NULL) {
    platform_socket_destroy(connection->sock);
    connection->sock = NULL;
  }
  connection->is_closing = 1;
}

#ifdef _WIN32
/* poll-dancer's IOCP backend never delivers PD_EVENT_WRITE (only READ is driven
 * through overlapped I/O), so the buffer-then-arm-WRITE strategy used on POSIX
 * can't flush a partial send on Windows — a large response body stalls once the
 * kernel send buffer fills (observed: OFF content GET hangs after ~384KB, exactly
 * when the first platform_socket_send returns EAGAIN). On loopback the peer drains
 * continuously, so a bounded synchronous retry completes the send without blocking
 * the connection actor for long. Handles both plain and TLS connections: SSL_write
 * is retried with the same buffer on SSL_ERROR_WANT_WRITE/WANT_READ (the OpenSSL
 * nonblocking contract). Returns 0 = fully sent, 1 = partial (cap exhausted, caller
 * buffers the remainder), -1 = peer closed, -2 = hard error. *out_sent receives the
 * bytes accepted. Mirrors tcp_connection.c:_connection_send_all_blocking. */
static int _connection_send_all_blocking(http_connection_t* connection,
                                         buffer_t* combined, size_t* out_sent) {
  size_t sent_total = 0;
  for (int attempts = 0; attempts < 2000 && sent_total < combined->size; attempts++) {
    ssize_t sent;
    if (connection->is_ssl && connection->ssl != NULL) {
      sent = SSL_write(connection->ssl, combined->data + sent_total,
                       (int)(combined->size - sent_total));
    } else {
      sent = platform_socket_send(connection->sock, combined->data + sent_total,
                                   combined->size - sent_total);
    }
    if (sent > 0) {
      sent_total += (size_t)sent;
    } else if (sent == 0) {
      *out_sent = sent_total;
      return -1;  /* peer closed */
    } else {
      int retry;
      if (connection->is_ssl && connection->ssl != NULL) {
        int ssl_err = SSL_get_error(connection->ssl, (int)sent);
        retry = (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ);
      } else {
        retry = (errno == EAGAIN || errno == EWOULDBLOCK);
      }
      if (!retry) {
        *out_sent = sent_total;
        return -2;  /* hard error */
      }
      platform_sleep_ms(1);  /* EAGAIN/WANT_WRITE: back off and retry */
    }
  }
  *out_sent = sent_total;
  return (sent_total == combined->size) ? 0 : 1;
}
#endif

/* Connection actor dispatch — runs on scheduler worker threads. */
void http_connection_dispatch(void* state, message_t* msg) {
  http_connection_t* connection = (http_connection_t*)state;
  if (connection->is_closing) {
    /* Connection is shutting down — discard all messages. */
    return;
  }

  switch (msg->type) {
    case HTTP_CONNECTION_READABLE: {
      /* ASIO-style: the I/O thread notified us that data is available.
         Perform the actual recv() and parsing here on the scheduler worker. */
      atomic_store(&connection->read_pending, 0);
      if (connection->sock == NULL) {
        break;
      }
      _connection_do_reads(connection);
      break;
    }

    case HTTP_CONNECTION_DATA: {
      /* Legacy path — kept for safety in case a test or other code sends
         pre-read data directly. Normal I/O now goes through READABLE. */
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL; /* Take ownership — actor_run won't destroy it */
      if (connection->sock == NULL) {
        DESTROY(data, buffer);
        break;
      }
      size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                            (const char*)data->data, data->size);
      (void)nparsed;
      DESTROY(data, buffer);
      if (connection->parser.http_errno != HPE_OK) {
        /* Parse error — close the connection */
        if (connection->piped_pending) {
          if (connection->streaming_route != NULL && connection->request != NULL) {
            stream_deactivate((stream_t*)connection->request,
                              OFFS_ERROR("Connection closed during streaming upload"));
            connection->streaming_route = NULL;
          }
        }
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case HTTP_CONNECTION_HANGUP:
    case HTTP_CONNECTION_ERROR: {
      if (connection->piped_pending) {
        if (connection->streaming_route != NULL && connection->request != NULL) {
          stream_deactivate((stream_t*)connection->request,
                            OFFS_ERROR("Connection closed during streaming upload"));
          connection->streaming_route = NULL;
        }
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case HTTP_CONNECTION_WRITE: {
      buffer_t* buf = (buffer_t*)msg->payload;
      msg->payload = NULL; /* Take ownership — actor_run won't destroy it */
      if (connection->sock == NULL) {
        DESTROY(buf, buffer);
        break;
      }
#ifdef _WIN32
      /* IOCP never delivers PD_EVENT_WRITE, so flush the full buffer with a
       * bounded blocking retry instead of buffering + arming WRITE. See
       * _connection_send_all_blocking above. The POSIX event-driven path is
       * preserved in the #else branch. */
      {
        buffer_t* combined = buf;
        if (connection->write_buffer != NULL && connection->write_buffer->size > 0) {
          size_t total = connection->write_buffer->size + buf->size;
          combined = buffer_create(total);
          memcpy(combined->data, connection->write_buffer->data,
                 connection->write_buffer->size);
          memcpy(combined->data + connection->write_buffer->size,
                 buf->data, buf->size);
          combined->size = total;
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          DESTROY(buf, buffer);
        }
        size_t sent_total = 0;
        int rc = _connection_send_all_blocking(connection, combined, &sent_total);
        if (rc == 0) {
          DESTROY(combined, buffer);
        } else if (rc < 0) {
          DESTROY(combined, buffer);
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        } else {
          size_t remaining = combined->size - sent_total;
          memmove(combined->data, combined->data + sent_total, remaining);
          combined->size = remaining;
          connection->write_buffer = combined;
          connection->write_pending = 1;
          _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        }
        break;
      }
#else
      /* If there's already buffered data, append to it */
      if (connection->write_buffer != NULL && connection->write_buffer->size > 0) {
        buffer_ensure_capacity(connection->write_buffer, connection->write_buffer->size + buf->size);
        memcpy(connection->write_buffer->data + connection->write_buffer->size,
               buf->data, buf->size);
        connection->write_buffer->size += buf->size;
        DESTROY(buf, buffer);
        break;
      }
      /* Try direct send */
      ssize_t sent = platform_socket_send(connection->sock, buf->data, buf->size);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          connection->write_buffer = buf;
          connection->write_pending = 1;
          _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        } else {
          DESTROY(buf, buffer);
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        }
      } else if (sent == 0) {
        /* Peer closed read side — stop writing */
        DESTROY(buf, buffer);
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if ((size_t)sent < buf->size) {
        size_t remaining = buf->size - (size_t)sent;
        connection->write_buffer = buffer_create(remaining);
        memcpy(connection->write_buffer->data, buf->data + sent, remaining);
        connection->write_buffer->size = remaining;
        connection->write_pending = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        DESTROY(buf, buffer);
      } else {
        DESTROY(buf, buffer);
      }
      break;
#endif
    }

    case HTTP_CONNECTION_WRITABLE: {
      if (connection->sock == NULL) {
        break;
      }
      if (connection->write_buffer == NULL || connection->write_buffer->size == 0) {
        connection->write_pending = 0;
        _connection_update_watcher(connection, PD_EVENT_READ);
        break;
      }
      ssize_t sent = platform_socket_send(connection->sock, connection->write_buffer->data,
                           connection->write_buffer->size);
      if (sent > 0) {
        if ((size_t)sent >= connection->write_buffer->size) {
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          connection->write_pending = 0;
          if (connection->is_closing) {
            /* All data flushed — finish the deferred close */
            if (connection->sock != NULL) {
              platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
            }
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
            break;
          }
          _connection_update_watcher(connection, PD_EVENT_READ);
        } else {
          size_t remaining = connection->write_buffer->size - (size_t)sent;
          memmove(connection->write_buffer->data,
                  connection->write_buffer->data + sent, remaining);
          connection->write_buffer->size = remaining;
        }
      } else if (sent == 0) {
        /* Peer closed read side */
        DESTROY(connection->write_buffer, buffer);
        connection->write_buffer = NULL;
        connection->write_pending = 0;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        DESTROY(connection->write_buffer, buffer);
        connection->write_buffer = NULL;
        connection->write_pending = 0;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case HTTP_CONNECTION_CLOSE: {
      /* If there's still buffered write data, defer the close until it's
       * flushed. Otherwise the shutdown truncates the remaining bytes,
       * causing ERR_CONTENT_LENGTH_MISMATCH on the client. */
      if (connection->write_pending) {
        connection->is_closing = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      if (connection->sock != NULL) {
        platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    default:
      break;
  }
}

/* I/O event callback — runs on the I/O thread. For plain HTTP it drains the
   bytes that completed the read directly here and ships them to the
   connection actor as a DATA message for parsing. For SSL it can't — the
   ciphertext has to be decrypted via SSL_read on the worker — so it sends a
   READABLE notification and the actor recvs/decrypts. The split exists because
   of Windows IOCP: a completion-based backend places the read bytes in the
   watcher's overlapped buffer, which is only valid until this callback
   returns (the loop re-arms a fresh WSARecv into the same buffer afterward),
   so the worker can't recv() them later — it would see an empty socket
   (EAGAIN) and never parse anything. Draining here and sending DATA mirrors
   unix_connection and works on every backend (on POSIX
   pd_watcher_drain_read returns 0 and we fall back to a synchronous recv). */
static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  (void)loop;
  http_connection_t* connection = (http_connection_t*)user_data;

  if (events & PD_EVENT_WRITE) {
    /* Socket is writable — notify actor to flush buffered write data */
    message_t writable_msg;
    writable_msg.type = HTTP_CONNECTION_WRITABLE;
    writable_msg.payload = NULL;
    writable_msg.payload_destroy = NULL;
    actor_send(&connection->actor, &writable_msg);
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? HTTP_CONNECTION_HANGUP : HTTP_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    /* Stop the watcher via the server actor's deferred destroy path. A
       completion-based backend (Windows IOCP) may still have an in-flight
       overlapped on this watcher; freeing it here would race that
       completion (use-after-free). _connection_stop_watcher claims the
       watcher atomically and defers pd_watcher_destroy to the I/O-thread
       destroy stack, so the pending completion is drained before the free. */
    _connection_stop_watcher(connection);
    return;
  }

  if (events & PD_EVENT_READ) {
    if (connection->is_ssl && connection->ssl != NULL) {
      /* SSL: decryption must happen on the worker via SSL_read, so just
         notify. One READABLE in flight at a time backpressures via the TCP
         window. (SSL over Windows IOCP is a pre-existing gap: SSL_read on
         the worker hits the same empty-socket EAGAIN as a plain recv would.
         Wiring it up needs a memory BIO fed from the watcher drain — tracked
         as a follow-up, not handled here.) */
      if (atomic_load(&connection->read_pending) == 0) {
        atomic_store(&connection->read_pending, 1);
        message_t msg;
        msg.type = HTTP_CONNECTION_READABLE;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
      }
      return;
    }

    /* Plain HTTP: drain the watcher's completed read buffer here, then send
       the bytes to the actor as DATA. Loop in case more than one buffer's
       worth completed. */
    uint8_t buffer[READ_BUFFER_SIZE];
    size_t total_read = 0;
    size_t n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    while (n > 0) {
      total_read += n;
      if (n < sizeof(buffer)) break; /* buffer fully drained */
      n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    }
    if (total_read == 0) {
      /* POSIX path: synchronous recv. */
      ssize_t bytes_read = platform_socket_recv(connection->sock, buffer, sizeof(buffer));
      if (bytes_read <= 0) {
        if (bytes_read == 0) {
          message_t msg;
          msg.type = HTTP_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        message_t msg;
        msg.type = HTTP_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
      total_read = (size_t)bytes_read;
    }

    buffer_t* data = buffer_create_from_pointer_copy(buffer, total_read);
    message_t msg;
    msg.type = HTTP_CONNECTION_DATA;
    msg.payload = data;
    msg.payload_destroy = (void (*)(void*))buffer_destroy;
    actor_send(&connection->actor, &msg);
  }
}

/* Perform a batch of SSL reads and HTTP parsing. Called from the connection
   actor dispatch (HTTP_CONNECTION_READABLE) on scheduler worker threads. The
   I/O-thread read callback sends READABLE only for SSL connections — plain HTTP
   is drained in the callback and shipped as a DATA message — so this path is
   SSL-only: SSL_read must run on the worker to decrypt the ciphertext. (SSL
   over Windows IOCP is a pre-existing gap tracked separately: on IOCP the read
   bytes land in the watcher's overlapped buffer, so SSL_read on the worker sees
   an empty socket and returns WANT_READ; wiring it needs a memory BIO fed from
   the watcher drain. On epoll/kqueue this path works as-is.) */
static void _connection_do_reads(http_connection_t* connection) {
  if (!connection->is_ssl || connection->ssl == NULL) {
    return;
  }
  for (int batch = 0; batch < 16; batch++) {
    char buffer[READ_BUFFER_SIZE];
    if (connection->sock == NULL) {
      return;
    }

    ssize_t bytes_read = SSL_read(connection->ssl, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
      int ssl_error = SSL_get_error(connection->ssl, (int)bytes_read);
      if (ssl_error == SSL_ERROR_WANT_READ) {
        return;
      }
      message_t msg;
      msg.type = HTTP_CONNECTION_HANGUP;
      msg.payload = NULL;
      msg.payload_destroy = NULL;
      actor_send(&connection->actor, &msg);
      /* Stop via the deferred path so a completion-based backend doesn't free
         the watcher's overlapped while a completion is still pending. */
      _connection_stop_watcher(connection);
      return;
    }

    buffer_t* data = buffer_create_from_pointer_copy((uint8_t*)buffer, (size_t)bytes_read);
    size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                          (const char*)data->data, data->size);
    (void)nparsed;
    DESTROY(data, buffer);
    if (connection->parser.http_errno != HPE_OK) {
      /* Parse error — close the connection */
      if (connection->piped_pending) {
        if (connection->streaming_route != NULL && connection->request != NULL) {
          stream_deactivate((stream_t*)connection->request,
                            OFFS_ERROR("Connection closed during streaming upload"));
          connection->streaming_route = NULL;
        }
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      return;
    }
  }
}

http_connection_t* http_connection_create(http_server_t* server, platform_socket_t* sock) {
  http_connection_t* connection = get_clear_memory(sizeof(http_connection_t));
  refcounter_init((refcounter_t*)connection);
  connection->server = server;
  connection->sock = sock;
  connection->ssl = NULL;
  connection->is_ssl = 0;
  connection->headers_complete = 0;
  connection->request_complete = 0;
  connection->request = NULL;
  connection->write_buffer = NULL;
  connection->write_pending = 0;
  atomic_store(&connection->read_pending, 0);
  connection->header_field = NULL;
  connection->header_field_len = 0;
  connection->header_field_cap = 0;
  connection->header_value = NULL;
  connection->header_value_len = 0;
  connection->header_value_cap = 0;
  connection->streaming_route = NULL;

  actor_init(&connection->actor, connection, http_connection_dispatch, server->pool);

  platform_socket_set_nonblocking(sock);

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->parser.data = connection;

  ATOMIC_STORE(&connection->watcher, pd_watcher_create(server->loop, platform_socket_fd(sock),
    PD_EVENT_READ, _connection_read_callback, connection));
  if (ATOMIC_LOAD(&connection->watcher) != NULL) {
    pd_watcher_start(ATOMIC_LOAD(&connection->watcher));
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
    actor_destroy(&connection->actor);
    if (ATOMIC_LOAD(&connection->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
      if (watcher != NULL) {
        /* During server shutdown (server == NULL), the I/O thread is already
         * stopped so we can stop and destroy the watcher directly. During
         * normal operation, defer stop+destroy to the I/O thread via the
         * server actor's destroy stack. */
        if (connection->server != NULL) {
          watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
          payload->watcher = watcher;
          payload->events = 0;
          message_t msg;
          msg.type = HTTP_SERVER_STOP_WATCHER;
          msg.payload = payload;
          msg.payload_destroy = free;
          actor_send(&connection->server->actor, &msg);
        } else {
          pd_watcher_stop(watcher);
          pd_watcher_destroy(watcher);
        }
      }
    }
    if (connection->sock != NULL) {
      platform_socket_destroy(connection->sock);
      connection->sock = NULL;
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
  if (connection == NULL || connection->sock == NULL) {
    return;
  }
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, length);
  message_t msg;
  msg.type = HTTP_CONNECTION_WRITE;
  msg.payload = buf;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;
  actor_send(&connection->actor, &msg);
}

void http_connection_close(http_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  message_t msg;
  msg.type = HTTP_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}