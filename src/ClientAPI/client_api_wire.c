//
// Created by victor on 5/20/26.
//

#include "client_api_wire.h"
#include "../Util/allocator.h"
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

// --- Helper: safe cbor_get_uint64 → size_t with overflow guard ---
static size_t _decode_size(cbor_item_t* item) {
  uint64_t val = cbor_get_uint64(item);
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
  uint8_t type = (uint8_t)cbor_get_uint8(type_item);
  cbor_decref(&type_item);
  return type;
}

// --- PUT Request ---
// [type, content_type, file_name, stream_length, server_address, data]

cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg) {
  cbor_item_t* array = cbor_new_definite_array(6);
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
    item = cbor_build_bytestring(NULL, 0);
  }
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
  if (validate_content_type(msg->content_type) != 0) return -1;
  if (validate_file_name(msg->file_name) != 0) return -1;
  if (msg->stream_length == 0 || msg->stream_length > OFFS_MAX_CBOR_MESSAGE_SIZE) return -1;

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
        return -1;
      }
      if (msg->data_size > 0) {
        msg->data = get_memory(msg->data_size);
        memcpy(msg->data, cbor_bytestring_handle(data_item), msg->data_size);
      }
    }
    cbor_decref(&data_item);
  }

  return 0;
}

void client_api_put_request_destroy(client_api_put_request_t* msg) {
  if (msg == NULL) return;
  free(msg->content_type);
  free(msg->file_name);
  free(msg->server_address);
  free(msg->data);
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

  if (validate_ori_string(msg->ori_string) != 0) return -1;

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

  if (validate_content_type(msg->content_type) != 0) return -1;

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