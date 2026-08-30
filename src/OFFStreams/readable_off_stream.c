//
// Created by victor on 5/7/26.
//

#include "readable_off_stream.h"
#include "../Util/allocator.h"
#include "../Util/error.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Scheduler/scheduler.h"
#include "../Network/network.h"

static size_t _block_size_for_type(block_size_e type);

size_t off_block_size_for_type(block_size_e type) {
  switch (type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

static size_t _block_size_for_type(block_size_e type) {
  return off_block_size_for_type(type);
}

static void _render_origin_data(readable_off_stream_t* stream, buffer_t* data) {
  size_t start;
  size_t length;
  if (!stream->offset_applied && stream->offset_remainder > 0) {
    size_t available = data->size - stream->offset_remainder;
    if (stream->sent_bytes + available > stream->ori->final_byte) {
      length = stream->ori->final_byte - stream->sent_bytes;
    } else {
      length = available;
    }
    start = stream->offset_remainder;
    stream->offset_applied = 1;
  } else {
    if (stream->sent_bytes + data->size > stream->ori->final_byte) {
      length = stream->ori->final_byte - stream->sent_bytes;
    } else {
      length = data->size;
    }
    start = 0;
  }

  buffer_t* slice = buffer_slice(data, start, start + length);
  if (slice != NULL) {
    stream_notify((stream_t*)stream, data_event, CONSUME(slice, buffer_t), (void (*)(void*))buffer_destroy);
  }

  stream->sent_bytes += length;

  if (stream->sent_bytes >= stream->ori->final_byte) {
    /* Mark closed before notifying: the close notification is queued through
     * the actor mailbox, and consumers of an earlier load_tuple_event must
     * already observe the closed flag when they run request_close. */
    stream->closed = 1;
    stream_notify((stream_t*)stream, finished_event, NULL, NULL);
    stream_notify((stream_t*)stream, complete_event, NULL, NULL);
    stream_notify((stream_t*)stream, close_event, NULL, NULL);
  }
}

static void _start_tuple_cache_lookup(readable_off_stream_t* stream, tuple_t* tuple);
static void _drain_tuple_queue(readable_off_stream_t* stream);
static void _close_stream(readable_off_stream_t* stream);

load_tuple_payload_t* load_tuple_payload_create(size_t tuples_loaded, size_t tuples_skipped) {
  load_tuple_payload_t* payload = get_clear_memory(sizeof(load_tuple_payload_t));
  payload->tuples_loaded = tuples_loaded;
  payload->tuples_skipped = tuples_skipped;
  refcounter_init((refcounter_t*)payload);
  return payload;
}

void load_tuple_payload_destroy(void* payload) {
  load_tuple_payload_t* progress = (load_tuple_payload_t*)payload;
  if (progress == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)progress)) {
    free(progress);
  }
}

/* Emit the load-mode progress event for the just-resolved tuple (loaded or
 * skipped). Only emitted when load_mode is set. */
static void _notify_load_tuple(readable_off_stream_t* stream) {
  if (!stream->load_mode) {
    return;
  }
  load_tuple_payload_t* progress =
      load_tuple_payload_create(stream->tuples_loaded, stream->tuples_skipped);
  stream_notify((stream_t*)stream, load_tuple_event,
                CONSUME(progress, load_tuple_payload_t),
                load_tuple_payload_destroy);
}

/* Tuple-completion point: tally the resolved tuple and emit the counted
 * progress event. Must be called BEFORE any render/finished/complete/close
 * notification so consumers always observe the final tuple count before the
 * stream closes. */
static void _tally_tuple_loaded(readable_off_stream_t* stream) {
  stream->tuples_loaded++;
  _notify_load_tuple(stream);
}

/* Destroy every node of a fetch list (pending or stale), releasing the hash
 * reference each node holds. */
static void _clear_fetch_list(pending_block_fetch_t* head) {
  while (head != NULL) {
    pending_block_fetch_t* next = head->next;
    DESTROY(head->hash, buffer);
    free(head);
    head = next;
  }
}

/* Release every stale fetch hash parked by skipped tuples. */
static void _clear_stale_fetches(readable_off_stream_t* stream) {
  _clear_fetch_list(stream->stale_fetches);
  stream->stale_fetches = NULL;
}

/* Unlink and destroy the pending fetch node matching `hash`, if any. Called at
 * the moment a block result is consumed for the live tuple, so an answered
 * fetch never lingers in pending_fetches. Only because of this prune does
 * _skip_pending_tuple park STRICTLY UNANSWERED hashes in stale_fetches — which
 * is what makes matching late results by hash sound (see _consume_stale_fetch).
 * A node's hash buffer is a private reference on the tuple's hash (taken by
 * _start_block_fetches), so destroying the node's reference never frees the
 * hash the tuple still owns. */
static void _prune_answered_fetch(readable_off_stream_t* stream, buffer_t* hash) {
  if (hash == NULL) {
    return;
  }
  pending_block_fetch_t** current = &stream->pending_fetches;
  while (*current != NULL) {
    pending_block_fetch_t* fetch_node = *current;
    if (buffer_compare(fetch_node->hash, hash) == 0) {
      *current = fetch_node->next;
      DESTROY(fetch_node->hash, buffer);
      free(fetch_node);
      return;
    }
    current = &fetch_node->next;
  }
}

/* If the result hash belongs to a tuple that was already skipped, consume the
 * matching stale entry and report the late result as dropped. Late results for
 * a skipped tuple must never accumulate into the live tuple's XOR accumulator.
 * Correctness of the hash match: every CACHE_GET_RESULT carries result->hash
 * (block_cache.c sets it on the LRU hit, the index miss and the section-read
 * result paths), and every NETWORK_FIND_BLOCK_RESULT construction site sets
 * result->hash (network.c wanted-list notify, store/accept relay notify and
 * the timeout sweep all build the payload from a 32-byte wanted-list hash).
 * A direct-return network result therefore matches a stale entry the same way
 * a cache result does — no generation counter is needed.
 *
 * Duplicate-hash safety: after _prune_answered_fetch, a hash can appear in
 * BOTH this list and the live tuple's request set only when two requests for
 * it were issued and NEITHER has been answered yet. Both block_cache and the
 * network deliver exactly one reply per outstanding request (one reply per
 * pending_get_t in block_cache.c; the network's wanted-list sweep guarantees a
 * found=0 NETWORK_FIND_BLOCK_RESULT for every requester), so the abandoned
 * tuple's unanswered request and the next tuple's fresh request for the same
 * hash each get their own reply: the first reply drains the stale entry
 * (dropping one duplicate of identical content — same hash means same block
 * bytes), and the reply for the live request is processed normally. Both
 * replies deliver, so the live tuple's block count still completes. */
static uint8_t _consume_stale_fetch(readable_off_stream_t* stream, buffer_t* hash) {
  if (hash == NULL) {
    return 0;
  }
  pending_block_fetch_t** current = &stream->stale_fetches;
  while (*current != NULL) {
    pending_block_fetch_t* fetch_node = *current;
    if (buffer_compare(fetch_node->hash, hash) == 0) {
      *current = fetch_node->next;
      DESTROY(fetch_node->hash, buffer);
      free(fetch_node);
      return 1;
    }
    current = &fetch_node->next;
  }
  return 0;
}

/* Load-mode skip: abandon the in-flight tuple, tally the miss, and park its
 * still-outstanding fetches in stale_fetches so their late results get dropped
 * (see _consume_stale_fetch). Fetches that were already answered were pruned
 * from pending_fetches as their results arrived (_prune_answered_fetch), so
 * only strictly unanswered hashes are parked and matching late results by hash
 * stays one-to-one. Blocks that already arrived were accumulated into the
 * abandoned xor_accumulator, which is destroyed together with the tuple. */
static void _skip_pending_tuple(readable_off_stream_t* stream) {
  stream->tuples_skipped++;
  pending_block_fetch_t* fetch_node = stream->pending_fetches;
  while (fetch_node != NULL) {
    pending_block_fetch_t* next_node = fetch_node->next;
    fetch_node->next = stream->stale_fetches;
    stream->stale_fetches = fetch_node;
    fetch_node = next_node;
  }
  stream->pending_fetches = NULL;
  if (stream->xor_accumulator != NULL) {
    DESTROY(stream->xor_accumulator, buffer);
    stream->xor_accumulator = NULL;
  }
  DESTROY(stream->pending_tuple, tuple);
  stream->pending_tuple = NULL;
  stream->blocks_expected = 0;
  stream->blocks_received = 0;
  /* Leave the state live: AWAITING_NETWORK would be a dead state once no
   * network request is outstanding. */
  stream->state = OFF_STREAM_FETCHING_BLOCKS;
  _notify_load_tuple(stream);
  _drain_tuple_queue(stream);
}

static void _finish_decode_and_render(readable_off_stream_t* stream) {
  if (stream->xor_accumulator == NULL) {
    DESTROY(stream->pending_tuple, tuple);
    stream->pending_tuple = NULL;
    return;
  }

  tuple_cache_put(stream->tc, stream->pending_tuple, stream->xor_accumulator);

  /* Tally and notify BEFORE rendering: the render path may emit the
   * finished/complete/close notifications, and consumers must always observe
   * the final tuple count before the stream closes. */
  _tally_tuple_loaded(stream);

  _render_origin_data(stream, stream->xor_accumulator);
  DESTROY(stream->xor_accumulator, buffer);
  stream->xor_accumulator = NULL;
  DESTROY(stream->pending_tuple, tuple);
  stream->pending_tuple = NULL;
  stream->blocks_expected = 0;
  stream->blocks_received = 0;

  /* Clean up any remaining pending fetches (shouldn't happen normally) */
  _clear_fetch_list(stream->pending_fetches);
  stream->pending_fetches = NULL;

  /* Queued tuples can only start once the finished tuple is fully cleared. */
  _drain_tuple_queue(stream);
}

static void _start_block_fetches(readable_off_stream_t* stream) {
  size_t count = tuple_size(stream->pending_tuple);
  stream->blocks_expected = count;
  stream->blocks_received = 0;
  stream->xor_accumulator = NULL;
  stream->pending_fetches = NULL;

  for (size_t i = 0; i < count; i++) {
    buffer_t* hash = tuple_get(stream->pending_tuple, i);
    pending_block_fetch_t* fetch = get_clear_memory(sizeof(pending_block_fetch_t));
    fetch->hash = (buffer_t*)refcounter_reference((refcounter_t*)hash);
    fetch->index = i;
    fetch->next = stream->pending_fetches;
    stream->pending_fetches = fetch;
    block_cache_get(stream->bc, hash, &stream->stream.actor);
  }
}

static void _start_tuple_cache_lookup(readable_off_stream_t* stream, tuple_t* tuple) {
  stream->pending_tuple = (tuple_t*)refcounter_reference((refcounter_t*)tuple);
  tuple_cache_get(stream->tc, tuple, &stream->stream.actor);
}

/* Process next tuple from the queue if available. Called after a tuple finishes processing. */
static void _drain_tuple_queue(readable_off_stream_t* stream) {
  if (stream->stream.is_deactivated || stream->pending_tuple != NULL) {
    return;
  }
  if (stream->tuple_queue != NULL) {
    /* Dequeue from front (FIFO) */
    pending_tuple_t* head = stream->tuple_queue;
    stream->tuple_queue = head->next;
    tuple_t* tuple = head->tuple;
    free(head);
    _start_tuple_cache_lookup(stream, tuple);
    DESTROY(tuple, tuple);
  }
}

void readable_off_stream_dispatch(void* state, message_t* msg) {
  readable_off_stream_t* stream = (readable_off_stream_t*)state;
  switch (msg->type) {
    case OFF_STREAM_WRITE: {
      tuple_t* tuple = (tuple_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      /* If we're already fetching blocks for a previous tuple, queue this one. */
      if (stream->pending_tuple != NULL) {
        pending_tuple_t* queued = get_clear_memory(sizeof(pending_tuple_t));
        queued->tuple = (tuple_t*)refcounter_reference((refcounter_t*)tuple);
        queued->next = NULL;
        /* Append to tail (FIFO) */
        if (stream->tuple_queue == NULL) {
          stream->tuple_queue = queued;
        } else {
          pending_tuple_t* tail = stream->tuple_queue;
          while (tail->next != NULL) {
            tail = tail->next;
          }
          tail->next = queued;
        }
        DESTROY(tuple, tuple);
        msg->payload = NULL;
        break;
      }
      _start_tuple_cache_lookup(stream, tuple);
      DESTROY(tuple, tuple);
      msg->payload = NULL;
      break;
    }
    case TUPLE_CACHE_GET_RESULT: {
      tuple_cache_get_result_payload_t* result = (tuple_cache_get_result_payload_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        if (result->value != NULL) {
          DESTROY(result->value, buffer);
        }
        if (result->key != NULL) {
          DESTROY(result->key, tuple);
        }
        break;
      }
      if (result->value != NULL) {
        /* Cache hit — tally, then render directly (the tally must be observed
         * before any close notification the render may emit). */
        _tally_tuple_loaded(stream);
        _render_origin_data(stream, result->value);
        DESTROY(result->value, buffer);
        DESTROY(stream->pending_tuple, tuple);
        stream->pending_tuple = NULL;
        _drain_tuple_queue(stream);
      } else {
        /* Cache miss — start fetching blocks */
        _start_block_fetches(stream);
      }
      if (result->key != NULL) {
        DESTROY(result->key, tuple);
      }
      break;
    }
    case CACHE_GET_RESULT: {
      cache_get_result_payload_t* result = (cache_get_result_payload_t*)msg->payload;
      if (stream->stream.is_deactivated) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
          result->block = NULL;
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        break;
      }

      /* Late result for an already-skipped tuple: drop it before it can
       * pollute the live tuple's accumulator. */
      if (_consume_stale_fetch(stream, result->hash)) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
          result->block = NULL;
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        break;
      }

      if (result->block == NULL) {
        /* Block not found in cache */
        if (stream->network != NULL) {
          /* Network-aware: send NETWORK_LOCAL_FIND_BLOCK for this specific hash.
           * Use result->hash directly — the network's wanted_list deduplicates. */
          stream->state = OFF_STREAM_AWAITING_NETWORK;
          network_local_find_block_payload_t* payload = get_clear_memory(sizeof(network_local_find_block_payload_t));
          payload->hash = REFERENCE(result->hash, buffer_t);
          payload->reply_to = &stream->stream.actor;
          message_t net_msg;
          net_msg.type = NETWORK_LOCAL_FIND_BLOCK;
          net_msg.payload = payload;
          net_msg.payload_destroy = network_local_find_block_payload_destroy;
          actor_send(&stream->network->actor, &net_msg);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
        } else if (stream->load_mode) {
          /* Load mode, local-only: skip the tuple and keep the stream alive.
           * Prune the triggering miss's node — a not-found reply is still an
           * answer; parking it answered would let a later tuple's only reply
           * for this hash be consumed as a late result (silent wedge). */
          _prune_answered_fetch(stream, result->hash);
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
          _skip_pending_tuple(stream);
        } else {
          /* Local-only: deactivate as before */
          if (stream->xor_accumulator != NULL) {
            DESTROY(stream->xor_accumulator, buffer);
            stream->xor_accumulator = NULL;
          }
          if (result->hash != NULL) {
            DESTROY(result->hash, buffer);
            result->hash = NULL;
          }
          DESTROY(stream->pending_tuple, tuple);
          stream->pending_tuple = NULL;
          stream_deactivate((stream_t*)stream, OFFS_ERROR("Block not found in cache"));
        }
        break;
      }

      /* Accumulate block into XOR result. First prune the matching fetch from
       * pending_fetches: this result has been answered, and it must not be
       * parked in stale_fetches if the tuple is later skipped. */
      _prune_answered_fetch(stream, result->hash);
      if (stream->xor_accumulator == NULL) {
        stream->xor_accumulator = buffer_copy(result->block->data);
      } else {
        buffer_t* xored = buffer_xor(stream->xor_accumulator, result->block->data);
        DESTROY(stream->xor_accumulator, buffer);
        stream->xor_accumulator = xored;
      }

      DESTROY(result->block, block);
      result->block = NULL;
      if (result->hash != NULL) {
        DESTROY(result->hash, buffer);
        result->hash = NULL;
      }

      stream->blocks_received++;

      if (stream->blocks_received >= stream->blocks_expected) {
        _finish_decode_and_render(stream);
      }
      break;
    }
    case NETWORK_FIND_BLOCK_RESULT: {
      network_find_block_result_payload_t* result = (network_find_block_result_payload_t*)msg->payload;
      /* Late result for an already-skipped tuple: drop it (direct-return
       * blocks included) — it belongs to no live tuple. */
      if (_consume_stale_fetch(stream, result->hash)) {
        if (result->block != NULL) {
          DESTROY(result->block, block);
          result->block = NULL;
        }
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        break;
      }
      if (result->found) {
        if (result->block != NULL) {
          /* Direct-return: network provided the block. XOR-accumulate it
           * directly instead of re-fetching from the cache. Prune the matching
           * fetch from pending_fetches first — answered fetches must never be
           * parked in stale_fetches by a later skip. */
          _prune_answered_fetch(stream, result->hash);
          if (stream->xor_accumulator == NULL) {
            stream->xor_accumulator = buffer_copy(result->block->data);
          } else {
            buffer_t* xored = buffer_xor(stream->xor_accumulator, result->block->data);
            DESTROY(stream->xor_accumulator, buffer);
            stream->xor_accumulator = xored;
          }
          stream->blocks_received++;
          if (stream->blocks_received >= stream->blocks_expected) {
            _finish_decode_and_render(stream);
          }
        } else {
          /* Local path: block is in the cache. Re-fetch as before. */
          buffer_t* fetch_hash = result->hash;
          if (fetch_hash != NULL) {
            block_cache_get(stream->bc, fetch_hash, &stream->stream.actor);
          }
          stream->state = OFF_STREAM_FETCHING_BLOCKS;
        }
      } else if (stream->load_mode) {
        /* Load mode: block never found on the network — skip the tuple and
         * keep the stream alive. Prune the triggering miss's node — a
         * not-found reply is still an answer; parking it answered would let
         * a later tuple's only reply for this hash be consumed as a late
         * result (silent wedge). */
        _prune_answered_fetch(stream, result->hash);
        if (result->hash != NULL) {
          DESTROY(result->hash, buffer);
          result->hash = NULL;
        }
        _skip_pending_tuple(stream);
      } else {
        /* Block not found on network — deactivate */
        if (stream->xor_accumulator != NULL) {
          DESTROY(stream->xor_accumulator, buffer);
          stream->xor_accumulator = NULL;
        }
        DESTROY(stream->pending_tuple, tuple);
        stream->pending_tuple = NULL;
        stream_deactivate((stream_t*)stream, OFFS_ERROR("Block not found on network"));
      }
      break;
    }
    case CLOSE_STREAM: {
      if (!stream->stream.is_deactivated) {
        _close_stream(stream);
      }
      break;
    }
    default:
      stream_dispatch(state, msg);
      break;
  }
}

/* Run exactly the CLOSE_STREAM cleanup and notify close_event without an
 * error. Used by the CLOSE_STREAM message and by request_close so the two
 * entry points cannot drift. */
static void _close_stream(readable_off_stream_t* stream) {
  if (stream->pending_tuple != NULL) {
    DESTROY(stream->pending_tuple, tuple);
    stream->pending_tuple = NULL;
  }
  if (stream->xor_accumulator != NULL) {
    DESTROY(stream->xor_accumulator, buffer);
    stream->xor_accumulator = NULL;
  }
  pending_tuple_t* queued = stream->tuple_queue;
  while (queued != NULL) {
    pending_tuple_t* next = queued->next;
    DESTROY(queued->tuple, tuple);
    free(queued);
    queued = next;
  }
  stream->tuple_queue = NULL;
  pending_block_fetch_t* fetch = stream->pending_fetches;
  while (fetch != NULL) {
    pending_block_fetch_t* next = fetch->next;
    DESTROY(fetch->hash, buffer);
    free(fetch);
    fetch = next;
  }
  stream->pending_fetches = NULL;
  _clear_stale_fetches(stream);
  stream->closed = 1;
  stream_notify((stream_t*)stream, close_event, NULL, NULL);
  stream->stream.is_deactivated = 1;
}

void readable_off_stream_request_close(readable_off_stream_t* stream) {
  if (stream == NULL) {
    return;
  }
  if (stream->stream.is_deactivated || stream->closed) {
    return;
  }
  _close_stream(stream);
}

static void _readable_off_stream_on_write(stream_t* stream, void* data) {
  (void)stream;
  (void)data;
}

readable_off_stream_t* readable_off_stream_create_ex(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network, uint8_t load_mode) {
  readable_off_stream_t* stream = get_clear_memory(sizeof(readable_off_stream_t));
  stream->bc = bc;
  stream->tc = tc;
  stream->ori = ori;
  stream->network = network;
  stream->descriptor_pad = descriptor_pad;
  stream->offset_applied = 0;
  stream->load_mode = load_mode;
  stream->state = OFF_STREAM_FETCHING_BLOCKS;

  size_t block_size = _block_size_for_type(ori->block_type);
  stream->sent_bytes = ori->file_offset;
  stream->offset_remainder = ori->file_offset % block_size;

  stream_init((stream_t*)stream, push, transform_stream, 0, pool,
              (void (*)(stream_t*))readable_off_stream_destroy);
  stream->stream.on_write = _readable_off_stream_on_write;

  stream->stream.actor.state = stream;
  stream->stream.actor.dispatch = readable_off_stream_dispatch;

  return stream;
}

readable_off_stream_t* readable_off_stream_create(
    scheduler_pool_t* pool, block_cache_t* bc, tuple_cache_t* tc,
    ori_t* ori, size_t descriptor_pad, network_t* network) {
  return readable_off_stream_create_ex(pool, bc, tc, ori, descriptor_pad,
                                       network, 0);
}

void readable_off_stream_destroy(readable_off_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*)stream)) {
    pending_tuple_t* queued = stream->tuple_queue;
    while (queued != NULL) {
      pending_tuple_t* next = queued->next;
      DESTROY(queued->tuple, tuple);
      free(queued);
      queued = next;
    }
    if (stream->pending_tuple != NULL) {
      DESTROY(stream->pending_tuple, tuple);
    }
    if (stream->xor_accumulator != NULL) {
      DESTROY(stream->xor_accumulator, buffer);
      stream->xor_accumulator = NULL;
    }
    pending_block_fetch_t* fetch = stream->pending_fetches;
    while (fetch != NULL) {
      pending_block_fetch_t* next = fetch->next;
      DESTROY(fetch->hash, buffer);
      free(fetch);
      fetch = next;
    }
    stream->pending_fetches = NULL;
    _clear_stale_fetches(stream);
    stream_deinit((stream_t*)stream);
    free(stream);
  }
}

void readable_off_stream_write(readable_off_stream_t* stream, tuple_t* tuple) {
  tuple_t* ref = REFERENCE(tuple, tuple_t);
  message_t msg;
  msg.type = OFF_STREAM_WRITE;
  msg.payload = ref;
  msg.payload_destroy = (void (*)(void*))tuple_destroy;

  actor_send(&stream->stream.actor, &msg);
}