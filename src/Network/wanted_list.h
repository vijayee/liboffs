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
  struct wanted_entry_t* next;
} wanted_entry_t;

typedef struct wanted_list_t {
  bloom_filter_t* bloom;
  wanted_entry_t* entries;
  size_t          entry_count;
} wanted_list_t;

wanted_list_t* wanted_list_create(void);
void wanted_list_destroy(wanted_list_t* wl);
bool wanted_list_check(wanted_list_t* wl, buffer_t* hash);
wanted_entry_t* wanted_list_find(wanted_list_t* wl, buffer_t* hash);
void wanted_list_add(wanted_list_t* wl, buffer_t* hash, actor_t* actor);
wanted_requester_t* wanted_list_remove(wanted_list_t* wl, buffer_t* hash);
wanted_requester_t* wanted_list_clear_requesters(wanted_list_t* wl, buffer_t* hash);

#endif // OFFS_WANTED_LIST_H