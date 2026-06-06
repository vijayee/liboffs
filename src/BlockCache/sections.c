//
// Created by victor on 8/5/25.
//
#include "sections.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/path_join.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/get_dir.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Platform/platform.h"
#include "../Scheduler/scheduler.h"
#include "block.h"
#include <stdatomic.h>

void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node);
void round_robin_save(void* ctx);
void sections_full(sections_t* sections, size_t section_id);
void sections_free(sections_t* sections, size_t section_id);
void sections_dispatch(void* state, message_t* msg);
static void section_on_dirty(void* context, section_t* section);

sections_lru_cache_t* sections_lru_cache_create(size_t size) {
  sections_lru_cache_t* lru = get_clear_memory(sizeof(sections_lru_cache_t));
  lru->size = size;
  lru->first = NULL;
  lru->last = NULL;
  hashmap_init(&lru->cache, (void*) hash_size_t, (void*) compare_size_t);
  hashmap_set_key_alloc_funcs(&lru->cache, duplicate_size_t, (void*)free);
  return lru;
}

void sections_lru_cache_destroy(sections_lru_cache_t* lru) {
  sections_lru_node_t* node;
  PLATFORM_DIAGNOSTIC_PUSH
  PLATFORM_DIAGNOSTIC_IGNORE(-Wmissing-field-initializers)
  hashmap_foreach_data(node, &lru->cache) {
    section_destroy(node->value);
    free(node);
  }
  PLATFORM_DIAGNOSTIC_POP
  hashmap_cleanup(&lru->cache);
  free(lru);
}

section_t* sections_lru_cache_get(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node == NULL) {
    return NULL;
  } else {
    sections_lru_cache_move(lru, node);
    return node->value;
  }
}

void sections_lru_cache_delete(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  if (node != NULL) {
    if (node->previous == NULL) {
      if (node->next == NULL) {
        if (lru->first != NULL) {
          lru->first = NULL;
        }
        if (lru->last != NULL) {
          lru->last = NULL;
        }
      } else {
        sections_lru_node_t* next_node = node->next;
        if (next_node->previous != NULL) {
          next_node->previous = NULL;
        }
        lru->first = node->next;
      }
    } else {
      sections_lru_node_t* previous_node = node->previous;
      if (node->next == NULL) {
        previous_node->next = NULL;
        lru->last = previous_node;
      } else {
        sections_lru_node_t* next_node = node->next;
        next_node->previous = node->previous;
        previous_node->next = node->next;
      }
    }
    hashmap_remove(&lru->cache, &section_id);
    section_destroy(node->value);
    free(node);
  }
}

void sections_lru_cache_put(sections_lru_cache_t* lru, section_t* section) {
  if (lru->size == 0) {
    return;
  }
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section->id);
  if (node == NULL) {
    if (hashmap_size(&lru->cache) == lru->size) {
      if (lru->last != NULL) {
        sections_lru_node_t* last_node = lru->last;
        if (last_node->previous == NULL) {
          lru->last = NULL;
          lru->first = NULL;
        } else {
          sections_lru_node_t* new_last_node = last_node->previous;
          new_last_node->next = NULL;
          lru->last = new_last_node;
        }
        hashmap_remove(&lru->cache, &last_node->value->id);
        section_destroy(last_node->value);
        free(last_node);
      }
    }
    node = get_clear_memory(sizeof(sections_lru_node_t));
    node->previous = NULL;
    node->next = NULL;
    node->value = refcounter_reference((refcounter_t*) section);
    hashmap_put(&lru->cache, &section->id, node);
  }
  sections_lru_cache_move(lru, node);
}

uint8_t sections_lru_cache_contains(sections_lru_cache_t* lru, size_t section_id) {
  sections_lru_node_t* node = hashmap_get(&lru->cache, &section_id);
  return node != NULL;
}

void sections_lru_cache_move(sections_lru_cache_t* lru, sections_lru_node_t* node) {
  if (lru->first == NULL) {
    lru->first = node;
    lru->last = node;
  } else {
    if (lru->first == node) {
      return;
    }
    if (lru->first == lru->last) {
      node->next = lru->first;
      sections_lru_node_t* first_node = lru->first;
      first_node->previous = node;
      lru->last = first_node;
      lru->first = node;
    } else if (lru->last == node) {
      lru->last = node->previous;
      sections_lru_node_t* last_node = lru->last;
      last_node->next = NULL;
      node->next = lru->first;
      node->previous = NULL;
      sections_lru_node_t* first_node = lru->first;
      first_node->previous = node;
      lru->first = node;
    } else {
      if ((node->next == NULL) && (node->previous == NULL)) {
        sections_lru_node_t* first_node = lru->first;
        first_node->previous = node;
        node->next = first_node;
        lru->first = node;
      } else {
        sections_lru_node_t* next_node = node->next;
        if (node->previous != NULL) {
          sections_lru_node_t* previous_node = node->previous;
          previous_node->next = next_node;
        }
        if (node->next != NULL) {
          next_node->previous = node->previous;
        }
        sections_lru_node_t* first_node = lru->first;
        first_node->previous = node;
        node->next = first_node;
        node->previous = NULL;
        lru->first = node;
      }
    }
  }
}

/* ---- section async payload destroy helpers ---- */

static void sections_read_result_destroy(void* ptr) {
  sections_read_result_payload_t* result = (sections_read_result_payload_t*)ptr;
  if (result->data != NULL) {
    buffer_destroy(result->data);
  }
  free(result);
}

/* ---- sections dispatch ---- */

void sections_dispatch(void* state, message_t* msg) {
  sections_t* sections = (sections_t*)state;
  switch (msg->type) {
    case SECTIONS_WRITE: {
      sections_write_payload_t* p = (sections_write_payload_t*)msg->payload;
      p->result = -1;
      p->section_id = 0;
      p->section_index = 0;
      for (size_t attempt = 0; attempt < sections->max_tuple_size; attempt++) {
        if (!round_robin_next(sections->robin, &p->section_id)) {
          break;
        }
        section_t* section = sections_lru_cache_get(sections->lru, p->section_id);
        if (section == NULL) {
          section = section_create(sections->data_path, sections->meta_path,
                                   sections->size, p->section_id, sections->type, sections->pool);
          section->on_dirty = section_on_dirty;
          section->on_dirty_context = sections;
          refcounter_yield((refcounter_t*) section);
          sections_lru_cache_put(sections->lru, section);
          if (section_full(section) != 0) {
            sections_full(sections, p->section_id);
            continue;
          }
        }
        section_write_payload_t write_payload;
        write_payload.data = p->data;
        write_payload.reply_to = NULL;
        write_payload.result = -1;
        write_payload.index = 0;
        write_payload.full = 0;
        message_t section_msg;
        section_msg.type = SECTION_WRITE;
        section_msg.payload = &write_payload;
        section_msg.payload_destroy = NULL;
        section_dispatch(section, &section_msg);
        p->result = write_payload.result;
        p->section_index = write_payload.index;
        if ((p->result == 2) || write_payload.full) {
          sections_full(sections, p->section_id);
        }
        if (p->result != 2) {
          break;
        }
      }
      if (p->reply_to != NULL) {
        sections_write_result_payload_t* result = get_clear_memory(sizeof(sections_write_result_payload_t));
        result->result = p->result;
        result->section_id = p->section_id;
        result->section_index = p->section_index;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTIONS_WRITE_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTIONS_READ: {
      sections_read_payload_t* p = (sections_read_payload_t*)msg->payload;
      p->result = NULL;
      section_t* section = sections_lru_cache_get(sections->lru, p->section_id);
      if (section == NULL) {
        section = section_create(sections->data_path, sections->meta_path,
                                 sections->size, p->section_id, sections->type, sections->pool);
        section->on_dirty = section_on_dirty;
        section->on_dirty_context = sections;
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        if (section_full(section) == 0 && !round_robin_contains(sections->robin, p->section_id)) {
          round_robin_add(sections->robin, p->section_id);
        }
      }
      section_read_payload_t read_payload;
      read_payload.index = p->section_index;
      read_payload.reply_to = NULL;
      read_payload.result = NULL;
      message_t section_msg;
      section_msg.type = SECTION_READ;
      section_msg.payload = &read_payload;
      section_msg.payload_destroy = NULL;
      section_dispatch(section, &section_msg);
      p->result = read_payload.result;
      if (p->reply_to != NULL) {
        sections_read_result_payload_t* result = get_clear_memory(sizeof(sections_read_result_payload_t));
        result->section_id = p->section_id;
        result->section_index = p->section_index;
        result->data = p->result;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTIONS_READ_RESULT;
        reply.payload = result;
        reply.payload_destroy = sections_read_result_destroy;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTIONS_DEALLOCATE: {
      sections_deallocate_payload_t* p = (sections_deallocate_payload_t*)msg->payload;
      p->result = -1;
      section_t* section = sections_lru_cache_get(sections->lru, p->section_id);
      if (section == NULL) {
        section = section_create(sections->data_path, sections->meta_path,
                                 sections->size, p->section_id, sections->type, sections->pool);
        section->on_dirty = section_on_dirty;
        section->on_dirty_context = sections;
        refcounter_yield((refcounter_t*) section);
        sections_lru_cache_put(sections->lru, section);
        if (section_full(section) == 0 && !round_robin_contains(sections->robin, p->section_id)) {
          round_robin_add(sections->robin, p->section_id);
        }
      }
      section_deallocate_payload_t dealloc_payload;
      dealloc_payload.index = p->section_index;
      dealloc_payload.reply_to = NULL;
      dealloc_payload.result = -1;
      message_t section_msg;
      section_msg.type = SECTION_DEALLOCATE;
      section_msg.payload = &dealloc_payload;
      section_msg.payload_destroy = NULL;
      section_dispatch(section, &section_msg);
      p->result = dealloc_payload.result;
      if (p->result == 0) {
        sections_free(sections, p->section_id);
      }
      if (p->reply_to != NULL) {
        sections_deallocate_result_payload_t* result = get_clear_memory(sizeof(sections_deallocate_result_payload_t));
        result->result = p->result;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTIONS_DEALLOCATE_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    case SECTIONS_SECTION_FULL: {
      /* A section reported it is full. Remove from round robin and create
         replacement sections to maintain the pool. Payload is section_id. */
      size_t section_id = (size_t)(uintptr_t)msg->payload;
      sections_full(sections, section_id);
      break;
    }
    case SECTION_SAVE_META: {
      if (sections->robin) round_robin_save(sections->robin);
      break;
    }
    case SECTION_WRITE_META: {
      sections_lru_node_t* node;
      PLATFORM_DIAGNOSTIC_PUSH
      PLATFORM_DIAGNOSTIC_IGNORE(-Wmissing-field-initializers)
      hashmap_foreach_data(node, &sections->lru->cache) {
        if (atomic_load(&node->value->dirty)) {
          section_save_meta(node->value);
          atomic_store(&node->value->dirty, 0);
        }
      }
      PLATFORM_DIAGNOSTIC_POP
      break;
    }
    case SECTION_WRITE_RESULT:
    case SECTION_READ_RESULT:
    case SECTION_DEALLOCATE_RESULT:
      /* Reserved for fully-async block_cache flow. */
      break;
    case SECTIONS_DEFRAGMENT: {
      sections_defragment_payload_t* p = (sections_defragment_payload_t*)msg->payload;
      p->result = -1;
      p->sections_defragmented = 0;
      p->relocations = NULL;
      p->relocation_count = 0;

      size_t capacity = 64;
      p->relocations = get_memory(capacity * sizeof(block_relocation_t));

      sections_lru_node_t* node;
      hashmap_foreach_data(node, &sections->lru->cache) {
        section_t* section = node->value;
        size_t total = section->free_map.total_blocks;
        if (total == 0) goto next_section;
        size_t free_count = section_count_free(section);
        float occupancy = (float)(total - free_count) / (float)total;

        if (occupancy >= p->occupancy_threshold || free_count == total) goto next_section;
        size_t used = total - free_count;
        if (used <= 1) goto next_section;

        /* Dispatch SECTION_DEFRAGMENT synchronously */
        section_defragment_payload_t defrag_payload;
        memset(&defrag_payload, 0, sizeof(defrag_payload));
        defrag_payload.reply_to = NULL;
        defrag_payload.result = -1;
        defrag_payload.section_id = 0;
        defrag_payload.defrag.relocation = NULL;
        defrag_payload.defrag.new_count = 0;
        message_t defrag_msg;
        defrag_msg.type = SECTION_DEFRAGMENT;
        defrag_msg.payload = &defrag_payload;
        defrag_msg.payload_destroy = NULL;
        section_dispatch(section, &defrag_msg);

        if (defrag_payload.result == 0 && defrag_payload.defrag.relocation != NULL) {
          /* Convert per-section relocation array to flat block_relocation_t entries */
          for (size_t i = 0; i < total; i++) {
            if (defrag_payload.defrag.relocation[i] != (size_t)-1 &&
                defrag_payload.defrag.relocation[i] != i) {
              if (p->relocation_count >= capacity) {
                capacity *= 2;
                p->relocations = realloc(p->relocations, capacity * sizeof(block_relocation_t));
              }
              p->relocations[p->relocation_count].section_id = section->id;
              p->relocations[p->relocation_count].old_index = i;
              p->relocations[p->relocation_count].new_index = defrag_payload.defrag.relocation[i];
              p->relocation_count++;
            }
          }
          free(defrag_payload.defrag.relocation);
          p->sections_defragmented++;
        }
        next_section:;
      }

      /* Trim the relocations array to actual size */
      if (p->relocation_count == 0) {
        free(p->relocations);
        p->relocations = NULL;
      } else if (p->relocation_count < capacity) {
        p->relocations = realloc(p->relocations, p->relocation_count * sizeof(block_relocation_t));
      }

      p->result = 0;

      /* Send completion message if async (reply_to is set) */
      if (p->reply_to != NULL) {
        sections_defragment_result_payload_t* result =
            get_clear_memory(sizeof(sections_defragment_result_payload_t));
        result->result = p->result;
        result->sections_defragmented = p->sections_defragmented;
        result->relocation_count = p->relocation_count;
        result->reply_to = NULL;
        message_t reply;
        reply.type = SECTIONS_DEFRAGMENT_RESULT;
        reply.payload = result;
        reply.payload_destroy = free;
        actor_send(p->reply_to, &reply);
      }
      break;
    }
    default:
      break;
  }
}

static void section_on_dirty(void* context, section_t* section) {
  (void)section;
  sections_t* sections = (sections_t*)context;
  if (sections->timer_actor != NULL) {
    timer_actor_debounce(sections->timer_actor,
                         sections->wait, sections->max_wait,
                         &sections->actor, SECTION_WRITE_META);
  } else {
    section_save_meta(section);
    atomic_store(&section->dirty, 0);
  }
}

/* Async API — send message to sections actor and inject into scheduler.
   Results arrive as SECTIONS_READ_RESULT / SECTIONS_WRITE_RESULT /
   SECTIONS_DEALLOCATE_RESULT messages on the reply_to actor. */
void sections_read(sections_t* sections, size_t section_id, size_t section_index, actor_t* reply_to) {
  sections_read_payload_t* payload = get_clear_memory(sizeof(sections_read_payload_t));
  payload->section_id = section_id;
  payload->section_index = section_index;
  payload->reply_to = reply_to;
  payload->result = NULL;

  message_t msg;
  msg.type = SECTIONS_READ;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&sections->actor, &msg);
}

void sections_write(sections_t* sections, buffer_t* data, actor_t* reply_to) {
  sections_write_payload_t* payload = get_clear_memory(sizeof(sections_write_payload_t));
  payload->data = data;
  payload->reply_to = reply_to;
  payload->result = -1;
  payload->section_id = 0;
  payload->section_index = 0;

  message_t msg;
  msg.type = SECTIONS_WRITE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&sections->actor, &msg);
}

void sections_deallocate(sections_t* sections, size_t section_id, size_t section_index, actor_t* reply_to) {
  sections_deallocate_payload_t* payload = get_clear_memory(sizeof(sections_deallocate_payload_t));
  payload->section_id = section_id;
  payload->section_index = section_index;
  payload->reply_to = reply_to;
  payload->result = -1;

  message_t msg;
  msg.type = SECTIONS_DEALLOCATE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&sections->actor, &msg);
}

void sections_defragment(sections_t* sections, float occupancy_threshold, actor_t* reply_to) {
  sections_defragment_payload_t* payload = get_clear_memory(sizeof(sections_defragment_payload_t));
  payload->occupancy_threshold = occupancy_threshold;
  payload->reply_to = reply_to;
  payload->result = -1;
  payload->sections_defragmented = 0;
  payload->relocations = NULL;
  payload->relocation_count = 0;

  message_t msg;
  msg.type = SECTIONS_DEFRAGMENT;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&sections->actor, &msg);
}

/* ---- round_robin (lock-free, only called from sections_dispatch) ---- */

round_robin_t* round_robin_create(char* robin_path, timer_actor_t* timer_actor, actor_t* save_target, uint64_t wait, uint64_t max_wait) {
  round_robin_t* robin = get_clear_memory(sizeof(round_robin_t));
  robin->timer_actor = timer_actor;
  robin->save_target = save_target;
  robin->wait = wait;
  robin->max_wait = max_wait;
  robin->path = robin_path;
  return robin;
}

void round_robin_destroy(round_robin_t* robin) {
  if (robin->timer_actor != NULL && robin->save_target != NULL
      && robin->save_target->pool != NULL
      && !atomic_load(&robin->save_target->pool->terminate)) {
    timer_actor_debounce_flush(robin->timer_actor, robin->save_target, SECTION_SAVE_META);
    platform_sleep_ms(10);
    scheduler_pool_wait_for_idle(robin->save_target->pool);
  }
  round_robin_save(robin);
  free(robin->path);
  round_robin_node_t* current = robin->first;
  while (current != NULL) {
    round_robin_node_t* next = current->next;
    free(current);
    current = next;
  }
  free(robin);
}

void round_robin_add(round_robin_t* robin, size_t id) {
  round_robin_node_t* node = get_clear_memory(sizeof(round_robin_node_t));
  node->id = id;
  node->previous = NULL;
  node->next = NULL;
  if ((robin->last == NULL) && (robin->first == NULL)) {
    robin->first = node;
    robin->last = node;
  } else {
    node->previous = robin->last;
    robin->last->next = node;
    robin->last = node;
  }
  if (robin->timer_actor != NULL && robin->save_target != NULL) {
    timer_actor_debounce(robin->timer_actor, robin->wait, robin->max_wait, robin->save_target, SECTION_SAVE_META);
  }
  robin->size++;
}

void round_robin_unshift(round_robin_t* robin, size_t id) {
  round_robin_node_t* node = get_clear_memory(sizeof(round_robin_node_t));
  node->id = id;
  node->next = robin->first;
  node->previous = NULL;
  if (robin->first != NULL) {
    robin->first->previous = node;
  } else {
    robin->last = node;
  }
  robin->first = node;
  if (robin->timer_actor != NULL && robin->save_target != NULL) {
    timer_actor_debounce(robin->timer_actor, robin->wait, robin->max_wait, robin->save_target, SECTION_SAVE_META);
  }
  robin->size++;
}

int round_robin_next(round_robin_t* robin, size_t* out_id) {
  if ((robin->last == NULL) && (robin->first == NULL)) {
    return 0;
  }
  round_robin_node_t* node = robin->first;
  if (robin->last == node) {
    *out_id = node->id;
    return 1;
  }
  robin->first = node->next;
  if (node->next != NULL) {
    node->next->previous = NULL;
    node->next = NULL;
  }
  *out_id = node->id;
  node->previous = robin->last;
  robin->last->next = node;
  robin->last = node;
  return 1;
}

void round_robin_remove(round_robin_t* robin, size_t id) {
  if ((robin->last == NULL) && (robin->first == NULL)) {
    return;
  }
  round_robin_node_t* current = robin->first;
  while (current != NULL) {
    if (current->id == id) {
      if (robin->last == current) {
        robin->last = current->previous;
      }
      if (robin->first == current) {
        robin->first = current->next;
      }
      if (current->previous != NULL) {
        current->previous->next = current->next;
      }
      if (current->next != NULL) {
        current->next->previous = current->previous;
      }
      free(current);
      if (robin->timer_actor != NULL && robin->save_target != NULL) {
        timer_actor_debounce(robin->timer_actor, robin->wait, robin->max_wait, robin->save_target, SECTION_SAVE_META);
      }
      robin->size--;
      return;
    } else {
      current = current->next;
    }
  }
}

uint8_t round_robin_contains(round_robin_t* robin, size_t id) {
  if ((robin->last == NULL) && (robin->first == NULL)) {
    return 0;
  }
  round_robin_node_t* current = robin->first;
  while (current != NULL) {
    if (current->id == id) {
      return 1;
    } else {
      current = current->next;
    }
  }
  return 0;
}

cbor_item_t* round_robin_to_cbor(round_robin_t* robin) {
  cbor_item_t* array = cbor_new_definite_array(robin->size);
  round_robin_node_t* current = robin->first;
  bool success = true;
  while (current != NULL) {
    success &= cbor_array_push(array, cbor_move(cbor_build_uint64(current->id)));
    current = current->next;
  }
  if (!success) {
    cbor_decref(&array);
    return NULL;
  } else {
    return array;
  }
}

round_robin_t* cbor_to_round_robin(cbor_item_t* cbor, char* robin_path, timer_actor_t* timer_actor, actor_t* save_target, uint64_t wait, uint64_t max_wait) {
  if(!cbor_isa_array(cbor)) {
    return NULL;
  }
  round_robin_t* robin = round_robin_create(robin_path, timer_actor, save_target, wait, max_wait);
  size_t size = cbor_array_size(cbor);
  for(size_t i = 0; i < size; i++) {
    cbor_item_t* cbor_id = cbor_array_get(cbor, i);
    round_robin_add(robin, cbor_get_int(cbor_id));
    cbor_decref(&cbor_id);
  }
  return robin;
}

void round_robin_save(void* ctx) {
  round_robin_t* robin = (round_robin_t*) ctx;
  cbor_item_t* cbor = round_robin_to_cbor(robin);
  if (cbor == NULL) {
    log_error("Failed to save robin file");
    return;
  }
  uint8_t* cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  FILE* robin_file = fopen(robin->path,"wb");
  if (robin_file == NULL) {
    log_error("Failed to save robin file");
    return;
  }
  fwrite(cbor_data, cbor_size, 1, robin_file);
  fflush(robin_file);
  fclose(robin_file);
  free(cbor_data);
  cbor_decref(&cbor);
}

/* ---- sections implementation ---- */

sections_t* sections_create(char* path, size_t size, size_t cache_size, size_t max_tuple_size, block_size_e type, timer_actor_t* timer_actor, scheduler_pool_t* pool, size_t wait, size_t max_wait) {
  char* robin_folder = path_join(path, "robin");
  mkdir_p(robin_folder);
  char* robin_path = path_join(robin_folder, ".robin");
  free(robin_folder);
  sections_t* sections = get_clear_memory(sizeof(sections_t));
  sections->timer_actor = timer_actor;
  sections->pool = pool;
  sections->wait = wait;
  sections->max_wait = max_wait;
  sections->type = type;
  sections->max_tuple_size = max_tuple_size;
  sections->size = size;
  actor_init(&sections->actor, sections, sections_dispatch, pool);
  if (platform_file_exists(robin_path)) {
    FILE* robin_file = fopen(robin_path, "rb");
    if (robin_file == NULL || fseek(robin_file, 0, SEEK_END)) {
      log_error("Failed to read round robin file size; recreating");
      if (robin_file != NULL) fclose(robin_file);
      platform_file_unlink(robin_path);
      sections->robin = round_robin_create(robin_path, timer_actor, &sections->actor, wait, max_wait);
    } else {
      long file_size = ftell(robin_file);
      rewind(robin_file);
      uint8_t* buffer = get_memory((size_t)file_size);
      size_t read_size = fread(buffer, sizeof(uint8_t), (size_t)file_size, robin_file);
      fclose(robin_file);
      int valid = 1;
      cbor_item_t* cbor = NULL;
      if ((size_t)file_size != read_size) {
        log_error("Failed to read round robin file; recreating");
        valid = 0;
      } else {
        struct cbor_load_result result;
        cbor = cbor_load(buffer, (size_t)file_size, &result);
        if (result.error.code != CBOR_ERR_NONE || !cbor_isa_array(cbor)) {
          log_error("Failed to parse round robin file; recreating");
          valid = 0;
        }
      }
      free(buffer);
      if (valid) {
        sections->robin = cbor_to_round_robin(cbor, robin_path, timer_actor, &sections->actor, wait, max_wait);
        cbor_decref(&cbor);
      } else {
        if (cbor != NULL) cbor_decref(&cbor);
        platform_file_unlink(robin_path);
        sections->robin = round_robin_create(robin_path, timer_actor, &sections->actor, wait, max_wait);
      }
    }
  } else {
    sections->robin = round_robin_create(robin_path, timer_actor, &sections->actor, wait, max_wait);
  }
  sections->lru = sections_lru_cache_create(cache_size);
  sections->data_path = path_join(path, "data");
  mkdir_p(sections->data_path);
  sections->meta_path = path_join(path, "meta");
  mkdir_p(sections->meta_path);

  vec_str_t* files = get_dir(sections->meta_path);
  if (files != NULL && files->length > 0) {
    char* last = vec_last(files);
    uint64_t last_id = strtoull(last, NULL, 10);
    sections->next_id = last_id + 1;
  } else {
    sections->next_id = 0;
  }
  if (files != NULL) {
    vec_deinit(files);
    free(files);
  }

  /* Replace any full sections in the robin with fresh ones.
     Stale sections from a previous run may have all blocks occupied. */
  {
    size_t ids[sections->robin->size];
    size_t robin_size = 0;
    round_robin_node_t* node = sections->robin->first;
    while (node != NULL) {
      ids[robin_size++] = node->id;
      node = node->next;
    }
    for (size_t i = 0; i < robin_size; i++) {
      section_t* section = section_create(sections->data_path, sections->meta_path,
                                          sections->size, ids[i], sections->type, sections->pool);
      section->on_dirty = section_on_dirty;
      section->on_dirty_context = sections;
      refcounter_yield((refcounter_t*) section);
      sections_lru_cache_put(sections->lru, section);
      if (section_full(section) != 0) {
        sections_full(sections, ids[i]);
      }
    }
  }

  while (sections->robin->size < sections->max_tuple_size) {
    section_t* section = section_create(sections->data_path, sections->meta_path, sections->size, sections->next_id++, sections->type, sections->pool);
    section->on_dirty = section_on_dirty;
    section->on_dirty_context = sections;
    refcounter_yield((refcounter_t*) section);
    sections_lru_cache_put(sections->lru, section);
    round_robin_add(sections->robin, section->id);
  }
  return sections;
}

void sections_destroy(sections_t* sections) {
  if (sections->timer_actor != NULL && sections->actor.pool != NULL
      && !atomic_load(&sections->actor.pool->terminate)) {
    timer_actor_debounce_flush(sections->timer_actor, &sections->actor, SECTION_WRITE_META);
    platform_sleep_ms(10);
    scheduler_pool_wait_for_idle(sections->actor.pool);
  }
  sections_lru_cache_destroy(sections->lru);
  actor_destroy(&sections->actor);
  round_robin_destroy(sections->robin);
  free(sections->meta_path);
  free(sections->data_path);
  free(sections->robin_path);
  free(sections);
}

void sections_full(sections_t* sections, size_t section_id) {
  round_robin_remove(sections->robin, section_id);
  while (sections->robin->size < sections->max_tuple_size) {
    section_t* section = section_create(sections->data_path, sections->meta_path, sections->size, sections->next_id++, sections->type, sections->pool);
    section->on_dirty = section_on_dirty;
    section->on_dirty_context = sections;
    refcounter_yield((refcounter_t*) section);
    sections_lru_cache_put(sections->lru, section);
    round_robin_unshift(sections->robin, section->id);
  }
}

void sections_free(sections_t* sections, size_t section_id) {
  if (round_robin_contains(sections->robin, section_id) == 0) {
    round_robin_add(sections->robin, section_id);
  }
}

