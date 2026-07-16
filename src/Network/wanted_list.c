//
// Created by victor on 5/15/25.
//

#include "wanted_list.h"
#include "../Util/allocator.h"
#include "../RefCounter/refcounter.h"

wanted_list_t* wanted_list_create(void) {
  wanted_list_t* wl = get_clear_memory(sizeof(wanted_list_t));
  size_t bloom_size;
  uint32_t hash_count;
  bloom_filter_optimal_size(10000, 0.01, &bloom_size, &hash_count);
  if (bloom_size < WANTED_BLOOM_SIZE) bloom_size = WANTED_BLOOM_SIZE;
  if (hash_count < WANTED_BLOOM_HASHES) hash_count = WANTED_BLOOM_HASHES;
  wl->bloom = bloom_filter_create(bloom_size, hash_count);
  wl->entries = NULL;
  wl->entry_count = 0;
  return wl;
}

void wanted_list_destroy(wanted_list_t* wl) {
  if (wl == NULL) return;
  wanted_entry_t* entry = wl->entries;
  while (entry != NULL) {
    wanted_entry_t* next = entry->next;
    if (entry->hash != NULL) buffer_destroy(entry->hash);
    wanted_requester_t* req = entry->requesters;
    while (req != NULL) {
      wanted_requester_t* next_req = req->next;
      free(req);
      req = next_req;
    }
    free(entry);
    entry = next;
  }
  bloom_filter_destroy(wl->bloom);
  free(wl);
}

bool wanted_list_check(wanted_list_t* wl, buffer_t* hash) {
  if (wl == NULL) return false;
  return bloom_filter_contains(wl->bloom, hash->data, hash->size);
}

wanted_entry_t* wanted_list_find(wanted_list_t* wl, buffer_t* hash) {
  if (wl == NULL) return NULL;
  wanted_entry_t* entry = wl->entries;
  while (entry != NULL) {
    if (buffer_compare(entry->hash, hash) == 0) return entry;
    entry = entry->next;
  }
  return NULL;
}

void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor,
                     uint64_t deadline_ms) {
  wanted_entry_t* entry = wanted_list_find(wl, hash);
  if (entry != NULL) {
    /* Entry exists — prepend requester. Preserve the existing deadline (the
       first requester set it; a late joiner shouldn't extend it). */
    wanted_requester_t* req = get_clear_memory(sizeof(wanted_requester_t));
    req->actor = actor;
    req->next = entry->requesters;
    entry->requesters = req;
  } else {
    /* New entry */
    if (hash != NULL && hash->data != NULL && hash->size > 0) {
      bloom_filter_add(wl->bloom, hash->data, hash->size);
    }
    entry = get_clear_memory(sizeof(wanted_entry_t));
    entry->hash = REFERENCE(hash, buffer_t);
    entry->deadline_ms = deadline_ms;
    entry->fanout_count = 0;
    entry->not_found_count = 0;
    wanted_requester_t* req = get_clear_memory(sizeof(wanted_requester_t));
    req->actor = actor;
    req->next = NULL;
    entry->requesters = req;
    entry->next = wl->entries;
    wl->entries = entry;
    wl->entry_count++;
  }
}

wanted_entry_t* wanted_list_get(wanted_list_t* wl, buffer_t* hash) {
  return wanted_list_find(wl, hash);
}

size_t wanted_list_sweep(wanted_list_t* wl, uint64_t now_ms,
                         wanted_list_sweep_cb cb, void* user_data) {
  if (wl == NULL) return 0;
  size_t swept = 0;
  wanted_entry_t** current = &wl->entries;
  while (*current != NULL) {
    wanted_entry_t* entry = *current;
    if (entry->deadline_ms != 0 && entry->deadline_ms <= now_ms) {
      /* Expired — unlink, hand (hash, requesters) to the callback, then free
         the entry. The callback owns `requesters` (must free each requester).
         The callback may REFERENCE `hash` to keep it alive beyond the callback;
         the buffer_destroy below decrements the refcount, so REFERENCE nets
         +1 for the callback's result. See audit #5/#9. */
      *current = entry->next;
      wl->entry_count--;
      if (cb != NULL) {
        cb(entry->hash, entry->requesters, user_data);
      } else {
        /* No callback — free the requesters ourselves (caller opted out). */
        wanted_requester_list_destroy(entry->requesters);
      }
      buffer_destroy(entry->hash);
      free(entry);
      swept++;
    } else {
      current = &entry->next;
    }
  }
  return swept;
}

wanted_requester_t* wanted_list_remove(wanted_list_t* wl, buffer_t* hash) {
  wanted_entry_t** current = &wl->entries;
  while (*current != NULL) {
    if (buffer_compare((*current)->hash, hash) == 0) {
      wanted_entry_t* entry = *current;
      *current = entry->next;
      wl->entry_count--;
      wanted_requester_t* requesters = entry->requesters;
      buffer_destroy(entry->hash);
      free(entry);
      /* Note: bloom filters don't support deletion.
       * False positives from stale bloom entries just mean
       * a redundant FindBlock that resolves quickly.
       * The bloom is rebuilt periodically to keep false positive rate low. */
      return requesters;
    }
    current = &(*current)->next;
  }
  return NULL;
}

void wanted_requester_list_destroy(wanted_requester_t* requesters) {
  while (requesters != NULL) {
    wanted_requester_t* next = requesters->next;
    free(requesters);
    requesters = next;
  }
}