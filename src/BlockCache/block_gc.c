//
// Created by victor on 9/28/26.
//

#include "block_gc.h"
#include "../Buffer/buffer.h"
#include "../OFFStreams/off_url.h"
#include "../OFFStreams/representation_actor.h"
#include "../RefCounter/refcounter.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <stdlib.h>
#include <string.h>

/* Phases (uint8_t fields; the names are .c-local). */
#define BLOCK_GC_PHASE_COLLECT 0
#define BLOCK_GC_PHASE_SWEEP   1
#define BLOCK_GC_PHASE_DEFRAG  2

/* Keep-filter budget: blocks spared per URL, on average. The filter elastically
   expands on saturation (elastic_bloom_filter_add), so a tight budget cannot
   lose blocks — it only costs extra expansion passes. */
#define BLOCK_GC_EXPECTED_PER_URL 16
#define BLOCK_GC_EBF_HASH_COUNT 4

/* Deferred destruction for the rep actor each collect line opened: the
   completion runs on a pool worker, so it can NEVER call
   representation_actor_destroy inline (that destroy parks the pool and would
   self-deadlock); this wrapper lands in the pool's cleanup drain, which only
   runs when every worker is already idle. The leading refcounter_t satisfies
   defer_cleanup's hold-a-reference contract — handing it a struct whose first
   member is a live actor mailbox would corrupt the mailbox head. Mirrors
   rep_api_defer_t (representation_api.c). */
typedef struct {
  refcounter_t refcounter;
  representation_actor_t* rep;
} block_gc_rep_defer_t;

static void block_gc_rep_deferred_destroy(block_gc_rep_defer_t* defer) {
  representation_actor_t* rep = defer->rep;
  refcounter_destroy_lock(&defer->refcounter);
  free(defer);
  representation_actor_destroy(rep);
}

/* Deferred self-destruction after the report goes out: the REPORT step runs
   on the gc's own actor (a pool worker), so block_gc_destroy must wait for
   the drain. */
typedef struct {
  refcounter_t refcounter;
  block_gc_t* gc;
} block_gc_defer_t;

static void block_gc_deferred_destroy(block_gc_defer_t* defer) {
  block_gc_t* gc = defer->gc;
  refcounter_destroy_lock(&defer->refcounter);
  free(defer);
  block_gc_destroy(gc);
}

typedef void (*block_gc_defer_destroy_fn)(void*);

void block_gc_result_destroy(void* ptr) {
  block_gc_result_t* result = (block_gc_result_t*)ptr;
  if (result == NULL) {
    return;
  }
  if (result->failed_lines != NULL) {
    for (size_t idx = 0; idx < result->failed_count; idx++) {
      if (result->failed_lines[idx] != NULL) {
        free(result->failed_lines[idx]);
      }
    }
    free(result->failed_lines);
  }
  free(result->failed_line);
  free(result->failed_reason);
  free(result);
}

/* ---- line splitting ---- */

/* Normalize one raw line: drop a trailing \r (CRLF tolerance), then spaces or
   tab padding on either end. Returns the trimmed length; start advances. */
static size_t block_gc_trim_line(const char** start, size_t length) {
  const char* cursor = *start;
  if (length > 0 && cursor[length - 1] == '\r') {
    length--;
  }
  while (length > 0 && (cursor[0] == ' ' || cursor[0] == '\t')) {
    cursor++;
    length--;
  }
  while (length > 0 && (cursor[length - 1] == ' ' || cursor[length - 1] == '\t')) {
    length--;
  }
  *start = cursor;
  return length;
}

static size_t block_gc_count_lines(const char* text) {
  size_t count = 0;
  if (text == NULL) {
    return 0;
  }
  const char* cursor = text;
  while (*cursor != '\0') {
    const char* start = cursor;
    while (*cursor != '\0' && *cursor != '\n') {
      cursor++;
    }
    size_t length = (size_t)(cursor - start);
    if (*cursor == '\n') {
      cursor++;
    }
    length = block_gc_trim_line(&start, length);
    if (length == 0) {
      continue;
    }
    count++;
  }
  return count;
}

static void block_gc_fill_lines(const char* text, char** lines, size_t line_count) {
  if (text == NULL || line_count == 0) {
    return;
  }
  size_t out_index = 0;
  const char* cursor = text;
  while (out_index < line_count && *cursor != '\0') {
    const char* start = cursor;
    while (*cursor != '\0' && *cursor != '\n') {
      cursor++;
    }
    size_t length = (size_t)(cursor - start);
    if (*cursor == '\n') {
      cursor++;
    }
    length = block_gc_trim_line(&start, length);
    if (length == 0) {
      continue;
    }
    char* copy = get_clear_memory(length + 1);
    memcpy(copy, start, length);
    lines[out_index++] = copy;
  }
}

/* ---- orchestration ---- */

/* Record one failed line: 1-based line number, GC_LINE_* reason, and a copy
   of the original text. The arrays are sized line_count at create, so every
   line can fail without reallocation. */
static void block_gc_push_failure(block_gc_t* gc, size_t line_index, uint8_t reason) {
  size_t slot = gc->failed_count;
  gc->failed_count++;
  gc->failed_line[slot] = line_index + 1;
  gc->failed_reason[slot] = reason;
  size_t length = strlen(gc->lines[line_index]);
  char* copy = get_clear_memory(length + 1);
  memcpy(copy, gc->lines[line_index], length);
  gc->failed_lines[slot] = copy;
  log_warn("block_gc: line %u failed (reason=%u): %s", (unsigned)(line_index + 1),
           (unsigned)reason, gc->lines[line_index]);
}

/* Merge one COLLECT result into the keep filter. Every transferred hash is
   destroyed per the mirror contract (count referenced buffers + the array);
   `source` is never destroyed by this helper — the caller defers it. Bloom
   false positives on contains merely skip an add (safe direction); add's
   elastic expansion handles filter saturation by itself. */
static void block_gc_merge_collect_result(block_gc_t* gc,
                                          representation_collect_result_payload_t* result) {
  for (size_t idx = 0; idx < result->count; idx++) {
    buffer_t* hash = result->hashes[idx];
    if (hash == NULL) {
      continue;
    }
    if (gc->keep == NULL ||
        !elastic_bloom_filter_contains(gc->keep, hash->data, hash->size)) {
      if (gc->keep != NULL) {
        elastic_bloom_filter_add(gc->keep, hash->data, hash->size);
      }
    }
    DESTROY(hash, buffer);
  }
  free(result->hashes);
  result->hashes = NULL;
}

/* Collection step: issue the next resolvable line's walk (one outstanding at
   a time) or, when the input is spent, apply the safety guards and move to
   the sweep. Called inline after each walk completes — the step itself never
   blocks. */
static void block_gc_collect_step(block_gc_t* gc) {
  if (gc->phase != BLOCK_GC_PHASE_COLLECT) {
    return;
  }
  while (gc->collecting == 0 && gc->next_line < gc->line_count) {
    size_t line_index = gc->next_line;
    gc->next_line++;
    const char* line = gc->lines[line_index];
    off_url_t* url = off_url_parse(line);
    if (url == NULL || url->descriptor_hash == NULL) {
      if (url != NULL) {
        off_url_destroy(url);
      }
      block_gc_push_failure(gc, line_index, GC_LINE_MALFORMED_URL);
      continue;
    }
    /* representation_actor_create references the descriptor hash itself: keep
       mine alive across the call, then drop my copy after the walk is kicked
       (the rep actor holds its own). */
    buffer_t* held = (buffer_t*)refcounter_reference(
        (refcounter_t*)url->descriptor_hash);
    off_url_destroy(url);
    gc->collecting = 1;
    gc->current_line = line_index;
    /* The create's return value is not stored — the completion reply carries
       the actor pointer (payload->source), which is the only safe way to
       reach it (the walk can finish before create returns). */
    representation_actor_create(gc->bc, gc->network, held,
                                REPRESENTATION_OP_COLLECT, &gc->actor);
    DESTROY(held, buffer);
    return;
  }
  if (gc->next_line >= gc->line_count) {
    /* Every line attempted. Safety guards: no usable filter (create failed)
       or nothing resolveable — refuse rather than sweep an empty keep filter,
       which would delete the entire cache. */
    if (gc->keep == NULL) {
      log_error("block_gc: no keep set could be built — refusing the sweep");
      gc->status = 1;
      block_gc_report(gc);
      return;
    }
    if (gc->urls_collected == 0 && gc->failed_count == gc->line_count) {
      log_error("block_gc: every keep-list line failed to resolve — refusing the sweep");
      gc->status = 1;
      block_gc_report(gc);
      return;
    }
    gc->phase = BLOCK_GC_PHASE_SWEEP;
    elastic_bloom_filter_t* keep = gc->keep;
    gc->keep = NULL;  /* the cache dispatch owns + destroys it */
    block_cache_gc(gc->bc, keep, gc->force, &gc->actor);
  }
}

/* REPORT step — moves the outcome out (the consumer owns the struct and its
   rows) and then defers the orchestrator's own teardown. Runs on the gc's own
   actor, so it must never destroy that actor inline. */
static void block_gc_report(block_gc_t* gc) {
  if (gc->reported) {
    return;
  }
  gc->reported = 1;
  if (gc->reply_to != NULL) {
    block_gc_result_t* result = get_clear_memory(sizeof(block_gc_result_t));
    result->status = gc->status;
    result->urls_request = gc->line_count;
    result->urls_collected = gc->urls_collected;
    result->blocks_deleted = gc->blocks_deleted;
    result->blocks_kept = gc->blocks_kept;
    result->skipped_pinned = gc->skipped_pinned;
    result->skipped_ephemeral_claimed = gc->skipped_ephemeral_claimed;
    result->defrag_applied = gc->defrag_applied;
    result->defrag_result = gc->defrag_result;
    result->defrag_sections = gc->defrag_sections;
    result->defrag_blocks_relocated = gc->defrag_blocks_relocated;
    /* The rows transfer: the consumer owns the line-text copies plus the
       number and reason arrays. */
    result->failed_count = gc->failed_count;
    result->failed_line = gc->failed_line;
    result->failed_reason = gc->failed_reason;
    result->failed_lines = gc->failed_lines;
    gc->failed_line = NULL;
    gc->failed_reason = NULL;
    gc->failed_lines = NULL;
    gc->failed_count = 0;

    message_t reply;
    reply.type = BLOCK_GC_RESULT;
    reply.payload = result;
    reply.payload_destroy = block_gc_result_destroy;
    actor_send(gc->reply_to, &reply);
  }
  block_gc_defer_t* defer = get_clear_memory(sizeof(block_gc_defer_t));
  refcounter_init(&defer->refcounter);
  defer->gc = gc;
  scheduler_pool_defer_cleanup(gc->pool, defer,
                               (block_gc_defer_destroy_fn)block_gc_deferred_destroy);
}

void block_gc_dispatch(void* state, message_t* msg) {
  block_gc_t* gc = (block_gc_t*)state;
  if (gc == NULL || msg == NULL) {
    return;
  }
  switch (msg->type) {
    case BLOCK_GC_START: {
      /* Kick from create: begin (or continue) the collect phase. */
      block_gc_collect_step(gc);
      break;
    }
    case REPRESENTATION_COLLECT_RESULT: {
      gc->collecting = 0;
      representation_collect_result_payload_t* result =
          (representation_collect_result_payload_t*)msg->payload;
      if (result != NULL) {
        block_gc_merge_collect_result(gc, result);
        representation_actor_t* source = result->source;
        size_t line_index = gc->current_line;
        int walk_result = result->result;
        msg->payload = NULL;  /* consumed here, not by actor_run's destroy */
        free(result);

        /* A failed walk records its line (reason per the failure shape); the
           cycle case (-3) still contributed its partial set above. */
        if (walk_result == 0 || walk_result == -3) {
          gc->urls_collected++;
          if (walk_result == -3) {
            block_gc_push_failure(gc, line_index, GC_LINE_CYCLE);
          }
        } else if (walk_result == -1) {
          block_gc_push_failure(gc, line_index, GC_LINE_MISSING_DESCRIPTOR);
        } else if (walk_result == -2) {
          block_gc_push_failure(gc, line_index, GC_LINE_MALFORMED_DESCRIPTOR);
        } else {
          log_warn("block_gc: unexpected walk result %d on line %u", walk_result,
                   (unsigned)(line_index + 1));
          block_gc_push_failure(gc, line_index, GC_LINE_MALFORMED_DESCRIPTOR);
        }
        if (source != NULL) {
          block_gc_rep_defer_t* rep_defer =
              get_clear_memory(sizeof(block_gc_rep_defer_t));
          refcounter_init(&rep_defer->refcounter);
          rep_defer->rep = source;
          scheduler_pool_defer_cleanup(gc->pool, rep_defer,
                                       (block_gc_defer_destroy_fn)block_gc_rep_deferred_destroy);
        }
      }
      block_gc_collect_step(gc);
      break;
    }
    case CACHE_GC_RESULT: {
      cache_gc_result_payload_t* result = (cache_gc_result_payload_t*)msg->payload;
      if (result == NULL) {
        gc->status = 1;
        block_gc_report(gc);
        break;
      }
      gc->blocks_deleted = result->blocks_deleted;
      gc->blocks_kept = result->blocks_kept;
      gc->skipped_pinned = result->skipped_pinned;
      gc->skipped_ephemeral_claimed = result->skipped_ephemeral_claimed;
      msg->payload = NULL;  /* consumed here, not by actor_run's destroy */
      free(result);
      if (gc->defrag) {
        gc->defrag_applied = 1;
        gc->phase = BLOCK_GC_PHASE_DEFRAG;
        block_cache_defragment(gc->bc, 0.5f, &gc->actor);
      } else {
        block_gc_report(gc);
      }
      break;
    }
    case CACHE_DEFRAGMENT_RESULT: {
      cache_defragment_result_payload_t* result =
          (cache_defragment_result_payload_t*)msg->payload;
      gc->defrag_result = (result != NULL) ? result->result : -1;
      gc->defrag_sections = (result != NULL) ? result->sections_defragmented : 0;
      gc->defrag_blocks_relocated = (result != NULL) ? result->blocks_relocated : 0;
      msg->payload = NULL;  /* consumed here, not by actor_run's destroy */
      free(result);
      block_gc_report(gc);
      break;
    }
    default:
      break;
  }
}

block_gc_t* block_gc_create(block_cache_t* bc, network_t* network, scheduler_pool_t* pool,
                            const char* text, uint8_t force, uint8_t defrag,
                            actor_t* reply_to) {
  block_gc_t* gc = get_clear_memory(sizeof(block_gc_t));
  gc->bc = bc;
  gc->network = network;
  gc->pool = pool;
  gc->force = force;
  gc->defrag = defrag;
  gc->phase = BLOCK_GC_PHASE_COLLECT;
  gc->reply_to = reply_to;
  gc->line_count = block_gc_count_lines(text);
  if (gc->line_count > 0) {
    gc->lines = get_clear_memory(sizeof(char*) * gc->line_count);
    block_gc_fill_lines(text, gc->lines, gc->line_count);
    /* Failure rows overallocate to line_count once, here. */
    gc->failed_line = get_clear_memory(sizeof(size_t) * gc->line_count);
    gc->failed_reason = get_clear_memory(sizeof(uint8_t) * gc->line_count);
    gc->failed_lines = get_clear_memory(sizeof(char*) * gc->line_count);
    size_t expected = gc->line_count * BLOCK_GC_EXPECTED_PER_URL;
    size_t ebf_size = expected * 16;
    if (ebf_size < 1024) {
      ebf_size = 1024;
    }
    gc->keep = elastic_bloom_filter_create(ebf_size, BLOCK_GC_EBF_HASH_COUNT, 1.0f,
                                           EBF_DEFAULT_FP_BITS);
    if (gc->keep == NULL) {
      /* Reported (and refused) at collect end, not torn down here: the reply
         carries the failed rows and the consumer learns the run failed. */
      log_error("block_gc_create: keep filter creation failed");
    }
  }
  actor_init(&gc->actor, gc, block_gc_dispatch, pool);
  message_t start;
  start.type = BLOCK_GC_START;
  start.payload = NULL;
  start.payload_destroy = NULL;
  actor_send(&gc->actor, &start);
  return gc;
}

void block_gc_destroy(block_gc_t* gc) {
  if (gc == NULL) {
    return;
  }
  /* EXTERNAL teardown only (the drain wrapper, or tests): park the pool so
     the actor's last run has fully finished before its memory goes away. */
  scheduler_pool_wait_for_idle(gc->pool);
  actor_destroy(&gc->actor);
  if (gc->lines != NULL) {
    for (size_t idx = 0; idx < gc->line_count; idx++) {
      if (gc->lines[idx] != NULL) {
        free(gc->lines[idx]);
      }
    }
    free(gc->lines);
  }
  if (gc->keep != NULL) {
    /* Unhanded filter (run refused before the sweep): destroy it here. */
    elastic_bloom_filter_destroy(gc->keep);
    gc->keep = NULL;
  }
  if (gc->failed_lines != NULL) {
    for (size_t idx = 0; idx < gc->failed_count; idx++) {
      if (gc->failed_lines[idx] != NULL) {
        free(gc->failed_lines[idx]);
      }
    }
    free(gc->failed_lines);
  }
  free(gc->failed_line);
  free(gc->failed_reason);
  free(gc);
}