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
#define CLIENT_API_BLOCK_PUT_REQUEST     13
#define CLIENT_API_BLOCK_PUT_RESPONSE    14
#define CLIENT_API_BLOCK_GET_REQUEST     15
#define CLIENT_API_BLOCK_GET_RESPONSE    16
#define CLIENT_API_BLOCK_DELETE_REQUEST  17
#define CLIENT_API_BLOCK_DELETE_RESPONSE 18
#define CLIENT_API_HEALTH_REQUEST   19
#define CLIENT_API_HEALTH_RESPONSE  20
#define CLIENT_API_PEER_INFO_REQUEST      21
#define CLIENT_API_PEER_INFO_RESPONSE     22
#define CLIENT_API_PEER_CONNECT           23
#define CLIENT_API_PEER_CONNECT_RESULT    24
#define CLIENT_API_PEER_LIST_REQUEST      25
#define CLIENT_API_PEER_LIST_RESPONSE     26
#define CLIENT_API_FRIEND_ADD             27
#define CLIENT_API_FRIEND_REMOVE          28
#define CLIENT_API_FRIEND_LIST            29
#define CLIENT_API_FRIEND_LIST_RESPONSE   30
#define CLIENT_API_UPDATE_STATUS_REQUEST  31
#define CLIENT_API_UPDATE_STATUS_RESPONSE 32
#define CLIENT_API_CONFIG_SHOW_REQUEST    33
#define CLIENT_API_CONFIG_SHOW_RESPONSE   34
#define CLIENT_API_CONFIG_SET_REQUEST     35
#define CLIENT_API_CONFIG_SET_RESPONSE    36
#define CLIENT_API_CONFIG_RELOAD_REQUEST  37
#define CLIENT_API_CONFIG_RELOAD_RESPONSE 38
#define CLIENT_API_LOAD_REQUEST            39
#define CLIENT_API_LOAD_PROGRESS           40
#define CLIENT_API_LOAD_END                41
#define CLIENT_API_REP_MARK_PERMANENT_REQUEST   42
#define CLIENT_API_REP_MARK_PERMANENT_RESPONSE  43
#define CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST  44
#define CLIENT_API_REP_DELETE_EPHEMERAL_RESPONSE 45
#define CLIENT_API_REP_PIN_REQUEST               46
#define CLIENT_API_REP_PIN_RESPONSE              47
#define CLIENT_API_REP_UNPIN_REQUEST             48
#define CLIENT_API_REP_UNPIN_RESPONSE            49
#define CLIENT_API_EPHEMERAL_LIST_REQUEST        50
#define CLIENT_API_EPHEMERAL_LIST_RESPONSE       51

/* Representation-op responses are the adjacent pair (request + 1); the
 * transport handler derives the response code arithmetically, so a renumbering
 * that breaks the pairing must fail at compile time. */
_Static_assert(CLIENT_API_REP_MARK_PERMANENT_RESPONSE == CLIENT_API_REP_MARK_PERMANENT_REQUEST + 1,
               "rep op response codes must be request + 1");
_Static_assert(CLIENT_API_REP_DELETE_EPHEMERAL_RESPONSE == CLIENT_API_REP_DELETE_EPHEMERAL_REQUEST + 1,
               "rep op response codes must be request + 1");
_Static_assert(CLIENT_API_REP_PIN_RESPONSE == CLIENT_API_REP_PIN_REQUEST + 1,
               "rep op response codes must be request + 1");
_Static_assert(CLIENT_API_REP_UNPIN_RESPONSE == CLIENT_API_REP_UNPIN_REQUEST + 1,
               "rep op response codes must be request + 1");
_Static_assert(CLIENT_API_EPHEMERAL_LIST_RESPONSE == CLIENT_API_EPHEMERAL_LIST_REQUEST + 1,
               "rep op response codes must be request + 1");

// Status codes for responses
#define CLIENT_API_STATUS_OK                0
#define CLIENT_API_STATUS_BAD_REQUEST       1
#define CLIENT_API_STATUS_NOT_FOUND         2
#define CLIENT_API_STATUS_INTERNAL_ERROR    3
#define CLIENT_API_STATUS_RANGE_NOT_SATISFIABLE 4
#define CLIENT_API_STATUS_UNAUTHORIZED      5
#define CLIENT_API_STATUS_CONFLICT           6

// --- PUT Request ---
// [type, content_type, file_name, stream_length, server_address, data, recycler_urls, temporary, tuple_size?, recycle_ephemeral?]
// data is NULL/empty for streaming uploads; subsequent PUT_DATA frames carry the body
// tuple_size is optional at index 8: present when has_tuple_size != 0,
// absent otherwise for backward compatibility (8- or 9-element arrays decode
// as before). recycle_ephemeral is optional at index 9 and carries the
// recycle_ephemeral_e value (0 = none, 1 = commit, 2 = propagate); when it is
// present without tuple_size, index 8 holds a null placeholder so the value
// still lands at index 9 — decoders read it whenever the array has 10 elements.
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
  uint8_t has_tuple_size; // 0 if tuple_size field absent on the wire
  size_t tuple_size;      // requested erasure-coding width (when has_tuple_size)
  uint8_t recycle_ephemeral; // recycle_ephemeral_e value: 0 = none, 1 = commit, 2 = propagate
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

// --- Load Request ---
// [type, ori_string] or [type, ori_string, has_range, range_start, range_end] —
// the same optional-range shape as GET_REQUEST: an unranged request is 2
// elements, a ranged request carries the literal has_range flag (uint8 1) in
// position 2 followed by the two range bounds, and has_range on the struct is
// derived from the array shape on decode. Asks the daemon to pull the file's
// blocks into its block cache without sending file data; progress arrives as
// LOAD_PROGRESS frames, terminated by LOAD_END.
typedef struct {
  char* ori_string;
  uint8_t has_range;    /* 0 → no range elements; 1 → following two present */
  size_t range_start;
  size_t range_end;
} client_api_load_request_t;

// --- Load Progress ---
// [type, tuples_loaded: uint, tuples_total: uint]
// (tuples_total - tuples_loaded includes both in-flight and skipped tuples)

// --- Load End ---
// [type, status: uint, tuples_loaded: uint, tuples_total: uint]
// status: 0 = loaded, 1 = partial (some tuples skipped), 2 = failed
#define CLIENT_API_LOAD_STATUS_LOADED    0
#define CLIENT_API_LOAD_STATUS_PARTIAL   1
#define CLIENT_API_LOAD_STATUS_FAILED    2

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

// --- Block PUT Request ---
// [type, data: bstr, encoding: uint]
// encoding: 0 = raw bytes, 1 = base58 text
typedef struct {
  uint8_t* data;
  size_t data_size;
  uint8_t encoding;
} client_api_block_put_request_t;

// --- Block PUT Response ---
// [type, status: uint, hash: bstr|tstr]
// hash is raw bytes when encoding=0, base58 text string when encoding=1
typedef struct {
  uint8_t status;
  uint8_t* hash_data;
  size_t hash_len;
  uint8_t hash_is_text;  // 0 = bstr, 1 = tstr
} client_api_block_put_response_t;

// --- Block GET Request ---
// [type, hash: bstr]
typedef struct {
  uint8_t* hash_data;
  size_t hash_len;
} client_api_block_get_request_t;

// --- Block GET Response ---
// [type, status: uint, data: bstr]
typedef struct {
  uint8_t status;
  uint8_t* data;
  size_t data_size;
} client_api_block_get_response_t;

// --- Block DELETE Request ---
// [type, hash: bstr, force?: uint]
// force is optional at index 2: present-and-nonzero means remove even when
// the block is pinned or carries ephemeral claims; absent (or zero) keeps the
// old behavior, so 2-element legacy frames decode unchanged.
typedef struct {
  uint8_t* hash_data;
  size_t hash_len;
  uint8_t force;  // 0 = respect pins/claims, 1 = remove regardless
} client_api_block_delete_request_t;

// --- Block DELETE Response ---
// [type, status: uint]
typedef struct {
  uint8_t status;
} client_api_block_delete_response_t;

// --- Health Request ---
// [type] — no payload

// --- Health Response ---
// [type, json_string: tstr]
typedef struct {
  char* json_data;  // caller must free
} client_api_health_response_t;

// --- Update Status Request ---
// [type] — no payload

// --- Update Status Response ---
// [type, json_string: tstr]
typedef struct {
  char* json_data;  // caller must free
} client_api_update_status_response_t;

// --- Config Show Request ---
// [type] — no payload (returns the full current config as JSON)

// --- Config Show Response ---
// [type, json_string: tstr] — the config (or a single field) as JSON
typedef struct {
  char* json_data;  // caller must free
} client_api_config_show_response_t;

// --- Config Set Request ---
// [type, field: tstr, value: tstr]
// value is always sent as a string; the daemon parses it to the field's type.
// Used by set/add/remove/set-auth/generate-auth (the latter pre-hashes client-side).
typedef struct {
  char* field;
  char* value;
} client_api_config_set_request_t;

// --- Config Set Response ---
// [type, status: uint, restart_required: uint, message: tstr]
// status: 0 = staged (pending config written, restart to apply), 1 = rejected
typedef struct {
  uint8_t status;
  uint8_t restart_required;  // 0 or 1
  char* message;             // caller must free
} client_api_config_set_response_t;

// --- Config Reload Request ---
// [type] — no payload (apply pending config by restarting the node)

// --- Config Reload Response ---
// [type, status: uint, message: tstr]
// status: 0 = restart triggered, 1 = no pending config / error
typedef struct {
  uint8_t status;
  char* message;  // caller must free
} client_api_config_reload_response_t;

// --- Peer Info Request ---
// [type] or [type, format: uint]
// format: 0 = raw CBOR (default), 1 = Base58 text, 2 = PPM QR image

// --- Peer Info Response ---
// [type, format_byte, data: bstr]
// format_byte: 0 = raw CBOR, 1 = Base58 text, 2 = PPM QR image
typedef struct {
  uint8_t format;
  uint8_t* data;
  size_t data_size;
} client_api_peer_info_response_t;

// --- Peer Connect ---
// [type, format_byte, data: bstr]
typedef struct {
  uint8_t format;
  uint8_t* data;
  size_t data_size;
} client_api_peer_connect_t;

// --- Peer Connect Result ---
// [type, status: uint]
typedef struct {
  uint8_t status;
} client_api_peer_connect_result_t;

// --- Peer List Request ---
// [type] — no payload

// --- Peer List Response ---
// [type, peers: cbor_array]
// Each entry is a keyed map: node_id (bstr), connected (uint),
// is_friend (uint), rtt_ms (float).
#define CLIENT_API_PEER_LIST_KEY_NODE_ID    1
#define CLIENT_API_PEER_LIST_KEY_CONNECTED  2
#define CLIENT_API_PEER_LIST_KEY_IS_FRIEND  3
#define CLIENT_API_PEER_LIST_KEY_RTT_MS     4

typedef struct {
  cbor_item_t* peers;  // owned by struct, freed by _destroy
} client_api_peer_list_response_t;

// --- Friend Add ---
// [type, format_byte, data: bstr]
typedef struct {
  uint8_t format;
  uint8_t* data;
  size_t data_size;
} client_api_friend_add_t;

// --- Friend Remove ---
// [type, node_id: bstr]
typedef struct {
  uint8_t* node_id;
  size_t node_id_len;
} client_api_friend_remove_t;

// --- Friend List Request ---
// [type] — no payload

// --- Friend List Response ---
// [type, friends: cbor_array]
typedef struct {
  cbor_item_t* friends;  // owned by struct, freed by _destroy
} client_api_friend_list_response_t;

// --- Representation op request (mark permanent / delete ephemeral / pin / unpin) ---
// [type, url] — url is the full OFF URL string of the representation to
// operate on. The type byte selects the op (42/44/46/48); the layout is
// shared. Decode validates the URL with validate_ori_string and rejects
// empty or over-length strings (OFFS_MAX_ORI_STRING_LEN bound).
typedef struct {
  char* url;  // caller must free via client_api_rep_request_destroy
} client_api_rep_request_t;

// --- Representation op response ---
// [type, status: uint, blocks_touched: uint]
// status: 0 = ok, 1 = error (the walk failed: missing, malformed, or cyclic
// descriptor). blocks_touched counts the blocks the op visited.
typedef struct {
  int status;
  size_t blocks;
} client_api_rep_response_t;

// --- Ephemeral list response ---
// [type, status: uint, [[hash: bstr(32), claims: uint, pins: uint], ...]]
// One entry per ephemeral block. hashes holds count 32-byte buffers; decode
// rejects entries whose hash bytestring is not exactly 32 bytes.
typedef struct {
  int status;
  size_t count;
  uint8_t** hashes;  // count × 32-byte buffers, caller-owned via _destroy
  uint16_t* claims;  // count entries, caller-owned via _destroy
  uint32_t* pins;    // count entries, caller-owned via _destroy
} client_api_ephemeral_list_response_t;

// Encode functions — return CBOR item (caller must cbor_decref)
cbor_item_t* client_api_put_request_encode(const client_api_put_request_t* msg);
cbor_item_t* client_api_put_data_encode(const client_api_put_data_t* msg);
cbor_item_t* client_api_put_end_encode(void);
cbor_item_t* client_api_put_response_encode(const client_api_put_response_t* msg);
cbor_item_t* client_api_get_request_encode(const client_api_get_request_t* msg);
cbor_item_t* client_api_get_response_start_encode(const client_api_get_response_start_t* msg);
cbor_item_t* client_api_get_data_encode(const client_api_get_data_t* msg);
cbor_item_t* client_api_get_end_encode(void);
cbor_item_t* client_api_load_request_encode(const client_api_load_request_t* msg);
cbor_item_t* client_api_load_progress_encode(size_t tuples_loaded, size_t tuples_total);
cbor_item_t* client_api_load_end_encode(uint8_t status, size_t tuples_loaded, size_t tuples_total);
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
int client_api_load_request_decode(cbor_item_t* item, client_api_load_request_t* msg);
int client_api_load_progress_decode(cbor_item_t* item, size_t* tuples_loaded, size_t* tuples_total);
int client_api_load_end_decode(cbor_item_t* item, uint8_t* status, size_t* tuples_loaded, size_t* tuples_total);
int client_api_error_decode(cbor_item_t* item, client_api_error_t* msg);

cbor_item_t* client_api_auth_request_encode(const client_api_auth_request_t* auth);
int client_api_auth_request_decode(cbor_item_t* item, client_api_auth_request_t* auth);
void client_api_auth_request_destroy(client_api_auth_request_t* auth);

cbor_item_t* client_api_block_put_request_encode(const client_api_block_put_request_t* msg);
int client_api_block_put_request_decode(cbor_item_t* item, client_api_block_put_request_t* msg);
void client_api_block_put_request_destroy(client_api_block_put_request_t* msg);

cbor_item_t* client_api_block_put_response_encode(const client_api_block_put_response_t* msg);
int client_api_block_put_response_decode(cbor_item_t* item, client_api_block_put_response_t* msg);
void client_api_block_put_response_destroy(client_api_block_put_response_t* msg);

cbor_item_t* client_api_block_get_request_encode(const client_api_block_get_request_t* msg);
int client_api_block_get_request_decode(cbor_item_t* item, client_api_block_get_request_t* msg);
void client_api_block_get_request_destroy(client_api_block_get_request_t* msg);

cbor_item_t* client_api_block_get_response_encode(const client_api_block_get_response_t* msg);
int client_api_block_get_response_decode(cbor_item_t* item, client_api_block_get_response_t* msg);
void client_api_block_get_response_destroy(client_api_block_get_response_t* msg);

cbor_item_t* client_api_block_delete_request_encode(const client_api_block_delete_request_t* msg);
int client_api_block_delete_request_decode(cbor_item_t* item, client_api_block_delete_request_t* msg);
void client_api_block_delete_request_destroy(client_api_block_delete_request_t* msg);

cbor_item_t* client_api_block_delete_response_encode(const client_api_block_delete_response_t* msg);
int client_api_block_delete_response_decode(cbor_item_t* item, client_api_block_delete_response_t* msg);
void client_api_block_delete_response_destroy(client_api_block_delete_response_t* msg);

cbor_item_t* client_api_health_request_encode(void);
cbor_item_t* client_api_health_response_encode(const client_api_health_response_t* msg);
int client_api_health_response_decode(cbor_item_t* item, client_api_health_response_t* msg);
void client_api_health_response_destroy(client_api_health_response_t* msg);

cbor_item_t* client_api_update_status_request_encode(void);
cbor_item_t* client_api_update_status_response_encode(const client_api_update_status_response_t* msg);
int client_api_update_status_response_decode(cbor_item_t* item, client_api_update_status_response_t* msg);
void client_api_update_status_response_destroy(client_api_update_status_response_t* msg);

cbor_item_t* client_api_config_show_request_encode(void);
cbor_item_t* client_api_config_show_response_encode(const client_api_config_show_response_t* msg);
int client_api_config_show_response_decode(cbor_item_t* item, client_api_config_show_response_t* msg);
void client_api_config_show_response_destroy(client_api_config_show_response_t* msg);

cbor_item_t* client_api_config_set_request_encode(const client_api_config_set_request_t* msg);
int client_api_config_set_request_decode(cbor_item_t* item, client_api_config_set_request_t* msg);
void client_api_config_set_request_destroy(client_api_config_set_request_t* msg);

cbor_item_t* client_api_config_set_response_encode(const client_api_config_set_response_t* msg);
int client_api_config_set_response_decode(cbor_item_t* item, client_api_config_set_response_t* msg);
void client_api_config_set_response_destroy(client_api_config_set_response_t* msg);

cbor_item_t* client_api_config_reload_request_encode(void);
cbor_item_t* client_api_config_reload_response_encode(const client_api_config_reload_response_t* msg);
int client_api_config_reload_response_decode(cbor_item_t* item, client_api_config_reload_response_t* msg);
void client_api_config_reload_response_destroy(client_api_config_reload_response_t* msg);

cbor_item_t* client_api_peer_info_request_encode(void);
/* Same frame with an explicit response format byte:
   0 = raw CBOR, 1 = base58 text, 2 = PPM QR image. */
cbor_item_t* client_api_peer_info_request_encode_format(uint8_t format);
/* Decode [type] or [type, format]; *format is 0 for the 1-element form.
   Rejects unknown formats (anything > 2) and frames with extra elements. */
int client_api_peer_info_request_decode(cbor_item_t* item, uint8_t* format);

cbor_item_t* client_api_peer_info_response_encode(const client_api_peer_info_response_t* msg);
int client_api_peer_info_response_decode(cbor_item_t* item, client_api_peer_info_response_t* msg);
void client_api_peer_info_response_destroy(client_api_peer_info_response_t* msg);

cbor_item_t* client_api_peer_connect_encode(const client_api_peer_connect_t* msg);
int client_api_peer_connect_decode(cbor_item_t* item, client_api_peer_connect_t* msg);
void client_api_peer_connect_destroy(client_api_peer_connect_t* msg);

cbor_item_t* client_api_peer_connect_result_encode(const client_api_peer_connect_result_t* msg);
int client_api_peer_connect_result_decode(cbor_item_t* item, client_api_peer_connect_result_t* msg);
void client_api_peer_connect_result_destroy(client_api_peer_connect_result_t* msg);

cbor_item_t* client_api_peer_list_request_encode(void);
cbor_item_t* client_api_peer_list_response_encode(const client_api_peer_list_response_t* msg);
int client_api_peer_list_response_decode(cbor_item_t* item, client_api_peer_list_response_t* msg);
void client_api_peer_list_response_destroy(client_api_peer_list_response_t* msg);

cbor_item_t* client_api_friend_add_encode(const client_api_friend_add_t* msg);
int client_api_friend_add_decode(cbor_item_t* item, client_api_friend_add_t* msg);
void client_api_friend_add_destroy(client_api_friend_add_t* msg);

cbor_item_t* client_api_friend_remove_encode(const client_api_friend_remove_t* msg);
int client_api_friend_remove_decode(cbor_item_t* item, client_api_friend_remove_t* msg);
void client_api_friend_remove_destroy(client_api_friend_remove_t* msg);

cbor_item_t* client_api_friend_list_request_encode(void);
cbor_item_t* client_api_friend_list_response_encode(const client_api_friend_list_response_t* msg);
int client_api_friend_list_response_decode(cbor_item_t* item, client_api_friend_list_response_t* msg);
void client_api_friend_list_response_destroy(client_api_friend_list_response_t* msg);

// Helper: extract type byte from CBOR item
uint8_t client_api_wire_get_type(cbor_item_t* item);

// --- Representation ephemeral/pin wire ops ---
// The four request ops share one struct/codec; op_code picks the type byte
// (one of CLIENT_API_REP_*_REQUEST). Decode accepts any of the four request
// type bytes. The response codecs likewise take the matching response type
// byte (one of CLIENT_API_REP_*_RESPONSE) and accept any of the four.
cbor_item_t* client_api_rep_request_encode(int op_code, const client_api_rep_request_t* msg);
int client_api_rep_request_decode(cbor_item_t* item, client_api_rep_request_t* msg);
void client_api_rep_request_destroy(client_api_rep_request_t* msg);

cbor_item_t* client_api_rep_response_encode(int op_code, const client_api_rep_response_t* msg);
int client_api_rep_response_decode(cbor_item_t* item, client_api_rep_response_t* msg);
void client_api_rep_response_destroy(client_api_rep_response_t* msg);

cbor_item_t* client_api_ephemeral_list_response_encode(
    const client_api_ephemeral_list_response_t* msg);
int client_api_ephemeral_list_response_decode(cbor_item_t* item,
                                              client_api_ephemeral_list_response_t* msg);
void client_api_ephemeral_list_response_destroy(client_api_ephemeral_list_response_t* msg);

// Destroy helpers for types with nested allocations
void client_api_put_request_destroy(client_api_put_request_t* msg);
void client_api_put_response_destroy(client_api_put_response_t* msg);
void client_api_get_request_destroy(client_api_get_request_t* msg);
void client_api_get_response_start_destroy(client_api_get_response_start_t* msg);
void client_api_get_data_destroy(client_api_get_data_t* msg);
void client_api_load_request_destroy(client_api_load_request_t* msg);
void client_api_error_destroy(client_api_error_t* msg);

#endif // OFFS_CLIENT_API_WIRE_H