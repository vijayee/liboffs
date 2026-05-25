//
// Created by victor on 5/20/26.
//

#ifndef OFFS_CLIENT_API_WIRE_H
#define OFFS_CLIENT_API_WIRE_H

#include <cbor.h>
#include <stdint.h>
#include <stddef.h>

// Client API wire protocol message types (first element of every CBOR array)
#define CLIENT_API_PUT_REQUEST          1
#define CLIENT_API_PUT_DATA             2
#define CLIENT_API_PUT_END              3
#define CLIENT_API_PUT_RESPONSE         4
#define CLIENT_API_GET_REQUEST          5
#define CLIENT_API_GET_RESPONSE_START   6
#define CLIENT_API_GET_DATA             7
#define CLIENT_API_GET_END              8
#define CLIENT_API_ERROR                11
#define CLIENT_API_AUTH_REQUEST        12

// Status codes for responses
#define CLIENT_API_STATUS_OK                0
#define CLIENT_API_STATUS_BAD_REQUEST       1
#define CLIENT_API_STATUS_NOT_FOUND         2
#define CLIENT_API_STATUS_INTERNAL_ERROR    3
#define CLIENT_API_STATUS_RANGE_NOT_SATISFIABLE 4
#define CLIENT_API_STATUS_UNAUTHORIZED      5

// --- PUT Request ---
// [type, content_type, file_name, stream_length, server_address, data, recycler_urls, temporary]
// data is NULL/empty for streaming uploads; subsequent PUT_DATA frames carry the body
typedef struct {
  char* content_type;
  char* file_name;
  size_t stream_length;
  char* server_address;   // may be NULL
  uint8_t* data;          // may be NULL for streaming uploads
  size_t data_size;
  char** recycler_urls;   // NULL or array of URL strings
  size_t recycler_count;  // 0 if no recycler
  uint8_t temporary;      // 0 or 1
} client_api_put_request_t;

// --- PUT Data (streaming upload chunk) ---
// [type, bytestring]
typedef struct {
  uint8_t* data;
  size_t data_size;
} client_api_put_data_t;

// --- PUT End (streaming upload complete) ---
// [type] — no payload
// (no struct needed, encode/decode handle it directly)

// --- PUT Response ---
// [type, ori_string]
typedef struct {
  char* ori_string;       // caller must free()
} client_api_put_response_t;

// --- GET Request ---
// [type, ori_string, has_range, range_start?, range_end?]
typedef struct {
  char* ori_string;       // the OFF URL to retrieve
  uint8_t has_range;
  size_t range_start;
  size_t range_end;
} client_api_get_request_t;

// --- GET Response Start ---
// [type, content_type, content_length, has_range, range_start?, range_end?]
typedef struct {
  char* content_type;
  size_t content_length;
  uint8_t has_range;
  size_t range_start;
  size_t range_end;
} client_api_get_response_start_t;

// --- GET Data (download chunk) ---
// [type, bytestring]
typedef struct {
  uint8_t* data;
  size_t data_size;
} client_api_get_data_t;

// --- GET End (download complete) ---
// [type] — no payload
// (no struct needed, encode/decode handle it directly)

// --- Error ---
// [type, status_code, message_string]
typedef struct {
  uint8_t status_code;
  char* message;           // caller must free()
} client_api_error_t;

// --- Auth Request ---
// [type, bytestring(api_key)]
typedef struct {
  uint8_t* api_key;
  size_t   api_key_len;
} client_api_auth_request_t;

// Encode functions — return CBOR item (caller must cbor_decref)
cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg);
cbor_item_t* client_api_put_data_encode(const client_api_put_data_t* msg);
cbor_item_t* client_api_put_end_encode(void);
cbor_item_t* client_api_put_response_encode(const client_api_put_response_t* msg);
cbor_item_t* client_api_get_request_encode(const client_api_get_request_t* msg);
cbor_item_t* client_api_get_response_start_encode(const client_api_get_response_start_t* msg);
cbor_item_t* client_api_get_data_encode(const client_api_get_data_t* msg);
cbor_item_t* client_api_get_end_encode(void);
cbor_item_t* client_api_error_encode(const client_api_error_t* msg);

// Decode functions — fill existing struct, return 0 on success, -1 on error
int client_api_put_request_decode(cbor_item_t* item, client_api_put_request_t* msg);
int client_api_put_data_decode(cbor_item_t* item, client_api_put_data_t* msg);
int client_api_put_end_decode(cbor_item_t* item);
int client_api_put_response_decode(cbor_item_t* item, client_api_put_response_t* msg);
int client_api_get_request_decode(cbor_item_t* item, client_api_get_request_t* msg);
int client_api_get_response_start_decode(cbor_item_t* item, client_api_get_response_start_t* msg);
int client_api_get_data_decode(cbor_item_t* item, client_api_get_data_t* msg);
int client_api_get_end_decode(cbor_item_t* item);
int client_api_error_decode(cbor_item_t* item, client_api_error_t* msg);

cbor_item_t* client_api_auth_request_encode(const client_api_auth_request_t* auth);
int client_api_auth_request_decode(cbor_item_t* item, client_api_auth_request_t* auth);
void client_api_auth_request_destroy(client_api_auth_request_t* auth);

// Helper: extract type byte from CBOR item
uint8_t client_api_wire_get_type(cbor_item_t* item);

// Destroy helpers for types with nested allocations
void client_api_put_request_destroy(client_api_put_request_t* msg);
void client_api_put_response_destroy(client_api_put_response_t* msg);
void client_api_get_request_destroy(client_api_get_request_t* msg);
void client_api_get_response_start_destroy(client_api_get_response_start_t* msg);
void client_api_get_data_destroy(client_api_get_data_t* msg);
void client_api_error_destroy(client_api_error_t* msg);

#endif // OFFS_CLIENT_API_WIRE_H