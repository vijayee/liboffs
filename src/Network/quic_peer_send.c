//
// Created by victor on 5/16/25.
//

#include "quic_peer_send.h"
#include "stream_framer.h"
#include "connection_manager.h"
#include "peer_connection.h"
#include "msquic_singleton.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform.h"
#include <cbor.h>
#include <string.h>

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

/* SEND_COMPLETE callback context — frees the frame buffer and QUIC_BUFFER
 * after msquic completes. The QUIC_BUFFER must be heap-allocated because
 * msquic stores a pointer to it and reads from it asynchronously in the
 * worker thread. */
typedef struct {
  uint8_t* frame;
  QUIC_BUFFER buf;
} send_complete_context_t;

#ifdef HAS_MSQUIC

static QUIC_STATUS QUIC_API PLATFORM_UNUSED _quic_send_complete_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
  (void)stream;
  if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
    send_complete_context_t* send_ctx = (send_complete_context_t*)context;
    if (send_ctx != NULL) {
      free(send_ctx->frame);
      free(send_ctx);
    }
  }
  return QUIC_STATUS_SUCCESS;
}

#endif /* HAS_MSQUIC */

int quic_peer_send(network_t* network, peer_connection_t* peer, cbor_item_t* cbor_msg) {
  if (network == NULL || peer == NULL || cbor_msg == NULL) return -1;

#ifdef HAS_MSQUIC
  if (peer->quic_stream == NULL) {
    log_error("quic_peer_send: peer has no QUIC stream");
    return -1;
  }
  /* Serialize CBOR to bytes */
  unsigned char* cbor_data = NULL;
  size_t cbor_len = 0;
  size_t serialized = cbor_serialize_alloc(cbor_msg, &cbor_data, &cbor_len);
  if (cbor_data == NULL || serialized == 0) {
    log_error("quic_peer_send: CBOR serialization failed");
    return -1;
  }

  /* Frame with 4-byte length prefix */
  size_t frame_len = 0;
  uint8_t* frame = stream_frame_encode(cbor_data, cbor_len, &frame_len);
  free(cbor_data);

  if (frame == NULL) {
    log_error("quic_peer_send: stream_frame_encode failed");
    return -1;
  }

  /* Create the SEND_COMPLETE context to free the frame buffer later.
   * The QUIC_BUFFER is included in the context struct so it remains
   * heap-allocated until SEND_COMPLETE fires — msquic stores a pointer
   * to the QUIC_BUFFER and reads from it asynchronously. */
  send_complete_context_t* send_ctx = get_clear_memory(sizeof(send_complete_context_t));
  if (send_ctx == NULL) {
    free(frame);
    log_error("quic_peer_send: failed to allocate send context");
    return -1;
  }
  send_ctx->frame = frame;
  send_ctx->buf.Buffer = frame;
  send_ctx->buf.Length = (uint32_t)frame_len;

  const struct QUIC_API_TABLE* msquic = offs_msquic_open();
  if (msquic == NULL) {
    free(frame);
    free(send_ctx);
    log_error("quic_peer_send: msquic API table not available");
    return -1;
  }

  QUIC_STATUS status = msquic->StreamSend(
      (HQUIC)peer->quic_stream,
      &send_ctx->buf,
      1,
      QUIC_SEND_FLAG_NONE,
      send_ctx);

  /* Release our msquic reference — the API table stays valid during the send */
  offs_msquic_close();

  if (QUIC_FAILED(status)) {
    free(frame);
    free(send_ctx);
    log_error("quic_peer_send: StreamSend failed: 0x%x", status);
    return -1;
  }

  return 0;

#else /* !HAS_MSQUIC */
  (void)network;
  log_error("quic_peer_send: no QUIC support (HAS_MSQUIC not defined)");
  return -1;
#endif
}

int quic_peer_send_path(network_t* network, node_id_t* path, uint8_t path_len, cbor_item_t* cbor_msg) {
  if (network == NULL || path == NULL || path_len == 0 || cbor_msg == NULL) return -1;

  /* The next hop is path[0] — the first node in the path */
  peer_connection_t* peer = connection_manager_lookup(&network->conn_mgr, &path[0]);
  if (peer == NULL) {
    log_error("quic_peer_send_path: no peer connection for next hop");
    return -1;
  }

  return quic_peer_send(network, peer, cbor_msg);
}

void quic_peer_broadcast(network_t* network, cbor_item_t* cbor_msg) {
  if (network == NULL || cbor_msg == NULL) return;

  for (size_t index = 0; index < network->conn_mgr.peer_count; index++) {
    peer_connection_t* peer = network->conn_mgr.peers[index];
    if (peer == NULL || !peer->connected) continue;

#ifdef HAS_MSQUIC
    if (peer->quic_stream == NULL) continue;
#endif

    /* Each call to quic_peer_send serializes the CBOR independently,
     * so we can pass the same cbor_msg pointer. The caller will
     * call cbor_decref once after broadcast completes. */
    quic_peer_send(network, peer, cbor_msg);
  }
}