//
// Created by victor on 9/28/26.
//

#ifndef OFFS_BLOCK_GC_H
#define OFFS_BLOCK_GC_H

#include "../Actor/actor.h"
#include "../BlockCache/block_cache.h"
#include <stddef.h>
#include <stdint.h>

typedef struct network_t network_t;

typedef struct representation_actor_t representation_actor_t;

/* Failed-line reasons — the numbers are shared with the wire encoding
   (CLIENT_API_GC_* line reasons in client_api_wire.h), so keep them in sync. */
#define GC_LINE_MALFORMED_URL        1  /* the line did not parse as OFF URL/ORI */
#define GC_LINE_MISSING_DESCRIPTOR   2  /* the walk found the descriptor block absent */
#define GC_LINE_MALFORMED_DESCRIPTOR 3  /* the descriptor block could not be walked */
#define GC_LINE_CYCLE                4  /* the descriptor chain looped (partial set kept) */

/* BLOCK_GC_RESULT payload — the keep-list sweep's outcome, TRANSFERRED to the
   consumer: the consumer owns the struct (rows included) and frees it via
   block_gc_result_destroy. status == 0 means the sweep ran; status == 1 is an
   internal error, which includes the empty-keep refusal: when EVERY line
   failed to resolve there is no keep set, and sweeping an empty filter would
   delete the entire cache — the orchestrator refuses and reports with
   blocks_deleted == 0 and the failed lines intact. */
typedef struct {
  int status;             /* 0 = sweep completed, 1 = internal error / refusal */
  size_t urls_request;    /* non-empty lines the orchestrator received */
  size_t urls_collected;  /* lines that resolved (cycle = partial count) */
  size_t blocks_deleted, blocks_kept, skipped_pinned;
  size_t skipped_ephemeral_claimed;   /* NO_FORCE skips (sweep summary) */
  size_t failed_count;                /* rows below are failed_count long */
  size_t* failed_line;                /* 1-based source line numbers */
  uint8_t* failed_reason;             /* GC_LINE_* */
  char** failed_lines;                /* the original line text */
  uint8_t defrag_applied;             /* 1 = the optional defragment pass ran */
  int defrag_result;
  size_t defrag_sections, defrag_blocks_relocated;
} block_gc_result_t;

void block_gc_result_destroy(void* ptr);

/* Exact keep-filter sizing, computed from the keep list BEFORE any
   descriptor walk: an OFF URL carries its stream extent (off_url_parse),
   and the encoding is deterministic after that — one data block per
   block-size slice of the stream, one 32-byte descriptor pad per data
   block, one next-descriptor pad per non-last descriptor block, and
   block_size / 32 - 1 of those pads per descriptor block. Parseless
   lines contribute 0 (collect reports them GC_LINE_MALFORMED_URL and
   they keep nothing either way). Overcounts are SAFE: they only buy
   filter room, and the sweep's elastic path stays as the
   belt-and-braces for anything the URL arithmetic cannot see. */
size_t block_gc_expected_blocks(block_cache_t* bc, const char* const* lines,
                                size_t line_count);

typedef struct block_gc_t block_gc_t;

struct block_gc_t {
  actor_t actor;          /* FIRST member — the deferred-teardown ritual */
  block_cache_t* bc;
  network_t* network;     /* NULL = local-only; the collect walks announce nothing */
  scheduler_pool_t* pool;
  char** lines;           /* line_count trimmed non-empty copies of `text` */
  size_t line_count, next_line;
  uint8_t force;          /* per the sweep: 1 deletes pinned/claimed blocks too */
  uint8_t defrag;         /* 1 chains a block_cache_defragment pass after the sweep */
  float defrag_threshold; /* occupancy threshold passed to that defrag pass */
  uint8_t phase;          /* COLLECT(0) / SWEEP(1) / DEFRAG(2) */
  uint8_t collecting;     /* 1 while one representation walk is outstanding */
  size_t current_line;    /* index of the line the outstanding walk belongs to */
  elastic_bloom_filter_t* keep;  /* NULL once ownership is handed to the cache */
  size_t* failed_line; uint8_t* failed_reason; char** failed_lines;
  size_t failed_count, urls_collected;
  int status;             /* 0 = sweep completed, 1 = internal error / refusal */
  size_t blocks_deleted, blocks_kept, skipped_pinned, skipped_ephemeral_claimed;
  uint8_t defrag_applied;
  int defrag_result;
  size_t defrag_sections, defrag_blocks_relocated;
  actor_t* reply_to;      /* BLOCK_GC_RESULT consumer */
  uint8_t reported;
};

/* Kick off a keep-list GC (async; replies BLOCK_GC_RESULT with a transferred
   block_gc_result_t). `text` is newline-delimited URLs/ORIs; lines are split,
   trimmed, and emptied dropped before the walk starts. Never destroy the
   returned actor inline on a pool worker — block_gc_destroy parks the pool,
   so call it through a scheduler_pool_defer_cleanup wrapper. */
block_gc_t* block_gc_create(block_cache_t* bc, network_t* network, scheduler_pool_t* pool,
                            const char* text, uint8_t force, uint8_t defrag,
                            float defrag_threshold, actor_t* reply_to);
void block_gc_destroy(block_gc_t* gc);
void block_gc_dispatch(void* state, message_t* msg);

#endif // OFFS_BLOCK_GC_H