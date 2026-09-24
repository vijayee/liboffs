//
// Created by victor on 5/20/26.
//
#ifndef OFFS_CLIENT_H
#define OFFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct buffer_t buffer_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Client configuration for retry and timeout behavior */
typedef struct {
  uint32_t connect_timeout_ms;
  uint32_t ws_upgrade_timeout_ms;
  uint32_t poll_timeout_ms;
  uint32_t max_retries;
  uint32_t retry_base_delay_ms;
  /* PEM-encoded CA certificate file path for validating the server's TLS
   * cert on outbound wts:// connections. NULL (or zero-length) disables
   * validation. See audit #11. */
  const char* ca_path;
  /* When true, require a configured CA certificate for TLS connections.
   * If no ca_path is set and allow_secure is true, offs_client_connect
   * returns NULL. Default false runs without CA validation (not recommended
   * for production). See audit #11. */
  bool allow_secure;
} offs_client_config_t;

offs_client_config_t offs_client_config_default(void);

/* PUT options struct for extended parameters */
typedef struct {
  const char* content_type;
  const char* file_name;
  size_t stream_length;
  const char* server_address;
  const char** recycler_urls;
  size_t recycler_count;
  uint8_t temporary;
  uint8_t has_tuple_size;  /* nonzero = send tuple_size, 0 = omit (daemon default) */
  uint8_t tuple_size;      /* accepted range: 2..daemon max_tuple_size; values < 2 are rejected client-side */
  /* recycle_ephemeral_e value riding at wire index 9: 0 = none (ephemeral
     source blocks error the put), 1 = commit them permanent, 2 = propagate
     ephemeral. Sent only when nonzero (see client_api_put_request_encode). */
  uint8_t recycle_ephemeral;
} offs_put_options_t;

/* Opaque client handle */
typedef struct offs_client_t offs_client_t;

/* Callback types */
/* Payload ownership for every callback below: pointer arguments handed to a
 * callback (payload strings, data buffers, the flattened peer/friend arrays)
 * remain VALID until the consumer calls offs_client_release_payload on each
 * pointer exactly once (after copying whatever it wants to keep), or until
 * offs_client_destroy tears the client down. The library no longer frees the
 * payload itself when the callback returns, so the pointer stays valid after
 * the callback returns — cross-thread consumers (NativeCallable.listener,
 * etc.) may copy it later. Releasing a payload twice, releasing an unknown
 * pointer, or releasing after disconnect is a safe no-op. Unreleased
 * payloads are reclaimed at disconnect/destroy. */
typedef void (*offs_put_response_cb_t)(void* ctx, const char* ori_string);
/* ori_string is NULL when the PUT failed (daemon ERROR frame). */
typedef void (*offs_get_data_cb_t)(void* ctx, const uint8_t* data, size_t len);
typedef void (*offs_get_end_cb_t)(void* ctx);
typedef void (*offs_error_cb_t)(void* ctx, uint8_t status_code, const char* message);
typedef void (*offs_block_put_cb_t)(void* ctx, uint8_t status,
    const uint8_t* hash_data, size_t hash_len, uint8_t hash_is_text);
typedef void (*offs_block_get_cb_t)(void* ctx, uint8_t status,
    const uint8_t* data, size_t data_len);
typedef void (*offs_block_delete_cb_t)(void* ctx, uint8_t status);
typedef void (*offs_health_cb_t)(void* ctx, const char* json_response);
/* json_response is NULL when the health check failed (daemon ERROR frame) —
 * the callback type carries no status, so NULL is the failure signal. */
typedef void (*offs_peer_info_cb_t)(void* ctx, uint8_t format, const uint8_t* data, size_t data_len);
/* data is NULL when the peer_info request failed (daemon ERROR frame) — the
 * callback type carries no status, so NULL data is the failure signal. */
typedef void (*offs_peer_connect_cb_t)(void* ctx, uint8_t status);
typedef void (*offs_load_progress_cb_t)(void* ctx, size_t tuples_loaded, size_t tuples_total);
typedef void (*offs_load_end_cb_t)(void* ctx, uint8_t status, size_t tuples_loaded, size_t tuples_total);

/* Flattened peer list entry — mirrors the daemon's keyed peer map
   (peer_handlers.c: node_id bstr, connected, is_friend, rtt_ms). */
typedef struct {
  char node_id[48];  /* base58 of the 32-byte node id */
  uint8_t connected;
  uint8_t is_friend;
  double rtt_ms;
} offs_peer_list_entry_t;

typedef void (*offs_peer_list_cb_t)(void* ctx, uint8_t status,
    const offs_peer_list_entry_t* entries, size_t entry_count);

/* Friend list: each entry is a CBOR peer_info blob, base58-encoded. */
typedef void (*offs_friend_list_cb_t)(void* ctx, uint8_t status,
    const char* const* friend_ids_b58, size_t count);

/* Bootstrap list entry, delivered by offs_client_bootstrap_list. */
typedef struct {
  const char* host;   /* NUL-terminated, held via offs_client_release_payload */
  uint16_t port;
  int source;         /* 0 = config, 1 = managed */
} offs_bootstrap_entry_t;

typedef void (*offs_bootstrap_list_cb_t)(void* ctx, uint8_t status,
    const offs_bootstrap_entry_t* entries, size_t entry_count);

/* Config set/reload result. status: 0 = accepted, nonzero = rejected
   (daemon-defined). restart_required is 1 when the staged config needs a
   node restart to apply (config_set only; always 0 for config_reload). */
typedef void (*offs_config_set_cb_t)(void* ctx, uint8_t status,
    uint8_t restart_required, const char* message);

/* Generic JSON-string response callback (config show, update status).
   json is a NUL-terminated UTF-8 JSON document; per the payload-ownership
   rule above it stays valid until released with offs_client_release_payload
   (or until offs_client_destroy). */
typedef void (*offs_json_cb_t)(void* ctx, uint8_t status, const char* json);

/* Representation-level ephemeral/pin operations (mark permanent, delete
   ephemeral, pin, unpin). status is 0 on success; blocks_touched counts the
   representation's blocks the daemon visited. */
typedef void (*offs_rep_op_cb_t)(void* ctx, int status, size_t blocks_touched);

/* Ephemeral block list. hashes holds count 32-byte block-hash buffers;
   claims/pins hold count entries each (0 when the daemon has none). Per the
   payload-ownership rule at the top of this header, EVERY pointer handed to
   the callback stays valid until released with offs_client_release_payload:
   each hashes[index] buffer, the hashes array itself, claims, and pins —
   count + 3 calls (a count of 0 means only the three array pointers, which
   may themselves be NULL no-op releases). */
typedef void (*offs_ephemeral_list_cb_t)(void* ctx, int status, size_t count,
                                         const uint8_t* const* hashes,
                                         const uint16_t* claims,
                                         const uint32_t* pins);

/* Connection lifecycle */
offs_client_t* offs_client_connect(const char* transport_url, const char* api_key);
offs_client_t* offs_client_connect_ex(const char* transport_url, const char* api_key,
                                       const offs_client_config_t* config);
void offs_client_disconnect(offs_client_t* client);

/* Final teardown — frees everything offs_client_disconnect left behind: any
 * remaining held payloads, the client mutex, the api key copy, and the
 * client struct itself. MUST be called AFTER offs_client_disconnect (the
 * recv thread must be joined and the transport closed first); calling it
 * while the client is still connected is undefined behavior.
 * Splitting disconnect and destroy keeps the client struct (and its mutex)
 * alive after disconnect so late cross-thread callback consumers can still
 * call offs_client_release_payload safely. */
void offs_client_destroy(offs_client_t* client);

/* Releases a payload previously passed to a callback. The consumer must
 * call this for every payload pointer it received once it has copied the
 * data. Passing an unknown or already-released pointer is a no-op; a
 * payload released twice, or after the client was disconnected or
 * destroyed, is also a no-op. */
void offs_client_release_payload(offs_client_t* client, void* payload);

/* Buffered PUT — sends data in a single request */
int offs_client_put(offs_client_t* client,
                    const char* content_type,
                    const char* file_name,
                    size_t stream_length,
                    const uint8_t* data,
                    size_t data_len,
                    offs_put_response_cb_t callback,
                    void* ctx);

/* Streaming PUT — three-phase upload */
int offs_client_put_stream_start(offs_client_t* client,
                                  const char* content_type,
                                  const char* file_name,
                                  size_t stream_length);
int offs_client_put_stream_data(offs_client_t* client,
                                 const uint8_t* data,
                                 size_t len);
int offs_client_put_stream_end(offs_client_t* client,
                                offs_put_response_cb_t callback,
                                void* ctx);

/* Extended PUT with recycler/temporary support */
int offs_client_put_ex(offs_client_t* client,
                       const offs_put_options_t* options,
                       const uint8_t* data,
                       size_t data_len,
                       offs_put_response_cb_t callback,
                       void* ctx);

int offs_client_put_stream_start_ex(offs_client_t* client,
                                     const offs_put_options_t* options);

/* GET — retrieves data by ORI string */
int offs_client_get(offs_client_t* client,
                     const char* ori_string,
                     offs_get_data_cb_t data_cb,
                     offs_get_end_cb_t end_cb,
                     offs_error_cb_t error_cb,
                     void* ctx);

/* Block cache operations */
int offs_client_block_put(offs_client_t* client,
    const uint8_t* data, size_t data_len, uint8_t encoding,
    offs_block_put_cb_t callback, void* ctx);

int offs_client_block_get(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_get_cb_t callback, void* ctx);

int offs_client_block_delete(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len,
    offs_block_delete_cb_t callback, void* ctx);

/* Same delete with an explicit force flag: force=1 removes the block even
   when it is pinned or carries ephemeral claims (the optional third wire
   element); force=0 leaves those deletes rejected with
   CLIENT_API_STATUS_CONFLICT (6) in the callback. */
int offs_client_block_delete_ex(offs_client_t* client,
    const uint8_t* hash_data, size_t hash_len, uint8_t force,
    offs_block_delete_cb_t callback, void* ctx);

/* Health check */
int offs_client_health(offs_client_t* client,
    offs_health_cb_t callback, void* ctx);

/* Peer operations. format: 0 = raw CBOR peer_info, 1 = base58 text,
   2 = PPM QR image. The _qr forms are sugar for format 2.
   Error delivery: daemon-side rejections (unauthorized, undecodable peer
   info, etc.) arrive as ERROR frames and complete the per-operation callback
   with the daemon's error status (see the generic ERROR-frame completion
   note at offs_client_set_error_cb); the shared error callback (if
   registered via offs_client_set_error_cb) also fires. Success results
   arrive on the per-operation callback.
   Concurrency: one outstanding operation per callback slot — issuing
   peer_connect and then friend_add before the first result arrives
   delivers the first result to the second callback. */
int offs_client_peer_info(offs_client_t* client, offs_peer_info_cb_t callback, void* ctx);
int offs_client_peer_info_ex(offs_client_t* client, uint8_t format,
                             offs_peer_info_cb_t callback, void* ctx);
int offs_client_peer_info_qr(offs_client_t* client, offs_peer_info_cb_t callback, void* ctx);
int offs_client_peer_connect(offs_client_t* client, uint8_t format,
                             const uint8_t* data, size_t data_len,
                             offs_peer_connect_cb_t callback, void* ctx);
int offs_client_peer_connect_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                                offs_peer_connect_cb_t callback, void* ctx);
int offs_client_friend_add(offs_client_t* client, uint8_t format,
                           const uint8_t* data, size_t data_len,
                           offs_peer_connect_cb_t callback, void* ctx);
int offs_client_friend_add_qr(offs_client_t* client, const uint8_t* ppm, size_t ppm_len,
                              offs_peer_connect_cb_t callback, void* ctx);

/* List all peers known to the daemon's connection manager. Entries are
   delivered as one array snapshot; per the payload-ownership rule at the top
   of this header the array stays valid until released with
   offs_client_release_payload (one call for the array pointer itself). */
int offs_client_peer_list(offs_client_t* client, offs_peer_list_cb_t cb, void* ctx);

/* List friends. Each string is the base58 encoding of the friend's serialized
   CBOR peer_info blob (feed it to peer_info_from_base58 to decode); per the
   payload-ownership rule at the top of this header both the array of pointers
   AND each string in it stay valid until released with
   offs_client_release_payload (count + 1 calls: one per string, one for the
   array). */
int offs_client_friend_list(offs_client_t* client, offs_friend_list_cb_t cb, void* ctx);

/* Remove a friend by its base58 node id. Replies PEER_CONNECT_RESULT, so
   offs_peer_connect_cb_t is reused. */
int offs_client_friend_remove(offs_client_t* client, const char* node_id_b58,
                              offs_peer_connect_cb_t cb, void* ctx);

/* Add a bootstrap peer by "host:port" endpoint (IPv6 as "[host]:port"). The
   result frame is CLIENT_API_PEER_CONNECT_RESULT, shared with the peer/friend
   ops above, so the result routes to bootstrap_result_cb when one of these
   two ops is outstanding and to peer_connect_cb otherwise — under the
   one-outstanding-op-per-connection rule above the two slots are never
   registered simultaneously. Error delivery follows the peer operations
   above (ERROR frames complete the callback with the daemon's error
   status). */
int offs_client_bootstrap_add(offs_client_t* client, const char* endpoint,
                              offs_peer_connect_cb_t callback, void* ctx);
int offs_client_bootstrap_remove(offs_client_t* client, const char* endpoint,
                                 offs_peer_connect_cb_t callback, void* ctx);

/* List the daemon's bootstrap peers (config-seeded and operator-managed).
   Each entry is [host, port, source] from the wire; host is NUL-terminated.
   Payload ownership follows the rule at the top of this header: every
   entries[index].host string AND the entries array itself stay valid until
   released with offs_client_release_payload (count + 1 calls). Error
   delivery follows the peer operations above (its own slot). Concurrency:
   one outstanding list per slot (see peer operations above). */
int offs_client_bootstrap_list(offs_client_t* client,
                               offs_bootstrap_list_cb_t callback, void* ctx);

/* Register the shared ERROR-frame callback without sending a GET.
   Peer/friend/load daemon-side rejections arrive here.

   Generic ERROR-frame completion: when the daemon rejects an op (ERROR
   frame), the shared error callback fires AND every currently-registered
   single-response op callback is completed with the error status so its
   awaiter resolves instead of hanging until its own timeout. Ops whose
   callback carries a status parameter (peer_connect/friend_add/friend_remove,
   bootstrap_add/bootstrap_remove/bootstrap_list, block_*, peer_list,
   friend_list, config_set/config_reload, load end)
   receive the daemon's status byte; ops without one (put, health, peer_info)
   receive a NULL payload as the failure signal. get() is NOT auto-completed:
   its errors are delivered via the error callback passed to
   offs_client_set_error_cb (offs_client_get installs its own). The completed
   slots are cleared afterwards, so a late duplicate frame cannot refire
   them. */
int offs_client_set_error_cb(offs_client_t* client, offs_error_cb_t cb, void* ctx);

/* Config operations. config_show replies with the full current config as a
   JSON document; config_set stages a field update (value is always sent as a
   string — the
   daemon parses it to the field's type); config_reload applies the pending
   config by restarting the node. For config_reload, restart_required is
   always delivered as 0 (the frame carries no such field).
   Error delivery: daemon-side rejections (unauthorized, etc.) arrive as
   ERROR frames and complete the per-operation callback with the daemon's
   error status and the frame's message string (see the generic ERROR-frame
   completion note at offs_client_set_error_cb); the shared error callback
   (if registered) also fires. Success results arrive on the per-operation
   callback.
   For config_set and config_reload, field-level rejections (bad field name,
   unparseable value, etc.) are NOT ERROR frames: the daemon replies with a
   result frame whose status is nonzero, delivered on the per-operation
   callback with the daemon's message.
   config_reload restarts the node and closes the connection; after a
   successful reload the client must reconnect (offs_client_disconnect +
   offs_client_connect) before issuing further requests.
   Concurrency: config_set and config_reload SHARE the config_set callback
   slot — issuing one before the previous result arrives delivers the first
   result to the second callback. */
int offs_client_config_show(offs_client_t* client, offs_json_cb_t cb, void* ctx);
int offs_client_config_set(offs_client_t* client, const char* field, const char* value,
                           offs_config_set_cb_t cb, void* ctx);
int offs_client_config_reload(offs_client_t* client, offs_config_set_cb_t cb, void* ctx);

/* Ask the daemon for its update status as a JSON document. Error delivery
   and concurrency follow the config operations above (its own slot). */
int offs_client_update_status(offs_client_t* client, offs_json_cb_t cb, void* ctx);

/* Load a file's blocks into the daemon's block cache without receiving file
   data. has_range + range_start/range_end limit which portion of the ORI is
   preloaded; has_range = 0 loads the whole file (bounds ignored). Passed as
   flat scalars rather than a struct because the public header exposes no
   range type — consistent with the header's flat-parameter style.
   progress_cb fires once per resolved tuple; end_cb fires EXACTLY ONCE with
   the terminal status (CLIENT_API_LOAD_STATUS_LOADED/PARTIAL/FAILED,
   defined in ClientAPI/client_api_wire.h).
   Error delivery: daemon-side rejections (unauthorized, undecodable ORI,
   etc.) arrive as ERROR frames and complete end_cb with the daemon's error
   status and (0, 0) counts (see the generic ERROR-frame completion note at
   offs_client_set_error_cb); the shared error callback (if registered) also
   fires.
   Concurrency: only one load may be outstanding per connection (shared
   one-op-per-slot rule; see peer operations above). */
int offs_client_load(offs_client_t* client, const char* ori_string,
                     uint8_t has_range, size_t range_start, size_t range_end,
                     offs_load_progress_cb_t progress_cb, void* progress_ctx,
                     offs_load_end_cb_t end_cb, void* end_ctx);

/* Representation-level ephemeral/pin operations. url is the full OFF URL of
   the representation to operate on (the ori_string a put callback returned).
   mark_permanent commits every ephemeral block the representation walks;
   delete_ephemeral removes them; pin/unpin adjust the per-block pin count.
   Error delivery: daemon-side rejections (unauthorized, missing/cyclic
   descriptor) arrive as ERROR frames and complete the callback with the
   daemon's error status and blocks_touched 0 (see the generic ERROR-frame
   completion note at offs_client_set_error_cb); walk failures arrive as the
   op's own response frame with a nonzero status.
   Concurrency: the four ops SHARE one callback slot — issuing a second
   before the first result arrives delivers the first result to the second
   callback (same one-op-per-slot rule as the config operations). */
int offs_client_mark_permanent(offs_client_t* client, const char* url,
                               offs_rep_op_cb_t callback, void* ctx);
int offs_client_delete_ephemeral(offs_client_t* client, const char* url,
                                 offs_rep_op_cb_t callback, void* ctx);
int offs_client_pin_representation(offs_client_t* client, const char* url,
                                   offs_rep_op_cb_t callback, void* ctx);
int offs_client_unpin_representation(offs_client_t* client, const char* url,
                                     offs_rep_op_cb_t callback, void* ctx);

/* List every ephemeral block the daemon currently tracks, with its claim and
   pin counts. Payload ownership follows the offs_ephemeral_list_cb_t note
   above (count + 3 offs_client_release_payload calls). Error delivery and
   the one-op-per-slot rule follow the representation operations above
   (separate slot). */
int offs_client_list_ephemerals(offs_client_t* client,
                                offs_ephemeral_list_cb_t callback, void* ctx);

/* Raw HTTP GET — opens a temporary TCP connection to fetch data from a URL.
   Returns a buffer_t* with the response body, or NULL on error.
   Caller must DESTROY the returned buffer. */
buffer_t* offs_http_get(const char* url);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_CLIENT_H */
