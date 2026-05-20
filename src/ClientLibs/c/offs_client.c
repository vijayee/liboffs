//
// Created by victor on 5/20/26.
//
#include "offs_client.h"
#include "../../ClientAPI/client_api_wire.h"
#include "../../Network/stream_framer.h"
#include "../../Buffer/buffer.h"
#include "../../Util/allocator.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#define READ_BUFFER_SIZE 65536

struct offs_client_t {
  int fd;
  uint8_t connected;
  uint8_t is_unix;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  pthread_mutex_t lock;
  pthread_t recv_thread;
  volatile uint8_t running;
  uint8_t* read_buf;
  size_t read_buf_size;

  /* Callbacks */
  offs_put_response_cb_t put_cb;
  void* put_cb_ctx;
  offs_get_data_cb_t get_data_cb;
  void* get_data_cb_ctx;
  offs_get_end_cb_t get_end_cb;
  void* get_end_cb_ctx;
  offs_error_cb_t error_cb;
  void* error_cb_ctx;
};

static void _send_frame(offs_client_t* client, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  size_t framed_len;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);

  if (framed == NULL) return;

  pthread_mutex_lock(&client->lock);
  if (client->write_buffer != NULL) {
    buffer_ensure_capacity(client->write_buffer, client->write_buffer->size + framed_len);
    memcpy(client->write_buffer->data + client->write_buffer->size, framed, framed_len);
    client->write_buffer->size += framed_len;
  } else {
    ssize_t sent = send(client->fd, framed, framed_len, MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent < framed_len) {
      size_t remaining = framed_len - (sent > 0 ? (size_t)sent : 0);
      client->write_buffer = buffer_create(remaining);
      if (sent > 0) {
        memcpy(client->write_buffer->data, framed + sent, remaining);
      } else {
        memcpy(client->write_buffer->data, framed, remaining);
      }
      client->write_buffer->size = remaining;
    }
  }
  pthread_mutex_unlock(&client->lock);

  free(framed);
}

static void _handle_frame(offs_client_t* client, uint8_t type, cbor_item_t* frame) {
  switch (type) {
    case CLIENT_API_PUT_RESPONSE: {
      client_api_put_response_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_put_response_decode(frame, &msg) == 0) {
        if (client->put_cb != NULL) {
          client->put_cb(client->put_cb_ctx, msg.ori_string);
        }
        client_api_put_response_destroy(&msg);
      }
      break;
    }
    case CLIENT_API_GET_RESPONSE_START: {
      /* We don't need to process the start response for the simple client */
      break;
    }
    case CLIENT_API_GET_DATA: {
      client_api_get_data_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_get_data_decode(frame, &msg) == 0) {
        if (client->get_data_cb != NULL) {
          client->get_data_cb(client->get_data_cb_ctx, msg.data, msg.data_size);
        }
        free(msg.data);
      }
      break;
    }
    case CLIENT_API_GET_END: {
      if (client->get_end_cb != NULL) {
        client->get_end_cb(client->get_end_cb_ctx);
      }
      break;
    }
    case CLIENT_API_ERROR: {
      client_api_error_t msg;
      memset(&msg, 0, sizeof(msg));
      if (client_api_error_decode(frame, &msg) == 0) {
        if (client->error_cb != NULL) {
          client->error_cb(client->error_cb_ctx, msg.status_code, msg.message);
        }
        client_api_error_destroy(&msg);
      }
      break;
    }
    default:
      break;
  }
}

static void* _recv_thread(void* arg) {
  offs_client_t* client = (offs_client_t*)arg;
  uint8_t buf[READ_BUFFER_SIZE];

  while (client->running) {
    ssize_t bytes_read = recv(client->fd, buf, sizeof(buf), 0);
    if (bytes_read <= 0) {
      client->connected = 0;
      break;
    }

    stream_framer_feed(client->framer, buf, (size_t)bytes_read);

    uint8_t* frame_data;
    size_t frame_len;
    while ((frame_data = stream_framer_next(client->framer, &frame_len)) != NULL) {
      struct cbor_load_result load_result;
      cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
      if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
        if (cbor_item != NULL) {
          cbor_decref(&cbor_item);
        }
        continue;
      }
      uint8_t type = client_api_wire_get_type(cbor_item);
      _handle_frame(client, type, cbor_item);
      cbor_decref(&cbor_item);
    }
  }

  return NULL;
}

static int _connect_unix(const char* path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int _connect_tcp(const char* host, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    close(fd);
    return -1;
  }

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

offs_client_t* offs_client_connect(const char* transport_url) {
  if (transport_url == NULL) return NULL;

  offs_client_t* client = get_clear_memory(sizeof(offs_client_t));
  client->fd = -1;
  client->connected = 0;
  client->running = 0;
  client->is_unix = 0;
  client->framer = stream_framer_create();
  client->write_buffer = NULL;
  pthread_mutex_init(&client->lock, NULL);
  client->put_cb = NULL;
  client->put_cb_ctx = NULL;
  client->get_data_cb = NULL;
  client->get_data_cb_ctx = NULL;
  client->get_end_cb = NULL;
  client->get_end_cb_ctx = NULL;
  client->error_cb = NULL;
  client->error_cb_ctx = NULL;

  if (strncmp(transport_url, "unix://", 7) == 0) {
    const char* path = transport_url + 7;
    client->fd = _connect_unix(path);
    client->is_unix = 1;
  } else if (strncmp(transport_url, "tcp://", 6) == 0) {
    const char* addr = transport_url + 6;
    char* host = get_memory(strlen(addr) + 1);
    strcpy(host, addr);
    char* colon = strrchr(host, ':');
    if (colon == NULL) {
      free(host);
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }
    *colon = '\0';
    uint16_t port = (uint16_t)atoi(colon + 1);
    client->fd = _connect_tcp(host, port);
    free(host);
  } else {
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  }

  if (client->fd < 0) {
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  }

  client->connected = 1;
  client->running = 1;
  pthread_create(&client->recv_thread, NULL, _recv_thread, client);

  return client;
}

void offs_client_disconnect(offs_client_t* client) {
  if (client == NULL) return;

  client->running = 0;
  client->connected = 0;
  if (client->fd >= 0) {
    shutdown(client->fd, SHUT_RDWR);
    close(client->fd);
    client->fd = -1;
  }
  pthread_join(client->recv_thread, NULL);

  if (client->framer != NULL) {
    stream_framer_destroy(client->framer);
  }
  if (client->write_buffer != NULL) {
    DESTROY(client->write_buffer, buffer);
  }
  pthread_mutex_destroy(&client->lock);
  free(client);
}

int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx) {
  if (client == NULL || !client->connected) return -1;

  client->put_cb = callback;
  client->put_cb_ctx = ctx;

  client_api_put_request_t msg;
  msg.content_type = (char*)content_type;
  msg.file_name = (char*)file_name;
  msg.stream_length = stream_length;
  msg.server_address = NULL;
  msg.data = (uint8_t*)data;
  msg.data_size = data_len;

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length) {
  if (client == NULL || !client->connected) return -1;

  client_api_put_request_t msg;
  msg.content_type = (char*)content_type;
  msg.file_name = (char*)file_name;
  msg.stream_length = stream_length;
  msg.server_address = NULL;
  msg.data = NULL;
  msg.data_size = 0;

  cbor_item_t* frame = client_api_put_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_data(offs_client_t* client,
                                 const uint8_t* data,
                                 size_t len) {
  if (client == NULL || !client->connected) return -1;

  client_api_put_data_t msg;
  msg.data = (uint8_t*)data;
  msg.data_size = len;

  cbor_item_t* frame = client_api_put_data_encode(&msg);
  _send_frame(client, frame);

  return 0;
}

int offs_client_put_stream_end(offs_client_t* client,
                                offs_put_response_cb_t callback,
                                void* ctx) {
  if (client == NULL || !client->connected) return -1;

  client->put_cb = callback;
  client->put_cb_ctx = ctx;

  cbor_item_t* frame = client_api_put_end_encode();
  _send_frame(client, frame);

  return 0;
}

int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx) {
  if (client == NULL || !client->connected) return -1;

  client->get_data_cb = data_cb;
  client->get_data_cb_ctx = ctx;
  client->get_end_cb = end_cb;
  client->get_end_cb_ctx = ctx;
  client->error_cb = error_cb;
  client->error_cb_ctx = ctx;

  client_api_get_request_t msg;
  msg.ori_string = (char*)ori_string;
  msg.has_range = 0;
  msg.range_start = 0;
  msg.range_end = 0;

  cbor_item_t* frame = client_api_get_request_encode(&msg);
  _send_frame(client, frame);

  return 0;
}