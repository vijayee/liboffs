//
// Created by victor on 5/20/26.
//

#include "client_api_wire.h"
#include "../Util/allocator.h"
#include "../Util/base58.h"
#include "../Util/validation.h"
#include <stdlib.h>
#include <string.h>

// --- Helper: encode a string as CBOR text string (empty string for NULL) ---
static cbor_item_t* _encode_string(const char* str) {
  if (str == NULL) {
    return cbor_build_string("");
  }
  return cbor_build_string(str);
}

// --- Helper: decode a CBOR text string, caller must free() the result ---
// Returns NULL for empty strings (treat as absent) or strings exceeding max_len
static char* _decode_string(cbor_item_t* item, size_t max_len) {
  if (!cbor_isa_string(item)) return NULL;
  size_t len = cbor_string_length(item);
  if (len == 0 || len > max_len) return NULL;
  char* str = get_memory(len + 1);
  memcpy(str, cbor_string_handle(item), len);
  str[len] = '\0';
  return str;
}

/* Sentinel source pointer passed to cbor_build_bytestring when the encoder
 * needs a zero-length bytestring. The C standard treats memcpy(dst, NULL, 0)
 * as undefined behavior, so we never pass NULL even when length is zero. */
static const uint8_t _empty_byte_sentinel = 0;

// --- Helper: safe cbor_get_int → size_t with overflow guard ---
static size_t _decode_size(cbor_item_t* item) {
  uint64_t val = cbor_get_int(item);
  if (val > SIZE_MAX) return 0;
  return (size_t)val;
}

// --- Helper: decode a CBOR bytestring into allocated buffer ---
static uint8_t* _decode_bytestring(cbor_item_t* item, size_t* out_len) {
  if (cbor_is_null(item)) {
    *out_len = 0;
    return NULL;
  }
  if (!cbor_isa_bytestring(item)) return NULL;
  size_t len = cbor_bytestring_length(item);
  uint8_t* data = get_memory(len);
  memcpy(data, cbor_bytestring_handle(item), len);
  *out_len = len;
  return data;
}

uint8_t client_api_wire_get_type(cbor_item_t* item) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 1) return 0;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item)) {
    cbor_decref(&type_item);
    return 0;
  }
  uint8_t type = (uint8_t)cbor_get_uint8(type_item);
  cbor_decref(&type_item);
  return type;
}

// --- PUT Request ---
// [type, content_type, file_name, stream_length, server_address, data, recycler_urls, temporary]

cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(8);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PUT_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->content_type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->file_name);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->stream_length);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->server_address);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->data != NULL && msg->data_size > 0) {
    item = cbor_build_bytestring(msg->data, msg->data_size);
  } else {
    item = cbor_build_bytestring(&_empty_byte_sentinel, 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  /* Index 6: recycler_urls CBOR array (always present, empty if none) */
  {
    size_t url_count = (msg->recycler_urls != NULL) ? msg->recycler_count : 0;
    cbor_item_t* urls_array = cbor_new_definite_array(url_count);
    for (size_t i = 0; i < url_count; i++) {
      item = cbor_build_string(msg->recycler_urls[i] ? msg->recycler_urls[i] : "");
      (void)cbor_array_push(urls_array, item);
      cbor_decref(&item);
    }
    (void)cbor_array_push(array, urls_array);
    cbor_decref(&urls_array);
  }

  /* Index 7: temporary (always present, 0 or 1) */
  item = cbor_build_uint8(msg->temporary ? 1 : 0);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_put_request_decode(cbor_item_t* item, client_api_put_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* content_type = cbor_array_get(item, 1);
  msg->content_type = _decode_string(content_type, OFFS_MAX_CONTENT_TYPE_LEN);
  cbor_decref(&content_type);

  cbor_item_t* file_name = cbor_array_get(item, 2);
  msg->file_name = _decode_string(file_name, OFFS_MAX_FILE_NAME_LEN);
  cbor_decref(&file_name);

  cbor_item_t* stream_length = cbor_array_get(item, 3);
  msg->stream_length = _decode_size(stream_length);
  cbor_decref(&stream_length);

  // Validate decoded fields
  if (validate_content_type(msg->content_type) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }
  if (validate_file_name(msg->file_name) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }
  if (msg->stream_length == 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }
  /* stream_length is the total size of a streaming PUT (the sum of all
   * subsequent PUT_DATA frames), not the size of a single CBOR message.
   * Capping it at OFFS_MAX_CBOR_MESSAGE_SIZE (64MB) made any upload larger
   * than 64MB fail at PUT_START with "Invalid PUT request" — the server
   * rejected the stream before a single PUT_DATA frame was processed.
   * The server does not allocate stream_length bytes upfront
   * (writeable_descriptor_create stores it as metadata only), so the
   * only resource bound that matters is the per-frame data_size check
   * below. Leave stream_length unbounded apart from the != 0 check. */

  if (cbor_array_size(item) >= 5) {
    cbor_item_t* server_address = cbor_array_get(item, 4);
    msg->server_address = _decode_string(server_address, OFFS_MAX_ORI_STRING_LEN);
    cbor_decref(&server_address);
  }

  if (cbor_array_size(item) >= 6) {
    cbor_item_t* data_item = cbor_array_get(item, 5);
    if (!cbor_is_null(data_item) && cbor_isa_bytestring(data_item)) {
      msg->data_size = cbor_bytestring_length(data_item);
      if (msg->data_size > OFFS_MAX_CBOR_MESSAGE_SIZE) {
        cbor_decref(&data_item);
        client_api_put_request_destroy(msg);
        return -1;
      }
      if (msg->data_size > 0) {
        msg->data = get_memory(msg->data_size);
        memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
      }
    }
    cbor_decref(&data_item);
  }

  /* Index 6: recycler_urls (optional array of strings) */
  if (cbor_array_size(item) >= 7) {
    cbor_item_t* urls_item = cbor_array_get(item, 6);
    if (cbor_isa_array(urls_item)) {
      size_t url_count = cbor_array_size(urls_item);
      if (url_count > 256) {
        cbor_decref(&urls_item);
        client_api_put_request_destroy(msg);
        return -1;
      }
      if (url_count > 0) {
        msg->recycler_urls = get_clear_memory(sizeof(char*) * url_count);
        msg->recycler_count = url_count;
        for (size_t i = 0; i < url_count; i++) {
          cbor_item_t* url_str = cbor_array_get(urls_item, i);
          msg->recycler_urls[i] = _decode_string(url_str, OFFS_MAX_ORI_STRING_LEN);
          cbor_decref(&url_str);
          if (msg->recycler_urls[i] == NULL
              || validate_ori_string(msg->recycler_urls[i]) != 0) {
            client_api_put_request_destroy(msg);
            cbor_decref(&urls_item);
            return -1;
          }
        }
      }
    }
    cbor_decref(&urls_item);
  }

  /* Index 7: temporary (optional uint8) */
  if (cbor_array_size(item) >= 8) {
    cbor_item_t* temp_item = cbor_array_get(item, 7);
    if (cbor_isa_uint(temp_item)) {
      msg->temporary = cbor_get_uint8(temp_item);
    }
    cbor_decref(&temp_item);
  }

  return 0;
}

void client_api_put_request_destroy(client_api_put_request_t* msg) {
  if (msg == NULL) return;
  free(msg->content_type);
  free(msg->file_name);
  free(msg->server_address);
  free(msg->data);
  if (msg->recycler_urls != NULL) {
    for (size_t i = 0; i < msg->recycler_count; i++) {
      free(msg->recycler_urls[i]);
    }
    free(msg->recycler_urls);
  }
}

// --- PUT Data ---
// [type, bytestring]

cbor_item_t* client_api_put_data_encode(const client_api_put_data_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PUT_DATA);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_put_data_decode(cbor_item_t* item, client_api_put_data_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* data_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);
  return 0;
}

void client_api_get_data_destroy(client_api_get_data_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- PUT End ---
// [type]

cbor_item_t* client_api_put_end_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_PUT_END);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_put_end_decode(cbor_item_t* item) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 1) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  uint8_t type = (uint8_t)cbor_get_uint8(type_item);
  cbor_decref(&type_item);
  return type == CLIENT_API_PUT_END ? 0 : -1;
}

// --- PUT Response ---
// [type, ori_string]

cbor_item_t* client_api_put_response_encode(const client_api_put_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PUT_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->ori_string);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_put_response_decode(cbor_item_t* item, client_api_put_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* ori = cbor_array_get(item, 1);
  msg->ori_string = _decode_string(ori, OFFS_MAX_ORI_STRING_LEN);
  cbor_decref(&ori);
  return 0;
}

void client_api_put_response_destroy(client_api_put_response_t* msg) {
  if (msg == NULL) return;
  free(msg->ori_string);
}

// --- GET Request ---
// [type, ori_string, has_range?, range_start?, range_end?]

cbor_item_t* client_api_get_request_encode(const client_api_get_request_t* msg) {
  cbor_item_t* array;
  cbor_item_t* item;

  if (msg->has_range) {
    array = cbor_new_definite_array(5);
  } else {
    array = cbor_new_definite_array(2);
  }

  item = cbor_build_uint8(CLIENT_API_GET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->ori_string);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->has_range) {
    item = cbor_build_uint8(1);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint64(msg->range_start);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint64(msg->range_end);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

int client_api_get_request_decode(cbor_item_t* item, client_api_get_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* ori = cbor_array_get(item, 1);
  msg->ori_string = _decode_string(ori, OFFS_MAX_ORI_STRING_LEN);
  cbor_decref(&ori);

  if (validate_ori_string(msg->ori_string) != 0) {
    free(msg->ori_string);
    msg->ori_string = NULL;
    return -1;
  }

  if (cbor_array_size(item) >= 5) {
    cbor_item_t* has_range = cbor_array_get(item, 2);
    msg->has_range = (uint8_t)cbor_get_uint8(has_range);
    cbor_decref(&has_range);

    cbor_item_t* range_start = cbor_array_get(item, 3);
    msg->range_start = _decode_size(range_start);
    cbor_decref(&range_start);

    cbor_item_t* range_end = cbor_array_get(item, 4);
    msg->range_end = _decode_size(range_end);
    cbor_decref(&range_end);
  }

  return 0;
}

void client_api_get_request_destroy(client_api_get_request_t* msg) {
  if (msg == NULL) return;
  free(msg->ori_string);
}

// --- GET Response Start ---
// [type, content_type, content_length, has_range, range_start?, range_end?]

cbor_item_t* client_api_get_response_start_encode(const client_api_get_response_start_t* msg) {
  cbor_item_t* array;
  cbor_item_t* item;

  if (msg->has_range) {
    array = cbor_new_definite_array(6);
  } else {
    array = cbor_new_definite_array(4);
  }

  item = cbor_build_uint8(CLIENT_API_GET_RESPONSE_START);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->content_type);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint64(msg->content_length);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->has_range);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->has_range) {
    item = cbor_build_uint64(msg->range_start);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);

    item = cbor_build_uint64(msg->range_end);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

int client_api_get_response_start_decode(cbor_item_t* item, client_api_get_response_start_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* content_type = cbor_array_get(item, 1);
  msg->content_type = _decode_string(content_type, OFFS_MAX_CONTENT_TYPE_LEN);
  cbor_decref(&content_type);

  if (validate_content_type(msg->content_type) != 0) {
    free(msg->content_type);
    msg->content_type = NULL;
    return -1;
  }

  cbor_item_t* content_length = cbor_array_get(item, 2);
  msg->content_length = _decode_size(content_length);
  cbor_decref(&content_length);

  cbor_item_t* has_range = cbor_array_get(item, 3);
  msg->has_range = (uint8_t)cbor_get_uint8(has_range);
  cbor_decref(&has_range);

  if (cbor_array_size(item) >= 6 && msg->has_range) {
    cbor_item_t* range_start = cbor_array_get(item, 4);
    msg->range_start = _decode_size(range_start);
    cbor_decref(&range_start);

    cbor_item_t* range_end = cbor_array_get(item, 5);
    msg->range_end = _decode_size(range_end);
    cbor_decref(&range_end);
  }

  return 0;
}

void client_api_get_response_start_destroy(client_api_get_response_start_t* msg) {
  if (msg == NULL) return;
  free(msg->content_type);
}

// --- GET Data ---
// [type, bytestring]

cbor_item_t* client_api_get_data_encode(const client_api_get_data_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_GET_DATA);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_get_data_decode(cbor_item_t* item, client_api_get_data_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* data_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);
  return 0;
}

// --- GET End ---
// [type]

cbor_item_t* client_api_get_end_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_GET_END);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

int client_api_get_end_decode(cbor_item_t* item) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 1) return -1;
  cbor_item_t* type_item = cbor_array_get(item, 0);
  uint8_t type = (uint8_t)cbor_get_uint8(type_item);
  cbor_decref(&type_item);
  return type == CLIENT_API_GET_END ? 0 : -1;
}

// --- Error ---
// [type, status_code, message_string]

cbor_item_t* client_api_error_encode(const client_api_error_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_ERROR);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status_code);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = _encode_string(msg->message);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_error_decode(cbor_item_t* item, client_api_error_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_code = cbor_array_get(item, 1);
  msg->status_code = (uint8_t)cbor_get_uint8(status_code);
  cbor_decref(&status_code);

  cbor_item_t* message = cbor_array_get(item, 2);
  msg->message = _decode_string(message, 1024);
  cbor_decref(&message);
  return 0;
}

void client_api_error_destroy(client_api_error_t* msg) {
  if (msg == NULL) return;
  free(msg->message);
}

// --- Auth Request ---
// [type, bytestring(api_key)]

cbor_item_t* client_api_auth_request_encode(const client_api_auth_request_t* auth) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_AUTH_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(auth->api_key, auth->api_key_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_auth_request_decode(cbor_item_t* item, client_api_auth_request_t* auth) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(auth, 0, sizeof(*auth));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_AUTH_REQUEST) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* key_item = cbor_array_get(item, 1);
  auth->api_key = _decode_bytestring(key_item, &auth->api_key_len);
  cbor_decref(&key_item);

  if (auth->api_key == NULL || auth->api_key_len == 0) {
    free(auth->api_key);
    auth->api_key = NULL;
    return -1;
  }
  return 0;
}

void client_api_auth_request_destroy(client_api_auth_request_t* auth) {
  if (auth == NULL) return;
  if (auth->api_key != NULL) {
    memset(auth->api_key, 0, auth->api_key_len);
    free(auth->api_key);
  }
  memset(auth, 0, sizeof(*auth));
}

// --- Block PUT Request ---
// [type, data: bstr, encoding: uint]

cbor_item_t* client_api_block_put_request_encode(const client_api_block_put_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->encoding);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_put_request_decode(cbor_item_t* item, client_api_block_put_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* data_item = cbor_array_get(item, 1);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size == 0 || msg->data_size > 128000) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data = get_memory(msg->data_size);
  memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  cbor_decref(&data_item);

  if (cbor_array_size(item) >= 3) {
    cbor_item_t* enc_item = cbor_array_get(item, 2);
    if (cbor_isa_uint(enc_item)) {
      msg->encoding = cbor_get_uint8(enc_item);
    }
    cbor_decref(&enc_item);
  }

  return 0;
}

void client_api_block_put_request_destroy(client_api_block_put_request_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- Block PUT Response ---
// [type, status: uint, hash: bstr|tstr]

cbor_item_t* client_api_block_put_response_encode(const client_api_block_put_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_PUT_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->hash_is_text) {
    item = cbor_build_string((const char*)msg->hash_data);
  } else {
    item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_put_response_decode(cbor_item_t* item, client_api_block_put_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  cbor_item_t* hash_item = cbor_array_get(item, 2);
  if (cbor_isa_string(hash_item)) {
    msg->hash_is_text = 1;
    msg->hash_len = cbor_string_length(hash_item);
    msg->hash_data = get_memory(msg->hash_len + 1);
    memcpy(msg->hash_data, cbor_string_handle(hash_item), msg->hash_len);
    msg->hash_data[msg->hash_len] = '\0';
  } else if (cbor_isa_bytestring(hash_item)) {
    msg->hash_is_text = 0;
    msg->hash_len = cbor_bytestring_length(hash_item);
    if (msg->hash_len > 0) {
      msg->hash_data = get_memory(msg->hash_len);
      memcpy(msg->hash_data, cbor_bytestring_handle(hash_item), msg->hash_len);
    }
  } else {
    cbor_decref(&hash_item);
    return -1;
  }
  cbor_decref(&hash_item);

  return 0;
}

void client_api_block_put_response_destroy(client_api_block_put_response_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}

// --- Block GET Request ---
// [type, hash: bstr]

cbor_item_t* client_api_block_get_request_encode(const client_api_block_get_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_GET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_get_request_decode(cbor_item_t* item, client_api_block_get_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* hash_item = cbor_array_get(item, 1);
  msg->hash_data = _decode_bytestring(hash_item, &msg->hash_len);
  cbor_decref(&hash_item);

  if (msg->hash_data == NULL || msg->hash_len != 32) {
    free(msg->hash_data);
    msg->hash_data = NULL;
    return -1;
  }
  return 0;
}

void client_api_block_get_request_destroy(client_api_block_get_request_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}

// --- Block GET Response ---
// [type, status: uint, data: bstr]

cbor_item_t* client_api_block_get_response_encode(const client_api_block_get_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_GET_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->data != NULL && msg->data_size > 0) {
    item = cbor_build_bytestring(msg->data, msg->data_size);
  } else {
    item = cbor_build_bytestring(&_empty_byte_sentinel, 0);
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_get_response_decode(cbor_item_t* item, client_api_block_get_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  cbor_item_t* data_item = cbor_array_get(item, 2);
  if (!cbor_is_null(data_item) && cbor_isa_bytestring(data_item)) {
    msg->data_size = cbor_bytestring_length(data_item);
    if (msg->data_size > 128000) {
      cbor_decref(&data_item);
      return -1;
    }
    if (msg->data_size > 0) {
      msg->data = get_memory(msg->data_size);
      memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
    }
  }
  cbor_decref(&data_item);

  return 0;
}

void client_api_block_get_response_destroy(client_api_block_get_response_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- Block DELETE Request ---
// [type, hash: bstr]

cbor_item_t* client_api_block_delete_request_encode(const client_api_block_delete_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_DELETE_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->hash_data, msg->hash_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_delete_request_decode(cbor_item_t* item, client_api_block_delete_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* hash_item = cbor_array_get(item, 1);
  msg->hash_data = _decode_bytestring(hash_item, &msg->hash_len);
  cbor_decref(&hash_item);

  if (msg->hash_data == NULL || msg->hash_len != 32) {
    free(msg->hash_data);
    msg->hash_data = NULL;
    return -1;
  }
  return 0;
}

void client_api_block_delete_request_destroy(client_api_block_delete_request_t* msg) {
  if (msg == NULL) return;
  free(msg->hash_data);
}

// --- Block DELETE Response ---
// [type, status: uint]

cbor_item_t* client_api_block_delete_response_encode(const client_api_block_delete_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_BLOCK_DELETE_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_block_delete_response_decode(cbor_item_t* item, client_api_block_delete_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* status_item = cbor_array_get(item, 1);
  msg->status = (uint8_t)cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  return 0;
}

void client_api_block_delete_response_destroy(client_api_block_delete_response_t* msg) {
  (void)msg;
}

// --- Health Request ---
// [type] — no payload

cbor_item_t* client_api_health_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_HEALTH_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Health Response ---
// [type, json_string: tstr]

cbor_item_t* client_api_health_response_encode(const client_api_health_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_HEALTH_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->json_data != NULL) {
    item = cbor_build_string(msg->json_data);
  } else {
    item = cbor_build_string("");
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_health_response_decode(cbor_item_t* item, client_api_health_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_HEALTH_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* json_item = cbor_array_get(item, 1);
  if (cbor_isa_string(json_item)) {
    size_t len = cbor_string_length(json_item);
    msg->json_data = get_memory(len + 1);
    memcpy(msg->json_data, cbor_string_handle(json_item), len);
    msg->json_data[len] = '\0';
  } else {
    msg->json_data = get_memory(1);
    msg->json_data[0] = '\0';
  }
  cbor_decref(&json_item);

  return 0;
}

void client_api_health_response_destroy(client_api_health_response_t* msg) {
  if (msg != NULL && msg->json_data != NULL) {
    free(msg->json_data);
    msg->json_data = NULL;
  }
}

// --- Update Status Request ---
// [type] — no payload

cbor_item_t* client_api_update_status_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_UPDATE_STATUS_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Update Status Response ---
// [type, json_string: tstr]

cbor_item_t* client_api_update_status_response_encode(const client_api_update_status_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_UPDATE_STATUS_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->json_data != NULL) {
    item = cbor_build_string(msg->json_data);
  } else {
    item = cbor_build_string("");
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_update_status_response_decode(cbor_item_t* item, client_api_update_status_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_UPDATE_STATUS_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* json_item = cbor_array_get(item, 1);
  if (cbor_isa_string(json_item)) {
    size_t len = cbor_string_length(json_item);
    msg->json_data = get_memory(len + 1);
    memcpy(msg->json_data, cbor_string_handle(json_item), len);
    msg->json_data[len] = '\0';
  } else {
    msg->json_data = get_memory(1);
    msg->json_data[0] = '\0';
  }
  cbor_decref(&json_item);

  return 0;
}

void client_api_update_status_response_destroy(client_api_update_status_response_t* msg) {
  if (msg != NULL && msg->json_data != NULL) {
    free(msg->json_data);
    msg->json_data = NULL;
  }
}

// --- Peer Info Request ---
// [type] — no payload

cbor_item_t* client_api_peer_info_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_PEER_INFO_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Peer Info Response ---
// [type, format_byte, data: bstr]

cbor_item_t* client_api_peer_info_response_encode(const client_api_peer_info_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PEER_INFO_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->format);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_peer_info_response_decode(cbor_item_t* item, client_api_peer_info_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_PEER_INFO_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* format_item = cbor_array_get(item, 1);
  if (!cbor_isa_uint(format_item)) {
    cbor_decref(&format_item);
    return -1;
  }
  msg->format = cbor_get_uint8(format_item);
  cbor_decref(&format_item);

  cbor_item_t* data_item = cbor_array_get(item, 2);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 65536) {
    cbor_decref(&data_item);
    return -1;
  }
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);

  return 0;
}

void client_api_peer_info_response_destroy(client_api_peer_info_response_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- Peer Connect ---
// [type, format_byte, data: bstr]

cbor_item_t* client_api_peer_connect_encode(const client_api_peer_connect_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PEER_CONNECT);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->format);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_peer_connect_decode(cbor_item_t* item, client_api_peer_connect_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_PEER_CONNECT) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* format_item = cbor_array_get(item, 1);
  if (!cbor_isa_uint(format_item)) {
    cbor_decref(&format_item);
    return -1;
  }
  msg->format = cbor_get_uint8(format_item);
  cbor_decref(&format_item);

  cbor_item_t* data_item = cbor_array_get(item, 2);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 65536) {
    cbor_decref(&data_item);
    return -1;
  }
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);

  return 0;
}

void client_api_peer_connect_destroy(client_api_peer_connect_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- Peer Connect Result ---
// [type, status: uint]

cbor_item_t* client_api_peer_connect_result_encode(const client_api_peer_connect_result_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PEER_CONNECT_RESULT);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_peer_connect_result_decode(cbor_item_t* item, client_api_peer_connect_result_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_PEER_CONNECT_RESULT) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* status_item = cbor_array_get(item, 1);
  if (!cbor_isa_uint(status_item)) {
    cbor_decref(&status_item);
    return -1;
  }
  msg->status = cbor_get_uint8(status_item);
  cbor_decref(&status_item);

  return 0;
}

void client_api_peer_connect_result_destroy(client_api_peer_connect_result_t* msg) {
  (void)msg;
}

// --- Peer List Request ---
// [type] — no payload

cbor_item_t* client_api_peer_list_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_PEER_LIST_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Peer List Response ---
// [type, peers: cbor_array]

cbor_item_t* client_api_peer_list_response_encode(const client_api_peer_list_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_PEER_LIST_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->peers != NULL) {
    (void)cbor_array_push(array, msg->peers);
  } else {
    item = cbor_new_definite_array(0);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

void client_api_peer_list_response_destroy(client_api_peer_list_response_t* msg) {
  if (msg == NULL) return;
  if (msg->peers != NULL) {
    cbor_decref(&msg->peers);
  }
}

int client_api_peer_list_response_decode(cbor_item_t* item, client_api_peer_list_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_PEER_LIST_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);
  msg->peers = cbor_array_get(item, 1);
  return 0;
}

// --- Friend Add ---
// [type, format_byte, data: bstr]

cbor_item_t* client_api_friend_add_encode(const client_api_friend_add_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_FRIEND_ADD);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->format);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->data, msg->data_size);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_friend_add_decode(cbor_item_t* item, client_api_friend_add_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_FRIEND_ADD) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* format_item = cbor_array_get(item, 1);
  if (!cbor_isa_uint(format_item)) {
    cbor_decref(&format_item);
    return -1;
  }
  msg->format = cbor_get_uint8(format_item);
  cbor_decref(&format_item);

  cbor_item_t* data_item = cbor_array_get(item, 2);
  if (!cbor_isa_bytestring(data_item)) {
    cbor_decref(&data_item);
    return -1;
  }
  msg->data_size = cbor_bytestring_length(data_item);
  if (msg->data_size > 65536) {
    cbor_decref(&data_item);
    return -1;
  }
  if (msg->data_size > 0) {
    msg->data = get_memory(msg->data_size);
    memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
  }
  cbor_decref(&data_item);

  return 0;
}

void client_api_friend_add_destroy(client_api_friend_add_t* msg) {
  if (msg == NULL) return;
  free(msg->data);
}

// --- Friend Remove ---
// [type, node_id: bstr]

cbor_item_t* client_api_friend_remove_encode(const client_api_friend_remove_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_FRIEND_REMOVE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_bytestring(msg->node_id, msg->node_id_len);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_friend_remove_decode(cbor_item_t* item, client_api_friend_remove_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_FRIEND_REMOVE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* node_id_item = cbor_array_get(item, 1);
  msg->node_id = _decode_bytestring(node_id_item, &msg->node_id_len);
  cbor_decref(&node_id_item);

  if (msg->node_id == NULL || msg->node_id_len == 0) {
    free(msg->node_id);
    msg->node_id = NULL;
    return -1;
  }
  return 0;
}

void client_api_friend_remove_destroy(client_api_friend_remove_t* msg) {
  if (msg == NULL) return;
  free(msg->node_id);
}

// --- Friend List Request ---
// [type] — no payload

cbor_item_t* client_api_friend_list_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_FRIEND_LIST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Friend List Response ---
// [type, friends: cbor_array]

cbor_item_t* client_api_friend_list_response_encode(const client_api_friend_list_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_FRIEND_LIST_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->friends != NULL) {
    (void)cbor_array_push(array, msg->friends);
  } else {
    item = cbor_new_definite_array(0);
    (void)cbor_array_push(array, item);
    cbor_decref(&item);
  }

  return array;
}

void client_api_friend_list_response_destroy(client_api_friend_list_response_t* msg) {
  if (msg == NULL) return;
  if (msg->friends != NULL) {
    cbor_decref(&msg->friends);
  }
}

int client_api_friend_list_response_decode(cbor_item_t* item, client_api_friend_list_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));
  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_FRIEND_LIST_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);
  msg->friends = cbor_array_get(item, 1);
  return 0;
}

// --- Config Show Request ---
// [type] — no payload

cbor_item_t* client_api_config_show_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_SHOW_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Config Show Response ---
// [type, json_string: tstr]

cbor_item_t* client_api_config_show_response_encode(const client_api_config_show_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_CONFIG_SHOW_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  if (msg->json_data != NULL) {
    item = cbor_build_string(msg->json_data);
  } else {
    item = cbor_build_string("");
  }
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_config_show_response_decode(cbor_item_t* item, client_api_config_show_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 2) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_CONFIG_SHOW_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* json_item = cbor_array_get(item, 1);
  if (cbor_isa_string(json_item)) {
    size_t len = cbor_string_length(json_item);
    msg->json_data = get_memory(len + 1);
    memcpy(msg->json_data, cbor_string_handle(json_item), len);
    msg->json_data[len] = '\0';
  } else {
    msg->json_data = get_memory(1);
    msg->json_data[0] = '\0';
  }
  cbor_decref(&json_item);

  return 0;
}

void client_api_config_show_response_destroy(client_api_config_show_response_t* msg) {
  if (msg != NULL && msg->json_data != NULL) {
    free(msg->json_data);
    msg->json_data = NULL;
  }
}

// --- Config Set Request ---
// [type, field: tstr, value: tstr]

cbor_item_t* client_api_config_set_request_encode(const client_api_config_set_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_CONFIG_SET_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(msg->field != NULL ? msg->field : "");
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(msg->value != NULL ? msg->value : "");
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_config_set_request_decode(cbor_item_t* item, client_api_config_set_request_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_CONFIG_SET_REQUEST) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* field_item = cbor_array_get(item, 1);
  if (cbor_isa_string(field_item)) {
    size_t len = cbor_string_length(field_item);
    msg->field = get_memory(len + 1);
    memcpy(msg->field, cbor_string_handle(field_item), len);
    msg->field[len] = '\0';
  } else {
    msg->field = get_memory(1);
    msg->field[0] = '\0';
  }
  cbor_decref(&field_item);

  cbor_item_t* value_item = cbor_array_get(item, 2);
  if (cbor_isa_string(value_item)) {
    size_t len = cbor_string_length(value_item);
    msg->value = get_memory(len + 1);
    memcpy(msg->value, cbor_string_handle(value_item), len);
    msg->value[len] = '\0';
  } else {
    msg->value = get_memory(1);
    msg->value[0] = '\0';
  }
  cbor_decref(&value_item);

  return 0;
}

void client_api_config_set_request_destroy(client_api_config_set_request_t* msg) {
  if (msg == NULL) return;
  if (msg->field != NULL) {
    free(msg->field);
    msg->field = NULL;
  }
  if (msg->value != NULL) {
    free(msg->value);
    msg->value = NULL;
  }
}

// --- Config Set Response ---
// [type, status: uint, restart_required: uint, message: tstr]

cbor_item_t* client_api_config_set_response_encode(const client_api_config_set_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_CONFIG_SET_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->restart_required ? 1 : 0);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(msg->message != NULL ? msg->message : "");
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_config_set_response_decode(cbor_item_t* item, client_api_config_set_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 4) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_CONFIG_SET_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* status_item = cbor_array_get(item, 1);
  if (cbor_isa_uint(status_item)) {
    msg->status = cbor_get_uint8(status_item);
  }
  cbor_decref(&status_item);

  cbor_item_t* restart_item = cbor_array_get(item, 2);
  if (cbor_isa_uint(restart_item)) {
    msg->restart_required = cbor_get_uint8(restart_item) ? 1 : 0;
  }
  cbor_decref(&restart_item);

  cbor_item_t* msg_item = cbor_array_get(item, 3);
  if (cbor_isa_string(msg_item)) {
    size_t len = cbor_string_length(msg_item);
    msg->message = get_memory(len + 1);
    memcpy(msg->message, cbor_string_handle(msg_item), len);
    msg->message[len] = '\0';
  } else {
    msg->message = get_memory(1);
    msg->message[0] = '\0';
  }
  cbor_decref(&msg_item);

  return 0;
}

void client_api_config_set_response_destroy(client_api_config_set_response_t* msg) {
  if (msg == NULL) return;
  if (msg->message != NULL) {
    free(msg->message);
    msg->message = NULL;
  }
}

// --- Config Reload Request ---
// [type] — no payload

cbor_item_t* client_api_config_reload_request_encode(void) {
  cbor_item_t* array = cbor_new_definite_array(1);
  cbor_item_t* item = cbor_build_uint8(CLIENT_API_CONFIG_RELOAD_REQUEST);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
  return array;
}

// --- Config Reload Response ---
// [type, status: uint, message: tstr]

cbor_item_t* client_api_config_reload_response_encode(const client_api_config_reload_response_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* item;

  item = cbor_build_uint8(CLIENT_API_CONFIG_RELOAD_RESPONSE);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_uint8(msg->status);
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  item = cbor_build_string(msg->message != NULL ? msg->message : "");
  (void)cbor_array_push(array, item);
  cbor_decref(&item);

  return array;
}

int client_api_config_reload_response_decode(cbor_item_t* item, client_api_config_reload_response_t* msg) {
  if (!cbor_isa_array(item) || cbor_array_size(item) < 3) return -1;
  memset(msg, 0, sizeof(*msg));

  cbor_item_t* type_item = cbor_array_get(item, 0);
  if (!cbor_isa_uint(type_item) || cbor_get_uint8(type_item) != CLIENT_API_CONFIG_RELOAD_RESPONSE) {
    cbor_decref(&type_item);
    return -1;
  }
  cbor_decref(&type_item);

  cbor_item_t* status_item = cbor_array_get(item, 1);
  if (cbor_isa_uint(status_item)) {
    msg->status = cbor_get_uint8(status_item);
  }
  cbor_decref(&status_item);

  cbor_item_t* msg_item = cbor_array_get(item, 2);
  if (cbor_isa_string(msg_item)) {
    size_t len = cbor_string_length(msg_item);
    msg->message = get_memory(len + 1);
    memcpy(msg->message, cbor_string_handle(msg_item), len);
    msg->message[len] = '\0';
  } else {
    msg->message = get_memory(1);
    msg->message[0] = '\0';
  }
  cbor_decref(&msg_item);

  return 0;
}

void client_api_config_reload_response_destroy(client_api_config_reload_response_t* msg) {
  if (msg == NULL) return;
  if (msg->message != NULL) {
    free(msg->message);
    msg->message = NULL;
  }
}