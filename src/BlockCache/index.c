//
// Created by victor on 4/29/25.
//
#include "index.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"
#include "../Util/log.h"
#include "../Util/mkdir_p.h"
#include "../Util/path_join.h"
#include "../Util/get_dir.h"
#include "../Util/portable_endian.h"
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <xxh3.h>
#include <string.h>


void index_add_to_node(index_t* index, index_entry_t* entry, index_node_t* node, size_t current);
void index_split_node(index_t* index, index_node_t* node, size_t current);
int _index_node_to_crc(index_node_t* node, XXH64_state_t* const state);
int _index_to_crc(index_t* index, uint64_t* crc);
int _sort_indexes( const void *str1, const void *str2 );
int _index_get_id_crc(char* filename, uint64_t* id, uint64_t* crc);

index_entry_t* index_get_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current);
index_entry_t* index_find_in_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current);
void index_remove_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current);
void index_destroy_node(index_t* index, index_node_t* node);
void index_debounce(index_t* index);
static uint64_t _index_prune_old_snapshots(index_t* index);
static void _index_prune_old_wals(index_t* index, uint64_t first_kept_id);
size_t _index_count(index_t* index);
void _index_increment(index_t* index, index_entry_t* entry);
cbor_item_t* _index_to_cbor(index_t* index);

uint8_t get_bit(buffer_t* buffer, size_t index) {
  size_t byte = index / 8;
  size_t byteIndex = index % 8;
  return ((buffer_get_index(buffer,byte) >> byteIndex)  & 0x01);
}

index_entry_t* index_entry_create(buffer_t* hash) {
  index_entry_t* entry = get_clear_memory(sizeof(index_entry_t));
  refcounter_init((refcounter_t*) entry);
  entry->counter = fibonacci_hit_counter_create();
  entry->hash = (buffer_t*) refcounter_reference( (refcounter_t*) hash);
  return entry;
}

index_entry_t* index_entry_from(buffer_t* hash, size_t section_id, size_t section_index, uint64_t ejection_date, fibonacci_hit_counter_t counter) {
  index_entry_t* entry = get_clear_memory(sizeof(index_entry_t));
  refcounter_init((refcounter_t*) entry);
  entry->hash= (buffer_t*) refcounter_reference((refcounter_t*) hash);
  entry->counter = counter;
  entry->section_id = section_id;
  entry->section_index = section_index;
  entry->ejection_date = ejection_date;
  return entry;
}

void index_entry_destroy(index_entry_t* entry) {
  refcounter_dereference((refcounter_t*) entry);
  if (refcounter_count((refcounter_t*) entry) == 0) {
    refcounter_destroy_lock((refcounter_t *) entry);
    buffer_destroy(entry->hash);
    free(entry);
  }
}

int index_entry_increment(index_entry_t* entry) {
  int promoted;
  promoted = fibonacci_hit_counter_increment(&entry->counter);
  return promoted;
}

void index_entry_set_ejection_date(index_entry_t* entry, uint64_t ejection_date) {
  entry->ejection_date = ejection_date;
}

cbor_item_t* index_entry_to_cbor(index_entry_t* entry) {
  cbor_item_t* array = cbor_new_definite_array(5);
  bool success = cbor_array_push(array, cbor_move(fibonacci_hit_counter_to_cbor(&entry->counter)));
  success &= cbor_array_push(array, cbor_move(buffer_to_cbor(entry->hash)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->section_index)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->section_id)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(entry->ejection_date)));
  if (!success) {
    cbor_decref(&array);
    return NULL;
  } else {
    return array;
  }
}

index_entry_t* cbor_to_index_entry(cbor_item_t* cbor) {
 cbor_item_t* item0 = cbor_array_get(cbor, 0);
 cbor_item_t* item1 = cbor_array_get(cbor, 1);
 cbor_item_t* item2 = cbor_array_get(cbor, 2);
 cbor_item_t* item3 = cbor_array_get(cbor, 3);
 cbor_item_t* item4 = cbor_array_get(cbor, 4);
 fibonacci_hit_counter_t counter = cbor_to_fibonacci_hit_counter(item0);
 buffer_t* hash = cbor_to_buffer(item1);
 size_t section_index = (size_t) cbor_get_int(item2);
 size_t section_id = (size_t) cbor_get_int(item3);
 uint64_t ejection_date = (size_t) cbor_get_int(item4);
 cbor_decref(&item0);
 cbor_decref(&item1);
 cbor_decref(&item2);
 cbor_decref(&item3);
 cbor_decref(&item4);
 refcounter_yield((refcounter_t*) hash);
 return index_entry_from(hash, section_id, section_index, ejection_date, counter);
}

index_node_t* index_node_create(size_t bucket_size) {
  index_node_t* node = get_clear_memory(sizeof(index_node_t));
  refcounter_init((refcounter_t*) node);
  node->bucket = get_clear_memory(sizeof(index_entry_vec_t));
  vec_init(node->bucket);
  vec_reserve(node->bucket, bucket_size);
  node->left = NULL;
  node->right = NULL;
  return node;
}
index_node_t* index_node_create_from_leaves(index_node_t* left, index_node_t* right) {
  index_node_t* node = get_clear_memory(sizeof(index_node_t));
  refcounter_init((refcounter_t*) node);
  node->bucket = NULL;
  node->left = (index_node_t*) refcounter_reference((refcounter_t*) left);
  node->right = (index_node_t*) refcounter_reference((refcounter_t*) right);
  return node;
}

void index_node_destroy(index_node_t* node) {
  refcounter_dereference((refcounter_t*) node);
  if (refcounter_count((refcounter_t*) node) == 0) {
    refcounter_destroy_lock((refcounter_t*) node);
    if (node->bucket == NULL) {
      index_node_destroy(node->left);
      index_node_destroy(node->right);
    } else {
      for (int i = 0; i < node->bucket->length; i++) {
        index_entry_destroy(node->bucket->data[i]);
      }
      vec_deinit(node->bucket);
      free(node->bucket);
    }
    free(node);
  }
}

index_t* _index_new_empty(size_t bucket_size, char* location, uint64_t wait, uint64_t max_wait, uint64_t most_recent_id, size_t max_snapshots, size_t max_wals) {
  index_t* index = get_clear_memory(sizeof(index_t));
  index->bucket_size = bucket_size;
  index->root = index_node_create(bucket_size);
  index->location = path_join(location,"index");
  index->parent_location = strdup(location);
  mkdir_p(index->location);
  uint64_t current_id = most_recent_id + 1;
  char id[20];
  sprintf(id,"%lu", current_id);
  index->next_id = most_recent_id + 2;
  index->current_file = path_join(index->location, id);
  index->last_file = NULL;

  index->wal = wal_create(index->parent_location, current_id);
  index->max_snapshots = max_snapshots;
  index->max_wals = max_wals;
  index->wait = wait;
  index->max_wait = max_wait;
  hashmap_init(&index->ranks, (void*)hash_uint32, (void*)compare_uint32);
  hashmap_set_key_alloc_funcs(&index->ranks, duplicate_uint32, (void*)free);
  refcounter_init((refcounter_t*) index);
  return index;
}

index_t* index_create(size_t bucket_size, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals, int* error_code) {
  *error_code = 0;
  index_t* index;
  char* index_location = path_join(location,"index");
  char* parent_location = strdup(location);
  mkdir_p(index_location);
  vec_str_t* files = get_dir(index_location);
  uint64_t most_recent_id = 0;

  if (files != NULL && files->length > 0) {
    vec_sort(files, _sort_indexes);
    for (int i = (int)files->length - 1; i >= 0; i--) { //loop through index files to find first valid file
      //Get index's crc
      char* last = files->data[i];
      uint64_t last_id = 0;
      uint64_t last_crc = 0;
      if (_index_get_id_crc(last, &last_id, &last_crc) == 1) {
        log_error("index_create: index file %d (%s) has invalid name", i, last);
        continue;
      }
      char* index_file_location = path_join(index_location, last);

      platform_file_t* index_file = platform_file_open(index_file_location, PLATFORM_O_RDONLY, 0644);
      free(index_file_location);
      if (index_file == NULL) {
        log_error("index_create: index file %d (%s) failed to open", i, last);
        *error_code = -1;
        continue;
      }
      int64_t size = platform_file_seek(index_file, 0, PLATFORM_SEEK_END);
      if (size < 0) {
        log_error("index file %d empty", i);
        *error_code= -1;
        platform_file_close(index_file);
        continue;
      }
      if (platform_file_seek(index_file, 0, PLATFORM_SEEK_SET) < 0) {
        log_error("index file %d failed to seek start", i);
        *error_code= -2;
        platform_file_close(index_file);
        continue;
      }
      uint8_t* buffer = get_clear_memory((size_t)size);
      if (buffer == NULL) {
        log_error("index file %d failed to allocate read buffer", i);
        *error_code = -3;
        platform_file_close(index_file);
        continue;
      }
      ssize_t bytes = platform_file_read(index_file, buffer, (size_t)size);

      if ((size_t)size != (size_t)bytes) {
        log_error("index file %lu failed to read file", i);
        *error_code= -3;
        free(buffer);
        platform_file_close(index_file);
        continue;
      }
      struct cbor_load_result result;

      cbor_item_t* cbor = cbor_load(buffer, (size_t)size, &result);
      free(buffer);
      platform_file_close(index_file);

      if (result.error.code != CBOR_ERR_NONE) {
        *error_code= -4;
        log_error("index_create: index file %d (%s) failed to load CBOR: %d", i, last, result.error.code);
        continue;
      }
      if(!cbor_isa_array(cbor)) {
        cbor_decref(&cbor);
        *error_code= -5;
        log_error("index_create: index file %d (%s) CBOR is not an array", i, last);
        continue;
      }
      index = cbor_to_index(cbor, location, wait, max_wait, max_snapshots, max_wals);
      cbor_decref(&cbor);
      if (index == NULL) {
        log_error("index_create: cbor_to_index returned NULL for file %d (%s)", i, last);
        continue;
      }
      uint64_t crc;
      int crc_result = _index_to_crc(index, &crc);
      if (crc_result != 0) {
        log_error("index_create: _index_to_crc failed for file %d (%s): %d", i, last, crc_result);
        DESTROY(index, index);
        continue;
      }
      if (crc != last_crc) { //Index is invalid, continue to iterate backward until we have a valid index
        log_error("index_create: CRC mismatch for file %d (%s): computed=%lu, expected=%lu", i, last, crc, last_crc);
        DESTROY(index, index);
        continue;
      } else {
        if (i != (int)(files->length - 1)) {
          index->is_rebuilding = 1;
          for (int j = i + 1; j < files->length; j++) {
            char* next = files->data[j];
            uint64_t next_id = 0;
            uint64_t next_crc = 0;
            _index_get_id_crc(next, &next_id, &next_crc);
            wal_t* wal = wal_load(parent_location, next_id);
            wal_type_e type = 'r';
            buffer_t* data;
            uint64_t cursor;
            int32_t wal_size;
            int read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
            while ((read_result == 0) && (cursor <= (uint64_t)wal_size)) {
              struct cbor_load_result result;
              cbor_item_t* cbor;
              switch (type) {
                case 'a':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    index_entry_t *entry = cbor_to_index_entry(cbor);
                    index_add(index, CONSUME(entry, index_entry_t));
                    cbor_decref(&cbor);
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code= -6;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                  }
                  break;
                case 'i':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    index_entry_t* entry = cbor_to_index_entry(cbor);
                    index_entry_t* from_index = REFERENCE(index_find(index, entry->hash), index_entry_t);
                    index_increment(index, from_index);
                    cbor_decref(&cbor);

                    DESTROY(entry, index_entry);
                    DESTROY(from_index, index_entry);
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code= -7;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                  }
                  break;
                case 'e':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    if (cbor_isa_array(cbor)) {
                      cbor_item_t* cbor_hash = cbor_array_get(cbor, 0);
                      cbor_item_t* cbor_date = cbor_array_get(cbor, 1);
                      if (cbor_isa_bytestring(cbor_hash) && cbor_isa_uint(cbor_date)) {
                        buffer_t* hash = cbor_to_buffer(cbor_hash);
                        index_entry_t* entry = index_find(index, hash);
                        index_entry_set_ejection_date(entry, cbor_get_int(cbor_date));
                        DESTROY(hash, buffer);
                        cbor_decref(&cbor_hash);
                        cbor_decref(&cbor_date);
                        cbor_decref(&cbor);
                      } else {
                        cbor_decref(&cbor_hash);
                        cbor_decref(&cbor_date);
                        cbor_decref(&cbor);
                        DESTROY(index, index);
                        *error_code = read_result;
                        free(index_location);
                        free(parent_location);
                        destroy_files(files);
                        return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                      }
                    }
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code = -8;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                  }
                  break;
                case 'r':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    if (cbor_isa_bytestring(cbor)) {
                      buffer_t* hash = cbor_to_buffer(cbor);
                      index_remove(index, hash);
                      DESTROY(hash, buffer);
                    } else {
                      cbor_decref(&cbor);
                      DESTROY(index, index);
                      *error_code = -9;
                      free(index_location);
                      free(parent_location);
                      destroy_files(files);
                      return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                    }
                    cbor_decref(&cbor);
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code = -10;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
                  }
                  break;
              }
              buffer_destroy(data);
              read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
            }

            DESTROY(wal, wal);
            if ((read_result != -3) || (cursor != (uint64_t)wal_size)) {
              DESTROY(index, index);
              *error_code = read_result;
              destroy_files(files);
              free(index_location);
              free(parent_location);
              return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
            }
          }

          index->is_rebuilding = 0;
          uint64_t current_id = (files->length - i) + last_id;
          char id[20];
          sprintf(id,"%lu", current_id);
          index->next_id = most_recent_id + 2;
          index->current_file = path_join(index->location, id);
          index->next_id = current_id + 1;
          sprintf(id,"%lu", current_id - 1 );
          index->last_file = path_join(index->location, id);
          index->max_snapshots = max_snapshots;
          index->max_wals = max_wals;
          uint64_t first_kept_id = _index_prune_old_snapshots(index);
          _index_prune_old_wals(index, first_kept_id);
          destroy_files(files);
          free(index_location);
          free(parent_location);
          return index;
        } else {
          index->max_snapshots = max_snapshots;
          index->max_wals = max_wals;
          uint64_t first_kept_id_b = _index_prune_old_snapshots(index);
          _index_prune_old_wals(index, first_kept_id_b);
          free(index_location);
          free(parent_location);
          destroy_files(files);
          return index;
        }
      }
    }
    log_warn("index_create: all %zu index files were invalid, creating empty index", files->length);
    destroy_files(files);
    free(index_location);
    free(parent_location);
    return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
  }
  destroy_files(files);
  free(index_location);
  free(parent_location);
  return _index_new_empty(bucket_size, location, wait, max_wait, most_recent_id, max_snapshots, max_wals);
}
index_t* index_create_from(size_t bucket_size, index_node_t* root, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals) {
  index_t* index = get_clear_memory(sizeof(index_t));
  index->bucket_size = bucket_size;
  index->location = path_join(location,"index");
  index->parent_location = strdup(location);
  vec_str_t* files = get_dir(index->location);
  uint64_t last_id = 0;
  if (files != NULL && files->length > 0) {
    char id[20];
    vec_sort(files, _sort_indexes);
    char* last = vec_last(files);
    char delims[] = "-";
    char* last_id_str = strtok(last,delims);
    last_id = strtoull(last_id_str, NULL, 10);
    index->next_id = last_id + 2;
    sprintf(id,"%lu", last_id + 1);
    index->current_file = path_join(index->location, id);
    index->last_file = path_join(index->location, last);
  } else {
    char id[20];
    sprintf(id,"%lu", last_id + 1);
    index->next_id = 2;
    index->current_file = path_join(index->location, id);
    index->last_file = NULL;
  }
  destroy_files(files);

  index->root = (index_node_t*) refcounter_reference((refcounter_t*) root);
  index->wal = wal_create(index->parent_location, last_id + 1);
  index->max_snapshots = max_snapshots;
  index->max_wals = max_wals;
  index->wait = wait;
  index->max_wait = max_wait;
  hashmap_init(&index->ranks, (void*)hash_uint32, (void*)compare_uint32);
  hashmap_set_key_alloc_funcs(&index->ranks, duplicate_uint32, (void*)free);
  refcounter_init((refcounter_t*) index);
  index_entry_vec_t* entries = index_to_array(index);
  for (int i = 0; i < entries->length; i++) {
    index_entry_t* entry = entries->data[i];
    uint32_t key = entry->counter.fib;
    index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
    if (rank == NULL) {
      rank = get_clear_memory(sizeof(index_entry_vec_t));
      vec_init(rank);
      vec_reserve(rank, 25);
      vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
      hashmap_put(&index->ranks, &key, rank);
    } else {
      vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
    }

    index_entry_destroy(entry);
  }
  vec_deinit(entries);
  free(entries);
  return index;
}
size_t index_node_count(index_node_t* node) {
  if (node == NULL) {
    return 0;
  }
  if (node->bucket == NULL) {
    return index_node_count(node->left) + index_node_count(node->right);
  } else {
    return node->bucket->length;
  }
}

void index_node_to_array(index_node_t* node, index_entry_vec_t* entries) {
  if (node == NULL) {
    return;
  }
  if(node->bucket == NULL) {
    index_node_to_array(node->left, entries);
    index_node_to_array(node->right, entries);
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      vec_push(entries, (index_entry_t*) refcounter_reference((refcounter_t*) cur_entry));
    }
  }
}

cbor_item_t* index_node_to_cbor(index_node_t* node) {
  if (node == NULL) {
    return NULL;
  }
  if (node->bucket == NULL) {
    cbor_item_t* cbor_left = index_node_to_cbor(node->left);
    if (cbor_left == NULL) {
      return NULL;
    }
    cbor_item_t* cbor_right = index_node_to_cbor(node->right);
    if (cbor_right == NULL) {
      return NULL;
    }
    cbor_item_t* array = cbor_new_definite_array(2);
    bool success = cbor_array_push(array, cbor_move(cbor_left));
    success &= cbor_array_push(array, cbor_move(cbor_right));
    if (!success) {
      cbor_decref(&array);
      return NULL;
    } else {
      return array;
    }
  } else {
    cbor_item_t* array = cbor_new_definite_array(1);
    cbor_item_t* bucket_array = cbor_new_definite_array(node->bucket->length);
    bool success = true;
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      success &= cbor_array_push(bucket_array, cbor_move(index_entry_to_cbor(cur_entry)));
    }
    success &= cbor_array_push(array, cbor_move(bucket_array));
    if (!success) {
      cbor_decref(&array);
      return NULL;
    } else {
      return array;
    }
  }
}

index_node_t* cbor_to_index_node(cbor_item_t* cbor, size_t bucket_size) {
  size_t size = cbor_array_size(cbor);
  if (size == 2) {
    cbor_item_t* cbor_left = cbor_array_get(cbor, 0);
    index_node_t* left = cbor_to_index_node(cbor_left, bucket_size);
    cbor_decref(&cbor_left);
    if (left == NULL) {
      return NULL;
    }
    cbor_item_t* cbor_right = cbor_array_get(cbor, 1);
    index_node_t* right = cbor_to_index_node(cbor_right, bucket_size);
    cbor_decref(&cbor_right);
    if (right == NULL) {
      return NULL;
    }
    refcounter_yield((refcounter_t*) left);
    refcounter_yield((refcounter_t*) right);
    index_node_t* node = index_node_create_from_leaves(left, right);
    return node;
  } else if (size == 1) {
    cbor_item_t* cbor_bucket = cbor_array_get(cbor, 0);
    index_node_t* node = index_node_create(bucket_size);
    size_t length = cbor_array_size(cbor_bucket);

    for (size_t i = 0; i < length; i++) {
      cbor_item_t* cbor_entry = cbor_array_get(cbor_bucket, i);
      index_entry_t* entry = cbor_to_index_entry(cbor_entry);
      cbor_decref(&cbor_entry);
      if (entry == NULL) {
        index_node_destroy(node);
        cbor_decref(&cbor_bucket);
        return NULL;
      }
      vec_push(node->bucket, entry);
    }
    cbor_decref(&cbor_bucket);
    return node;
  } else {
    return NULL;
  }
}

int _index_node_to_crc(index_node_t* node, XXH64_state_t* const state) {
  if (node == NULL) {
    return 0;
  }
  if (node->bucket == NULL) {
    int result = _index_node_to_crc(node->left, state);
    if (result != 0) {
      return result;
    } else {
      return _index_node_to_crc(node->right, state);
    }
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      if (XXH64_update(state, cur_entry->hash->data, cur_entry->hash->size) == XXH_ERROR) {
        log_error("failed to update crc with hash");
        return 3;
      }
      uint32_t fib = htobe32(cur_entry->counter.fib);
      if (XXH64_update(state, &fib, sizeof(uint32_t)) == XXH_ERROR) {
        log_error("failed to update crc with counter");
        return 4;
      }

      uint32_t count = htobe32(cur_entry->counter.count);
      if (XXH64_update(state, &count, sizeof(uint32_t)) == XXH_ERROR) {
        log_error("failed to update crc with counter");
        return 4;
      }

      uint64_t ejection_date  = htobe64(cur_entry->ejection_date);
      if (XXH64_update(state, &ejection_date, sizeof(uint64_t)) == XXH_ERROR) {
        log_error("failed to update crc with ejection date");
        return 5;
      }

      uint64_t section_index = htobe64((uint64_t)cur_entry->section_index);
      if (XXH64_update(state, &section_index, sizeof(uint64_t)) == XXH_ERROR) {
        log_error("failed to update crc with section index");
        return 6;
      }

      uint64_t section_id = htobe64((uint64_t)cur_entry->section_id);
      if (XXH64_update(state, &section_id, sizeof(uint64_t)) == XXH_ERROR) {
        log_error("failed to update crc with section id");
        return 7;
      }
    }
    return 0;
  }
}

void index_add(index_t* index, index_entry_t* entry) {
  index_add_to_node(index, entry, index->root, 0);
}

void index_add_to_node(index_t* index, index_entry_t* entry, index_node_t* node, size_t current) {
   if (node == NULL) {
     return;
   }
   if (node->bucket == NULL) {
     if (node->left == NULL || node->right == NULL) {
       log_error("index_add_to_node: branch node at depth %zu has NULL child (left=%p right=%p)",
                 current, (void*)node->left, (void*)node->right);
       return;
     }
     if (get_bit(entry->hash, current + 1)) {
       index_add_to_node(index, entry, node->right, current + 1);
     } else {
       index_add_to_node(index, entry, node->left, current + 1);
     }
   } else {
     for (int i = 0; i < node->bucket->length; i++) {
       index_entry_t* cur_entry = node->bucket->data[i];
       if (buffer_compare(cur_entry->hash, entry->hash) == 0) {
         if(!index->is_rebuilding) {
           _index_increment(index, cur_entry);
         }
         return;
       }
     }
     if (entry->counter.fib == 0 && entry->counter.count == 0) {
       uint32_t key = entry->counter.fib;
       index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
       if (rank == NULL) {
         rank = get_clear_memory(sizeof(index_entry_vec_t));
         vec_init(rank);
         vec_reserve(rank, 25);
         vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
         hashmap_put(&index->ranks, &key, rank);
       } else {
         vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
       }
     }

     if ((size_t)node->bucket->length < index->bucket_size) {
       if(!index->is_rebuilding) {
         cbor_item_t* cbor_entry = index_entry_to_cbor(entry);
         uint8_t* cbor_data;
         size_t cbor_size;
         cbor_serialize_alloc(cbor_entry, &cbor_data, &cbor_size);
         buffer_t *cbor_buf = buffer_create_from_existing_memory(cbor_data, cbor_size);
         wal_write(index->wal, addition, cbor_buf);
         buffer_destroy(cbor_buf);
         cbor_decref(&cbor_entry);
       }
       vec_push(node->bucket, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
     } else {
       index_split_node(index, node, current);
       index_add_to_node(index, entry, node, current);
     }
   }
}
void index_increment(index_t* index, index_entry_t* entry) {
  _index_increment(index, entry);
}

void _index_increment(index_t* index, index_entry_t* entry) {
  if(!index->is_rebuilding) {
    cbor_item_t *cbor_entry = index_entry_to_cbor(entry);
    uint8_t *cbor_data;
    size_t cbor_size;
    cbor_serialize_alloc(cbor_entry, &cbor_data, &cbor_size);
    buffer_t* cbor_buf = buffer_create_from_existing_memory(cbor_data, cbor_size);
    wal_write(index->wal, increment, cbor_buf);
    buffer_destroy(cbor_buf);
    cbor_decref(&cbor_entry);
  }

  if (index_entry_increment(entry)) {
    uint32_t key = entry->counter.fib - 1;
    index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
     if (rank != NULL) {
       for (int i = 0; i < rank->length; i++) {
         if (buffer_compare(rank->data[i]->hash, entry->hash) == 0) {
           refcounter_yield((refcounter_t*) rank->data[i]);
           vec_splice(rank, i, 1);
           break;
         }
       }
     }
     key = entry->counter.fib;
     rank = hashmap_get(&index->ranks, &key);
     if (rank == NULL) {
       rank = get_clear_memory(sizeof(index_entry_vec_t));
       vec_init(rank);
       vec_reserve(rank, 25);
       vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
       hashmap_put(&index->ranks, &key, rank);
     } else {
       vec_push(rank, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
     }
  }
}



void index_split_node(index_t* index, index_node_t* node, size_t current) {
  node->left = index_node_create(index->bucket_size);
  node->right = index_node_create(index->bucket_size);
  index_entry_vec_t* bucket = node->bucket;
  size_t count = node->bucket->length;
  node->bucket = NULL;
  for (size_t i = 0; i < count; i++) {
    index_entry_t* entry = bucket->data[i];
    refcounter_yield((refcounter_t*) entry);
    index_add_to_node(index, entry, node, current);
  }
  vec_deinit(bucket);
  free(bucket);
}

index_entry_t* index_get(index_t* index, buffer_t* hash) {
  index_entry_t* entry;
  entry = index_get_from_node(index, hash, index->root, 0);
  if (entry != NULL) {
    entry = (index_entry_t*) refcounter_reference((refcounter_t*) entry);
    refcounter_yield((refcounter_t*) entry);
  }
  return entry;
}

index_entry_t* index_find(index_t* index, buffer_t* hash) {
  index_entry_t* entry;
  entry = index_find_in_node(index, hash, index->root, 0);
  if (entry != NULL) {
    entry = (index_entry_t*) refcounter_reference((refcounter_t*) entry);
    refcounter_yield((refcounter_t*) entry);
  }
  return entry;
}

index_entry_t* index_peek(index_t* index, buffer_t* hash) {
  if (index->root == NULL) {
    return NULL;
  }
  index_entry_t* entry = index_find_in_node(index, hash, index->root, 0);
  return entry;
}

index_entry_t* index_get_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current) {
  if (node == NULL) {
    return NULL;
  }
  if (node->bucket == NULL) {
    if (node->left == NULL || node->right == NULL) {
      log_error("index_get_from_node: branch node at depth %zu has NULL child (left=%p right=%p)",
                current, (void*)node->left, (void*)node->right);
      return NULL;
    }
    if (get_bit(hash, current + 1)) {
      return index_get_from_node(index, hash, node->right, current + 1);
    } else {
      return index_get_from_node(index, hash, node->left, current + 1);
    }
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      if (buffer_compare(cur_entry->hash, hash) == 0) {
        _index_increment(index, cur_entry);
        return cur_entry;
      }
    }
    return NULL;
  }
}

index_entry_t* index_find_in_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current) {
  if (node == NULL) {
    return NULL;
  }
  if (node->bucket == NULL) {
    if (node->left == NULL || node->right == NULL) {
      log_error("index_find_in_node: branch node at depth %zu has NULL child (left=%p right=%p)",
                current, (void*)node->left, (void*)node->right);
      return NULL;
    }
    if (get_bit(hash, current + 1)) {
      return index_find_in_node(index, hash, node->right, current + 1);
    } else {
      return index_find_in_node(index, hash, node->left, current + 1);
    }
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      if (buffer_compare(cur_entry->hash, hash) == 0) {
        return cur_entry;
      }
    }
    return NULL;
  }
}

void index_remove(index_t* index, buffer_t* hash) {
  index_remove_from_node(index, hash, index->root, 0);
}
void index_remove_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current) {
  if (node == NULL) {
    return;
  }
  if (node->bucket == NULL) {
    if (node->left == NULL || node->right == NULL) {
      log_error("index_remove_from_node: branch node at depth %zu has NULL child (left=%p right=%p)",
                current, (void*)node->left, (void*)node->right);
      return;
    }
    if (get_bit(hash, current + 1)) {
      index_remove_from_node(index, hash, node->right, current + 1);
    } else {
      index_remove_from_node(index, hash, node->left, current + 1);
    }
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      if (buffer_compare(cur_entry->hash, hash) == 0) {
        if (!index->is_rebuilding) {
          cbor_item_t *cbor_hash = cbor_build_bytestring(cur_entry->hash->data, cur_entry->hash->size);
          uint8_t *cbor_data;
          size_t cbor_size;
          cbor_serialize_alloc(cbor_hash, &cbor_data, &cbor_size);
          buffer_t *cbor_buf = buffer_create_from_existing_memory(cbor_data, cbor_size);
          wal_write(index->wal, removal, cbor_buf);
          buffer_destroy(cbor_buf);
          cbor_decref(&cbor_hash);
        }
        vec_splice(node->bucket, i, 1);

        uint32_t key = cur_entry->counter.fib;
        index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
        if (rank != NULL) {
          size_t length = rank->length;
          for (size_t j = 0; j < length; j++) {
            if (buffer_compare(cur_entry->hash, rank->data[j]->hash) == 0) {
              index_entry_destroy(rank->data[j]);
              vec_splice(rank, j, 1);
              break;
            }
          }
        }
        index_entry_destroy(cur_entry);
        break;
      }
    }
  }
}
void index_destroy(index_t* index) {
  if (index == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*) index)) {
    index_debounce(index);
    refcounter_destroy_lock((refcounter_t*) index);
    index_destroy_node(index, index->root);
    index_entry_vec_t *rank;
    PLATFORM_DIAGNOSTIC_PUSH
    PLATFORM_DIAGNOSTIC_IGNORE(-Wmissing-field-initializers)
    hashmap_foreach_data(rank, &index->ranks) {
      for (int i = 0; i < rank->length; i++) {
        index_entry_destroy(rank->data[i]);
      }
      vec_deinit(rank);
      free(rank);
    }
    PLATFORM_DIAGNOSTIC_POP
    hashmap_cleanup(&index->ranks);
    wal_destroy(index->wal);
    free(index->location);
    free(index->current_file);
    free(index->parent_location);
    if (index->last_file != NULL) {
      free(index->last_file);
    }
    /* Fill freed memory with poison pattern to detect use-after-free */
    memset(index, 0xDD, sizeof(index_t));
    free(index);
  }
}
void index_destroy_node(index_t* index, index_node_t* node) {
  if (node->bucket == NULL) {
    index_destroy_node(index, node->left);
    index_destroy_node(index, node->right);
  } else {
    for (int i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      index_entry_destroy(cur_entry);
    }
    vec_deinit(node->bucket);
    free(node->bucket);
  }
  free(node);
}

size_t index_count(index_t* index) {
  size_t count = _index_count(index);
  return count;
}

size_t _index_count(index_t* index) {
  return index_node_count(index->root);
}

index_entry_vec_t* index_to_array(index_t* index) {
 index_entry_vec_t* entries = get_clear_memory(sizeof(index_entry_vec_t));
 vec_init(entries);
 vec_reserve(entries, _index_count(index));
 index_node_to_array(index->root, entries);
 return entries;
}

cbor_item_t* index_to_cbor(index_t* index) {
  cbor_item_t* array = _index_to_cbor(index);
  return array;
}
cbor_item_t* _index_to_cbor(index_t* index) {
  cbor_item_t* array = cbor_new_definite_array(3);
  bool success = cbor_array_push(array, cbor_move(index_node_to_cbor(index->root)));
  success &= cbor_array_push(array, cbor_move(cbor_build_uint64(index->bucket_size)));
  if (!success) {
    cbor_decref(&array);
    return NULL;
  }
  return array;
}


index_t* cbor_to_index(cbor_item_t* cbor, char* location, uint64_t wait, uint64_t max_wait, size_t max_snapshots, size_t max_wals) {
  cbor_item_t* cbor_root = cbor_array_get(cbor, 0);
  cbor_item_t* cbor_bucket_size = cbor_array_get(cbor, 1);
  size_t bucket_size = cbor_get_int(cbor_bucket_size);
  cbor_decref(&cbor_bucket_size);
  index_node_t* root = cbor_to_index_node(cbor_root, bucket_size);
  cbor_decref(&cbor_root);
  if (root == NULL) {
    return NULL;
  }
  index_t* index= index_create_from(bucket_size, root, location, wait, max_wait, max_snapshots, max_wals);
  return index;
}
void index_set_entry_ejection(index_t* index, index_entry_t* entry, uint64_t date) {
  if (!index->is_rebuilding) {
    cbor_item_t *array = cbor_new_definite_array(2);
    bool success = cbor_array_push(array, cbor_move(cbor_build_bytestring(entry->hash->data, entry->hash->size)));
    success &= cbor_array_push(array, cbor_move(cbor_build_uint64(date)));
    if (success) {
      uint8_t *cbor_data;
      size_t cbor_size;
      cbor_serialize_alloc(array, &cbor_data, &cbor_size);
      buffer_t *cbor_buf = buffer_create_from_existing_memory(cbor_data, cbor_size);
      wal_write(index->wal, ejection, cbor_buf);
      buffer_destroy(cbor_buf);
      cbor_decref(&array);
    } else {
      log_error("Failed to commit ejection date to log");
    }
  }
  index_entry_set_ejection_date(entry, date);
}

static uint64_t _index_prune_old_snapshots(index_t* index) {
  if (index->max_snapshots == 0) return 0;

  vec_str_t* files = get_dir(index->location);
  if (files == NULL || files->length == 0) {
    if (files) destroy_files(files);
    return 0;
  }

  vec_sort(files, _sort_indexes);

  size_t delete_count = (size_t)files->length > index->max_snapshots
                      ? (size_t)files->length - index->max_snapshots
                      : 0;

  uint64_t first_kept_id = 0;
  for (size_t i = 0; i < delete_count; i++) {
    char* filepath = path_join(index->location, files->data[i]);
    if (platform_file_unlink(filepath) != 0) {
      log_warn("Failed to delete old index snapshot %s: %s", filepath, strerror(errno));
    }
    free(filepath);
  }

  if (delete_count < (size_t)files->length) {
    uint64_t crc;
    _index_get_id_crc(files->data[delete_count], &first_kept_id, &crc);
  }

  destroy_files(files);
  return first_kept_id;
}

static void _index_prune_old_wals(index_t* index, uint64_t first_kept_id) {
  if (index->max_wals == 0) return;

  char* wal_dir = path_join(index->parent_location, "wal");
  vec_str_t* files = get_dir(wal_dir);
  if (files == NULL) {
    free(wal_dir);
    return;
  }

  vec_sort(files, _sort_indexes);

  for (int i = 0; i < files->length; i++) {
    uint64_t wal_id = strtoull(files->data[i], NULL, 10);
    if (wal_id < first_kept_id) {
      char* filepath = path_join(wal_dir, files->data[i]);
      if (platform_file_unlink(filepath) != 0) {
        log_warn("Failed to delete old WAL %s: %s", filepath, strerror(errno));
      }
      free(filepath);
    }
  }

  destroy_files(files);
  free(wal_dir);
}

void index_debounce(index_t* index) {
  cbor_item_t *cbor = _index_to_cbor(index);
  uint64_t crc = 0;
  int result = _index_to_crc(index, &crc);
  char* file = malloc(strlen(index->current_file) + 22);
  if (file == NULL) {
    log_error("index_debounce: out of memory allocating snapshot filename");
    cbor_intermediate_decref(cbor);
    return;
  }
  if (result == 0) {
    sprintf(file, "%s-%llu", index->current_file, (unsigned long long)crc);
  } else {
    log_error("Could not store index with correct crc");
    sprintf(file, "%s-crc_error", index->current_file);
  }

  if (index->last_file != NULL) {
    free(index->last_file);
  }
  index->last_file = index->current_file;

  char id[20];
  sprintf(id, "%lu", index->next_id);
  index->current_file = path_join(index->location, id);
  index->next_id++;
  wal_t* wal = index->wal;
  index->wal = wal_create_next(index->parent_location, wal->next_id, wal->last_file);

  uint8_t *cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
  platform_file_t* index_file = platform_file_open(file, PLATFORM_O_WRONLY | PLATFORM_O_CREAT, 0644);
  platform_file_write(index_file, cbor_data, cbor_size);
  platform_file_close(index_file);
  free(cbor_data);
  free(file);
  wal_destroy(wal);
  cbor_intermediate_decref(cbor);
  uint64_t first_kept_id = _index_prune_old_snapshots(index);
  _index_prune_old_wals(index, first_kept_id);
}
int index_sync(index_t* index) {
  if (index == NULL) return -1;
  return wal_sync(index->wal);
}
int _index_to_crc(index_t* index, uint64_t* crc) {
  XXH64_state_t* const state = XXH64_createState();
  if (state == NULL) {
    log_error("failed to create crc");
    return 1;
  }
  if (XXH64_reset(state, 0) == XXH_ERROR) {
    log_error("failed to init crc");
    return 2;
  }
  int result = _index_node_to_crc(index->root, state);
  if (result != 0) {
    XXH64_freeState(state);
    return result;
  }
  *crc = XXH64_digest(state);
  XXH64_freeState(state);
  return 0;
}

int index_to_crc(index_t* index, uint64_t* crc) {
  int result = _index_to_crc(index, crc);
  return result;
}
int _index_get_id_crc(char* filename, uint64_t* id, uint64_t* crc) {
  char* _filename = strdup(filename);
  char delims[] = "-";
  char* id_str = strtok(_filename,delims);
  *id = strtoull(id_str, NULL, 10);
  char* crc_str = strtok(NULL, delims);
  if (strcmp(crc_str, "crc_error") == 0) {
    free(_filename);
    return 1;
  } else {
    *crc = strtoull(crc_str, NULL, 10);
    free(_filename);
    return 0;
  }
}

static int _compare_ejection_date(const void* left, const void* right) {
  const index_entry_t* entry_left = *(const index_entry_t**)left;
  const index_entry_t* entry_right = *(const index_entry_t**)right;
  uint64_t date_left = entry_left->ejection_date;
  uint64_t date_right = entry_right->ejection_date;
  if (date_left == 0 && date_right == 0) return 0;
  if (date_left == 0) return 1;   /* never ejected → last */
  if (date_right == 0) return -1;
  if (date_left < date_right) return -1;
  if (date_left > date_right) return 1;
  return 0;
}

index_entry_vec_t* index_entries_by_ejection_date(index_t* index) {
  index_entry_vec_t* entries = index_to_array(index);
  if (entries != NULL && entries->length > 0) {
    vec_sort(entries, _compare_ejection_date);
  }
  return entries;
}

int _sort_indexes( const void *str1, const void *str2 ){
  char *const *pp1 = str1;
  char *const *pp2 = str2;
  char* cp1 = strdup(*pp1);
  char* cp2 = strdup(*pp2);
  char delims[] = "-";
  char* id_str1 = strtok(cp1,delims);
  char* id_str2 = strtok(cp2,delims);
  uint64_t id1 = strtoull(id_str1, NULL, 10);
  uint64_t id2 = strtoull(id_str2, NULL, 10);
  free(cp1);
  free(cp2);
  if (id1 < id2) {
    return -1;
  } else if (id1 > id2) {
    return 1;
  } else {
    return 0;
  }
}