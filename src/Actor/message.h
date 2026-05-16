//
// Created by victor on 5/6/25.
//

#ifndef OFFS_MESSAGE_H
#define OFFS_MESSAGE_H

#include <stdint.h>

/* Forward declarations for payload structs — avoids circular include with actor.h */
typedef struct buffer_t buffer_t;
typedef struct actor_t actor_t;

typedef enum message_type_e {
  SECTION_WRITE = 0,
  SECTION_READ,
  SECTION_DEALLOCATE,
  SECTION_SAVE_META,
  SECTION_CLOSE,
  SECTIONS_WRITE,
  SECTIONS_READ,
  SECTIONS_DEALLOCATE,
  SECTIONS_SECTION_FULL,
  SECTIONS_WRITE_COMPLETE,
  SECTIONS_READ_COMPLETE,
  SECTIONS_DEALLOCATE_COMPLETE,
  CACHE_GET,
  CACHE_PUT,
  CACHE_REMOVE,
  CACHE_BLOCK_LOADED,
  TIMER_SET,
  TIMER_CANCEL,
  TIMER_DEBOUNCE,
  INDEX_SAVE,
  SECTION_WRITE_META,
  READABLE_PUSH,
  READABLE_READ,
  READABLE_READ_COMPLETE,
  WRITEABLE_WRITE,
  CLOSE_STREAM,
  READABLE_PULL,
  DEFERRED_DEREF,
  TUPLE_CACHE_GET,
  TUPLE_CACHE_PUT,
  TUPLE_CACHE_REMOVE,
  TUPLE_CACHE_CONTAINS,
  TUPLE_CACHE_SIZE,
  OFF_STREAM_WRITE,
  WRITEABLE_FINALIZE,
  WRITEABLE_DESCRIPTOR_WRITE,
  BLOCK_RECIPE_PULL,
  RECIPE_BLOCK_DATA,
  RECIPE_ROTATE,
  SECTION_WRITE_COMPLETE,
  SECTION_READ_COMPLETE,
  SECTION_DEALLOCATE_COMPLETE,
  STREAM_NOTIFY,
  /* Async result messages — sent back to reply_to actor */
  SECTION_READ_RESULT,
  SECTION_WRITE_RESULT,
  SECTION_DEALLOCATE_RESULT,
  SECTIONS_READ_RESULT,
  SECTIONS_WRITE_RESULT,
  SECTIONS_DEALLOCATE_RESULT,
  CACHE_GET_RESULT,
  CACHE_PUT_RESULT,
  CACHE_REMOVE_RESULT,
  TUPLE_CACHE_GET_RESULT,
  /* HTTP connection messages */
  HTTP_CONNECTION_DATA,
  HTTP_CONNECTION_HANGUP,
  HTTP_CONNECTION_ERROR,
  HTTP_CONNECTION_WRITE,
  HTTP_CONNECTION_WRITABLE,
  HTTP_CONNECTION_CLOSE,
  /* HTTP server watcher messages */
  HTTP_SERVER_UPDATE_WATCHER,
  HTTP_SERVER_STOP_WATCHER,
  /* OFD cache resolve messages */
  OFD_CACHE_RESOLVE,
  OFD_CACHE_RESOLVE_RESULT,
  /* Stream actor messages */
  STREAM_SUBSCRIBE,
  STREAM_UNSUBSCRIBE,
  STREAM_DEACTIVATE,
  STREAM_PIPE,
  STREAM_PIPED,
  STREAM_CLOSE_HANDLER,
  STREAM_SET_PULLING,
  /* Network messages */
  NETWORK_PING,
  NETWORK_PING_RESPONSE,
  NETWORK_PING_CAPACITY,
  NETWORK_PING_CAPACITY_RESPONSE,
  NETWORK_PING_BLOCK,
  NETWORK_PING_BLOCK_RESPONSE,
  NETWORK_FIND_BLOCK,
  NETWORK_FIND_BLOCK_RESPONSE,
  NETWORK_FIND_NODE,
  NETWORK_FIND_NODE_RESPONSE,
  NETWORK_STORE_BLOCK,
  NETWORK_STORE_BLOCK_RESPONSE,
  NETWORK_SEEKING_BLOCKS,
  NETWORK_SEEKING_BLOCKS_RESPONSE,
  NETWORK_RANK_BLOCK,
  NETWORK_RECALL_BLOCK,
  NETWORK_RECALL_ACCEPT,
  NETWORK_RECALL_DECLINE,
  NETWORK_RATE_LIMITED,
  NETWORK_EABF_EXPIRE,
  NETWORK_GOSSIP_TICK,
  NETWORK_GOSSIP_EXPIRE,
  NETWORK_QUIC_DATA,
  NETWORK_QUIC_CONNECTED,
  NETWORK_QUIC_DISCONNECTED,
  QUIC_LISTENER_SEND,
  QUIC_LISTENER_OPEN_STREAM,
  QUIC_LISTENER_CLOSE_CONNECTION,
  RELAY_CLIENT_SEND,
  RELAY_CLIENT_ADDR_REQUEST,
  NETWORK_RELAY_RECEIVED,
  /* Peer connection messages */
  PEER_SEND_FIND_BLOCK,
  PEER_SEND_STORE_BLOCK,
  PEER_SEND_PING_CAPACITY,
  PEER_SEND_SEEKING_BLOCKS,
  PEER_SEND_PING_BLOCK,
  PEER_SEND_FIND_NODE,
  PEER_UPDATE_HEBBIAN,
  PEER_GET_METRICS,
  PEER_CLOSE,
  PEER_EABF_TICK,
  /* Connection manager messages */
  CM_PEER_CONNECTED,
  CM_PEER_DISCONNECTED,
  CM_LOOKUP_PEER,
  CM_LOOKUP_ALL_PEERS,
  CM_GET_PEERS_FOR_TOPIC,
  /* Topology metrics messages */
  TOPOLOGY_METRICS_UPDATE,
  /* Local stream-to-network messages (distinct from wire-level NETWORK_FIND_BLOCK etc.) */
  NETWORK_LOCAL_FIND_BLOCK,        /* Stream sends this to network actor on cache miss */
  NETWORK_FIND_BLOCK_RESULT,      /* Network actor sends this back to stream */
  NETWORK_LOCAL_STORE_BLOCK,      /* Stream sends this to network actor on CACHE_PUT_NEW */
  NETWORK_STORE_BLOCK_RESULT,     /* Network actor sends this back to stream */
} message_type_e;

/* Stream-to-network: request block from peers */
typedef struct {
  buffer_t* hash;       /* block hash to find */
  actor_t*  reply_to;   /* stream actor to notify */
} network_local_find_block_payload_t;

/* Network-to-stream: result of FindBlock */
typedef struct {
  buffer_t* hash;       /* same hash from the request */
  int       found;      /* 1 = found (block now in cache), 0 = not found */
} network_find_block_result_payload_t;

/* Stream-to-network: announce new block to peers */
typedef struct {
  buffer_t* hash;       /* block hash */
  uint32_t  fib;        /* FIB counter for the block */
  actor_t*  reply_to;   /* stream actor to notify (NULL = fire-and-forget) */
} network_local_store_block_payload_t;

/* Network-to-stream: result of StoreBlock */
typedef struct {
  int       accepted;   /* 1 = accepted by network, 0 = declined */
  uint32_t  replicas;   /* number of replicas stored */
  actor_t*  reply_to;   /* NULL for fire-and-forget */
} network_store_block_result_payload_t;

typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

#endif // OFFS_MESSAGE_H
