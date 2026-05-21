//
// Created by victor on 5/20/26.
//
#include "offs_client.h"
#include "../../ClientAPI/client_api_wire.h"
#include "../../Network/stream_framer.h"
#include "../../Buffer/buffer.h"
#include "../../Util/allocator.h"
#include "../../ClientAPI/WS/ws_frame.h"

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
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define READ_BUFFER_SIZE 65536

typedef enum {
  OFFS_TRANSPORT_UNIX,
  OFFS_TRANSPORT_TCP,
  OFFS_TRANSPORT_WS,
  OFFS_TRANSPORT_WT,
} offs_transport_type_e;

struct offs_client_t {
  offs_transport_type_e transport_type;
  uint8_t connected;
  union {
    struct {
      int fd;
      uint8_t is_unix;
    } raw;
    struct {
      int fd;
      SSL* ssl;
      uint8_t is_ssl;
      buffer_t* recv_buf;
    } ws;
    struct {
      void* registration;
      void* configuration;
      void* connection;
      void* stream;
      uint8_t stream_open;
      pthread_mutex_t recv_lock;
      pthread_cond_t recv_cond;
      uint8_t* recv_buf;
      size_t recv_buf_size;
      size_t recv_buf_used;
      uint8_t* send_buf;
      size_t send_buf_len;
      uint8_t send_complete;
    } wt;
  } transport;
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

static int _raw_send(offs_client_t* client, const uint8_t* data, size_t len) {
  ssize_t sent = send(client->transport.raw.fd, data, len, MSG_NOSIGNAL);
  if (sent < 0) return -1;
  size_t total_sent = (size_t)sent;
  while (total_sent < len) {
    sent = send(client->transport.raw.fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
    if (sent <= 0) return -1;
    total_sent += (size_t)sent;
  }
  return 0;
}

static int _ws_send(offs_client_t* client, const uint8_t* data, size_t len) {
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    size_t total_written = 0;
    while (total_written < len) {
      int written = SSL_write(client->transport.ws.ssl, data + total_written, (int)(len - total_written));
      if (written <= 0) return -1;
      total_written += (size_t)written;
    }
    return 0;
  }
  return _raw_send(client, data, len);
}

static void _send_frame(offs_client_t* client, cbor_item_t* frame) {
  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  cbor_decref(&frame);

  if (cbor_buf == NULL || cbor_len == 0) return;

  uint8_t* framed;
  size_t framed_len;

  if (client->transport_type == OFFS_TRANSPORT_WS) {
    framed = ws_frame_build_masked(WS_OPCODE_BINARY, cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    pthread_mutex_lock(&client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_ensure_capacity(client->write_buffer, client->write_buffer->size + framed_len);
      memcpy(client->write_buffer->data + client->write_buffer->size, framed, framed_len);
      client->write_buffer->size += framed_len;
    } else {
      if (_ws_send(client, framed, framed_len) < 0) {
        client->connected = 0;
      }
    }
    pthread_mutex_unlock(&client->lock);
    free(framed);
  } else {
    /* unix/tcp/wt: length-prefix framing */
    framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
    free(cbor_buf);
    if (framed == NULL) return;

    pthread_mutex_lock(&client->lock);
    if (client->write_buffer != NULL && client->write_buffer->size > 0) {
      buffer_ensure_capacity(client->write_buffer, client->write_buffer->size + framed_len);
      memcpy(client->write_buffer->data + client->write_buffer->size, framed, framed_len);
      client->write_buffer->size += framed_len;
    } else {
      int fd = client->transport.raw.fd;
      ssize_t sent = send(fd, framed, framed_len, MSG_NOSIGNAL);
      if (sent < 0) {
        client->connected = 0;
      } else if ((size_t)sent < framed_len) {
        size_t remaining = framed_len - (size_t)sent;
        client->write_buffer = buffer_create(remaining);
        memcpy(client->write_buffer->data, framed + sent, remaining);
        client->write_buffer->size = remaining;
      }
    }
    pthread_mutex_unlock(&client->lock);
    free(framed);
  }
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

static ssize_t _ws_recv(offs_client_t* client, uint8_t* buf, size_t buf_size) {
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    return SSL_read(client->transport.ws.ssl, buf, (int)buf_size);
  }
  return recv(client->transport.ws.fd, buf, buf_size, 0);
}

static void* _recv_thread(void* arg) {
  offs_client_t* client = (offs_client_t*)arg;
  uint8_t buf[READ_BUFFER_SIZE];

  while (client->running) {
    if (client->transport_type == OFFS_TRANSPORT_WS) {
      /* WS: read raw bytes, parse WebSocket frames */
      ssize_t bytes_read = _ws_recv(client, buf, sizeof(buf));
      if (bytes_read <= 0) {
        client->connected = 0;
        break;
      }

      /* Append to recv buffer */
      if (client->transport.ws.recv_buf == NULL) {
        client->transport.ws.recv_buf = buffer_create(bytes_read);
        memcpy(client->transport.ws.recv_buf->data, buf, bytes_read);
        client->transport.ws.recv_buf->size = bytes_read;
      } else {
        buffer_ensure_capacity(client->transport.ws.recv_buf, client->transport.ws.recv_buf->size + bytes_read);
        memcpy(client->transport.ws.recv_buf->data + client->transport.ws.recv_buf->size, buf, bytes_read);
        client->transport.ws.recv_buf->size += bytes_read;
      }

      /* Parse all complete WebSocket frames */
      while (1) {
        ws_frame_t parsed;
        size_t needed;
        ssize_t consumed = ws_frame_parse(
          client->transport.ws.recv_buf->data,
          client->transport.ws.recv_buf->size,
          &parsed, &needed);
        if (consumed == 0) break; /* incomplete frame */
        if (consumed < 0) {
          client->connected = 0;
          goto cleanup;
        }
        /* Consume parsed bytes from recv buffer */
        size_t remaining = client->transport.ws.recv_buf->size - (size_t)consumed;
        if (remaining > 0) {
          memmove(client->transport.ws.recv_buf->data,
                  client->transport.ws.recv_buf->data + consumed,
                  remaining);
        }
        client->transport.ws.recv_buf->size = remaining;

        if (parsed.opcode == WS_OPCODE_BINARY && parsed.payload != NULL && parsed.payload_len > 0) {
          struct cbor_load_result load_result;
          cbor_item_t* item = cbor_load(parsed.payload, parsed.payload_len, &load_result);
          if (item != NULL && load_result.error.code == CBOR_ERR_NONE) {
            uint8_t type = client_api_wire_get_type(item);
            _handle_frame(client, type, item);
            cbor_decref(&item);
          } else if (item != NULL) {
            cbor_decref(&item);
          }
        } else if (parsed.opcode == WS_OPCODE_CLOSE) {
          client->connected = 0;
          ws_frame_destroy(&parsed);
          goto cleanup;
        }
        ws_frame_destroy(&parsed);
      }
    } else {
      /* unix/tcp: raw socket + stream_framer */
      ssize_t bytes_read = recv(client->transport.raw.fd, buf, sizeof(buf), 0);
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
        free(frame_data);
        if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
          if (cbor_item != NULL) cbor_decref(&cbor_item);
          continue;
        }
        uint8_t type = client_api_wire_get_type(cbor_item);
        _handle_frame(client, type, cbor_item);
        cbor_decref(&cbor_item);
      }
    }
  }

cleanup:
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

static char* _ws_compute_accept_key(const char* client_key) {
  const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  size_t combined_len = strlen(client_key) + strlen(magic);
  uint8_t* combined = get_memory(combined_len + 1);
  memcpy(combined, client_key, strlen(client_key));
  memcpy(combined + strlen(client_key), magic, strlen(magic));
  combined[combined_len] = '\0';

  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(combined, combined_len, hash);
  free(combined);

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, hash, SHA_DIGEST_LENGTH);
  BIO_flush(bio);

  char* result;
  long len = BIO_get_mem_data(bio, &result);
  char* accept_key = get_memory(len + 1);
  memcpy(accept_key, result, len);
  accept_key[len] = '\0';

  BIO_free_all(bio);
  return accept_key;
}

static int _ws_upgrade(offs_client_t* client) {
  /* Generate a 16-byte random key and base64 encode */
  uint8_t raw_key[16];
  FILE* urandom = fopen("/dev/urandom", "rb");
  if (urandom == NULL) return -1;
  if (fread(raw_key, 1, 16, urandom) != 16) {
    fclose(urandom);
    return -1;
  }
  fclose(urandom);

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* bio = BIO_new(BIO_s_mem());
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(bio, raw_key, 16);
  BIO_flush(bio);

  char* key_b64;
  long key_len = BIO_get_mem_data(bio, &key_b64);
  char* client_key = get_memory(key_len + 1);
  memcpy(client_key, key_b64, key_len);
  client_key[key_len] = '\0';
  BIO_free_all(bio);

  /* Build and send upgrade request */
  char request[1024];
  snprintf(request, sizeof(request),
    "GET /offs HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n",
    client_key);

  if (_ws_send(client, (const uint8_t*)request, strlen(request)) < 0) {
    free(client_key);
    return -1;
  }

  /* Read response */
  char response[4096];
  ssize_t received;
  if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
    received = SSL_read(client->transport.ws.ssl, response, sizeof(response) - 1);
  } else {
    received = recv(client->transport.ws.fd, response, sizeof(response) - 1, 0);
  }

  free(client_key);

  if (received <= 0) return -1;
  response[received] = '\0';

  /* Check for 101 Switching Protocols */
  if (strstr(response, "101") == NULL) return -1;

  return 0;
}

offs_client_t* offs_client_connect(const char* transport_url) {
  if (transport_url == NULL) return NULL;

  offs_client_t* client = get_clear_memory(sizeof(offs_client_t));
  client->transport_type = OFFS_TRANSPORT_UNIX;
  client->connected = 0;
  client->running = 0;
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
    client->transport.raw.fd = _connect_unix(path);
    client->transport.raw.is_unix = 1;
    client->transport_type = OFFS_TRANSPORT_UNIX;
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
    client->transport.raw.fd = _connect_tcp(host, port);
    client->transport.raw.is_unix = 0;
    client->transport_type = OFFS_TRANSPORT_TCP;
    free(host);
  } else if (strncmp(transport_url, "ws://", 5) == 0 || strncmp(transport_url, "wss://", 6) == 0) {
    uint8_t is_ssl = (transport_url[4] == 's');
    const char* addr_start = is_ssl ? transport_url + 6 : transport_url + 5;
    char* addr_copy = get_memory(strlen(addr_start) + 1);
    strcpy(addr_copy, addr_start);
    /* Extract path (everything after first /) */
    char* path_start = strchr(addr_copy, '/');
    if (path_start != NULL) {
      *path_start = '\0';
    }
    /* Extract host and port */
    char* colon = strrchr(addr_copy, ':');
    uint16_t port;
    char* ws_host;
    if (colon != NULL) {
      *colon = '\0';
      port = (uint16_t)atoi(colon + 1);
      ws_host = addr_copy;
    } else {
      port = is_ssl ? 443 : 80;
      ws_host = addr_copy;
    }

    client->transport.ws.fd = _connect_tcp(ws_host, port);
    client->transport.ws.ssl = NULL;
    client->transport.ws.is_ssl = is_ssl;
    client->transport.ws.recv_buf = NULL;

    if (client->transport.ws.fd < 0) {
      free(addr_copy);
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }

    if (is_ssl) {
      SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_client_method());
      if (ssl_ctx == NULL) {
        free(addr_copy);
        close(client->transport.ws.fd);
        stream_framer_destroy(client->framer);
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NULL;
      }
      SSL* ssl = SSL_new(ssl_ctx);
      SSL_set_fd(ssl, client->transport.ws.fd);
      SSL_set_tlsext_host_name(ssl, ws_host);
      if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        free(addr_copy);
        close(client->transport.ws.fd);
        stream_framer_destroy(client->framer);
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NULL;
      }
      client->transport.ws.ssl = ssl;
      /* SSL_CTX freed automatically when all SSL connections are freed */
    }

    /* addr_copy still alive here — ws_host points into it */
    if (_ws_upgrade(client) != 0) {
      if (client->transport.ws.ssl != NULL) {
        SSL_free(client->transport.ws.ssl);
      }
      free(addr_copy);
      close(client->transport.ws.fd);
      stream_framer_destroy(client->framer);
      pthread_mutex_destroy(&client->lock);
      free(client);
      return NULL;
    }

    free(addr_copy);

    client->transport_type = OFFS_TRANSPORT_WS;
    client->connected = 1;
    client->running = 1;
    /* WS doesn't use stream_framer — it uses ws_frame_parse instead */
    stream_framer_destroy(client->framer);
    client->framer = NULL;
    pthread_create(&client->recv_thread, NULL, _recv_thread, client);
    return client;
  } else if (strncmp(transport_url, "wt://", 5) == 0 || strncmp(transport_url, "wts://", 6) == 0) {
    /* WT -- not yet implemented */
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  } else {
    stream_framer_destroy(client->framer);
    pthread_mutex_destroy(&client->lock);
    free(client);
    return NULL;
  }

  if (client->transport.raw.fd < 0) {
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

  switch (client->transport_type) {
    case OFFS_TRANSPORT_UNIX:
    case OFFS_TRANSPORT_TCP:
      if (client->transport.raw.fd >= 0) {
        shutdown(client->transport.raw.fd, SHUT_RDWR);
        close(client->transport.raw.fd);
      }
      break;
    case OFFS_TRANSPORT_WS:
      if (client->transport.ws.is_ssl && client->transport.ws.ssl != NULL) {
        SSL_shutdown(client->transport.ws.ssl);
        SSL_free(client->transport.ws.ssl);
      }
      if (client->transport.ws.fd >= 0) {
        shutdown(client->transport.ws.fd, SHUT_RDWR);
        close(client->transport.ws.fd);
      }
      if (client->transport.ws.recv_buf != NULL) {
        DESTROY(client->transport.ws.recv_buf, buffer);
      }
      break;
    case OFFS_TRANSPORT_WT:
      /* WT cleanup -- implemented later */
      break;
  }

  if (client->recv_thread != 0) {
    pthread_join(client->recv_thread, NULL);
  }

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