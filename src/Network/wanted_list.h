//
// Created by victor on 5/15/25.
//

#ifndef OFFS_WANTED_LIST_H
#define OFFS_WANTED_LIST_H

#include "../Bloom/bloom_filter.h"
#include "../Actor/actor.h"
#include "../Buffer/buffer.h"
#include <stdint.h>
#include <stdbool.h>

#define WANTED_BLOOM_SIZE       4096
#define WANTED_BLOOM_HASHES     3

typedef struct wanted_requester_t {
  actor_t*  actor;
  struct wanted_requester_t* next;
} wanted_requester_t;

typedef struct wanted_entry_t {
  buffer_t* hash;
  wanted_requester_t* requesters;
  uint64_t deadline_ms;       /* 0 = no deadline (back-compat; never swept) */
  uint64_t message_id;        /* the FindBlock request's message_id; the origin
                                 checks this against a not-found response's
                                 message_id before counting it, so a stale or
                                 forged not-found for a different request
                                 can't prematurely fail the current search.
                                 0 = unset (no message_id check). */
  uint8_t  fanout_count;      /* # of next-hops the origin fanned out to */
  uint8_t  not_found_count;   /* # of not-found responses received */
  struct wanted_entry_t* next;
} wanted_entry_t;

typedef struct wanted_list_t {
  bloom_filter_t* bloom;
  wanted_entry_t* entries;
  size_t          entry_count;
} wanted_list_t;

/* Callback invoked by wanted_list_sweep for each entry whose deadline_ms has
   passed. The callback takes ownership of the `requesters` list (it must free
   each requester with free()). The `hash` is owned by the sweep; the callback
   may REFERENCE it to keep it alive beyond the callback — the sweep calls
   buffer_destroy(entry->hash) after the callback returns, so a REFERENCE
   bumps the refcount and the sweep's buffer_destroy decrements it (net +1
   for the callback's result). See audit #5/#9. */
typedef void (*wanted_list_sweep_cb)(buffer_t* hash,
                                     wanted_requester_t* requesters,
                                     void* user_data);

wanted_list_t* wanted_list_create(void);
void wanted_list_destroy(wanted_list_t* wl);
bool wanted_list_check(wanted_list_t* wl, buffer_t* hash);
wanted_entry_t* wanted_list_find(wanted_list_t* wl, buffer_t* hash);

/* Get (without removing) an entry. Same as wanted_list_find; named for the
   fanout/not_found accounting use case in Task 4. */
wanted_entry_t* wanted_list_get(wanted_list_t* wl, buffer_t* hash);

/* Add `actor` as a requester for `hash`. If an entry already exists, only the
   requester is prepended (the existing deadline_ms is preserved). If this is
   a new entry, `deadline_ms` is stored on the entry (0 = no deadline, never
   swept — back-compat). fanout_count and not_found_count start at 0. */
void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor,
                     uint64_t deadline_ms);

wanted_requester_t* wanted_list_remove(wanted_list_t* wl, buffer_t* hash);
void wanted_requester_list_destroy(wanted_requester_t* requesters);

/* Walk the list, remove every entry whose deadline_ms != 0 and
   deadline_ms <= now_ms, and invoke `cb` for each expired entry with its
   hash and requesters. Entries with deadline_ms == 0 are never swept.
   Returns the number of entries swept. See audit #5/#9. */
size_t wanted_list_sweep(wanted_list_t* wl, uint64_t now_ms,
                         wanted_list_sweep_cb cb, void* user_data);

#endif // OFFS_WANTED_LIST_H