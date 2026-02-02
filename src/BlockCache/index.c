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
#include <fcntl.h>
#include <unistd.h>
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
void index_debounce(void* ctx);
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
  platform_lock_init(&entry->lock);
  entry->counter = fibonacci_hit_counter_create();
  entry->hash = (buffer_t*) refcounter_reference( (refcounter_t*) hash);
  return entry;
}

index_entry_t* index_entry_from(buffer_t* hash, size_t section_id, size_t section_index, uint64_t ejection_date, fibonacci_hit_counter_t counter) {
  index_entry_t* entry = get_clear_memory(sizeof(index_entry_t));
  refcounter_init((refcounter_t*) entry);
  platform_lock_init(&entry->lock);
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
    platform_lock_destroy(&entry->lock);
    buffer_destroy(entry->hash);
    free(entry);
  }
}

int index_entry_increment(index_entry_t* entry) {
  int promoted;
  platform_lock(&entry->lock);
  promoted = fibonacci_hit_counter_increment(&entry->counter);
  platform_unlock(&entry->lock);
  return promoted;
}

void index_entry_set_ejection_date(index_entry_t* entry, uint64_t ejection_date) {
  platform_lock(&entry->lock);
  entry->ejection_date = ejection_date;
  platform_unlock(&entry->lock);
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
 fibonacci_hit_counter_t counter = cbor_to_fibonacci_hit_counter(cbor_move(cbor_array_get(cbor, 0)));
 buffer_t* hash= cbor_to_buffer(cbor_move(cbor_array_get(cbor, 1)));
 size_t sectionId = (size_t) cbor_get_uint64(cbor_move(cbor_array_get(cbor, 2)));
 size_t section_index = (size_t) cbor_get_uint64(cbor_move(cbor_array_get(cbor, 3)));
 uint64_t ejection_date = (size_t) cbor_get_uint64(cbor_move(cbor_array_get(cbor,4)));
 refcounter_yield((refcounter_t*) hash);
 return index_entry_from(hash, sectionId, section_index, ejection_date, counter);
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
    for (size_t i = 0; i < node->bucket->length; i++) {
      index_entry_destroy(node->bucket->data[i]);
    }
    free(node->bucket);
    free(node);
  }
}

index_t* _index_new_empty(size_t bucket_size, char* location, hierarchical_timing_wheel_t* wheel, uint64_t wait, uint64_t max_wait, uint64_t most_recent_id) {
  index_t* index = get_clear_memory(sizeof(index_t));
  platform_lock_init(&index->lock);
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
  index->debouncer = debouncer_create(wheel, index, index_debounce, index_debounce, wait, max_wait);
  hashmap_init(&index->ranks, (void*)hash_uint32, (void*)compare_uint32);
  hashmap_set_key_alloc_funcs(&index->ranks, duplicate_uint32, (void*)free);
  refcounter_init((refcounter_t*) index);
  return index;
}

index_t* index_create(size_t bucket_size, char* location, hierarchical_timing_wheel_t* wheel, uint64_t wait, uint64_t max_wait, int* error_code) {
  *error_code = 0;
  index_t* index;
  char* index_location = path_join(location,"index");
  char* parent_location = strdup(location);
  mkdir_p(index_location);
  vec_str_t* files = get_dir(index_location);
  uint64_t most_recent_id = 0;

  if (files->length > 0) {
    vec_sort(files, _sort_indexes);
    char id[20];
    for (size_t i = files->length - 1; i >= 0; i--) { //loop through index files to find first valid file
      //Get index's crc
      char* last = files->data[i];
      uint64_t last_id = 0;
      uint64_t last_crc = 0;
      if (_index_get_id_crc(last, &last_id, &last_crc) == 1) {
        log_error("index file %lu invalid", i);
        continue;
      }
      char* index_file_location = path_join(index_location, last);

#ifdef _WIN32
      int32_t index_fd = open(index_file_location, O_RDWR | O_CREAT, 0644);
#else
      int32_t index_fd = open(index_file_location, O_RDWR | O_CREAT, 0644);
#endif
      free(index_file_location);
      int32_t size = lseek(index_fd, 0,SEEK_END);
      if(size < 0) {
        log_error("index file %lu empty", i);
        *error_code= -1;
        continue;
      }
      if (lseek(index_fd, 0, SEEK_SET) < 0) {
        log_error("index file %lu failed to seek start", i);
        *error_code= -2;
        continue;
      }
      uint8_t buffer[size];
      size_t bytes = read(index_fd, buffer, size);

      if (size != bytes) {
        log_error("index file %lu failed to read file", i);
        *error_code= -3;
        continue;
      }
      struct cbor_load_result result;

      cbor_item_t* cbor = cbor_load(buffer, size, &result);

      if (result.error.code != CBOR_ERR_NONE) {
        *error_code= -4;
        log_error("index file %lu failed to load CBOR", i);
        continue;
      }
      if(!cbor_isa_array(cbor)) {
        cbor_decref(&cbor);
        *error_code= -5;
        log_error("index file %lu CBOR is invalid", i);
        continue;
      }
      index = cbor_to_index(cbor, location, wheel, wait, max_wait);
      cbor_decref(&cbor);
      uint64_t crc;
      _index_to_crc(index, &crc);
      if (crc != last_crc) { //Index is invalid, continue to iterate backward until we have a valid index
        DESTROY(index, index);
        continue;
      } else { // Index is valid rebuild if it is not the most recent index
        if (i != (files->length - 1)) { //incorporate every wal's changes until we get to the most recent index
          index->is_rebuilding = 1;
          for (size_t j = i + 1; j < files->length; j++) {
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
            while ((read_result == 0) && (cursor <= wal_size)) {
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
                    return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
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
                    return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
                  }
                  break;
                case 'e':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    if (cbor_isa_array(cbor)) {
                      cbor_item_t* cbor_hash = cbor_move(cbor_array_get(cbor, 0));
                      cbor_item_t* cbor_date = cbor_move(cbor_array_get(cbor,1));
                      if (cbor_isa_bytestring(cbor_hash) && cbor_isa_uint(cbor_date)) {
                        buffer_t* hash = cbor_to_buffer(cbor_move(cbor_hash));
                        index_entry_t* entry = index_find(index, hash);
                        index_entry_set_ejection_date(entry, cbor_get_uint64(cbor_date));
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
                        return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
                      }
                    }
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code = -8;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
                  }
                  break;
                case 'r':
                  cbor = cbor_load(data->data, data->size, &result);
                  if (result.error.code == CBOR_ERR_NONE) {
                    if (cbor_isa_bytestring(cbor)) {
                      buffer_t* hash = cbor_to_buffer(cbor_move(cbor));
                      index_remove(index, hash);
                    } else {
                      cbor_decref(&cbor);
                      DESTROY(index, index);
                      *error_code = -9;
                      free(index_location);
                      free(parent_location);
                      destroy_files(files);
                      return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
                    }
                    cbor_decref(&cbor);
                  } else {
                    cbor_decref(&cbor);
                    DESTROY(index, index);
                    *error_code = -10;
                    free(index_location);
                    free(parent_location);
                    destroy_files(files);
                    return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
                  }
                  break;
              }
              buffer_destroy(data);
              read_result = wal_read(wal, &type, &data, &cursor, &wal_size);
            }

            DESTROY(wal, wal);
            if ((read_result != -3) || (cursor != wal_size)) { // some error other than end of file
              DESTROY(index, index);
              *error_code = read_result;
              free(index_location);
              free(parent_location);
              return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
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
          destroy_files(files);
          free(index_location);
          free(parent_location);
          return index;
        } else {
          free(index_location);
          free(parent_location);
          destroy_files(files);
          return index;
        }
      }
    }
  } else {
    destroy_files(files);
    free(index_location);
    free(parent_location);
    return _index_new_empty(bucket_size, location, wheel, wait, max_wait, most_recent_id);
  }
}
index_t* index_create_from(size_t bucket_size, index_node_t* root, char* location, hierarchical_timing_wheel_t* wheel, uint64_t wait, uint64_t max_wait) {
  index_t* index = get_clear_memory(sizeof(index_t));
  index->bucket_size = bucket_size;
  index->location = path_join(location,"index");
  index->parent_location = strdup(location);
  vec_str_t* files = get_dir(index->location);
  uint64_t last_id = 0 ;
  if (files->length > 0) {
    char id[20];
    vec_sort(files, _sort_indexes);
    char* last = vec_last(files);
    char delims[] = "-";
    char* last_id_str = strtok(last,delims);
    last_id = strtoull(last_id_str, NULL, 10);
    index->next_id = last_id + 2; // TODO Handle integer rollover
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
  index->debouncer = debouncer_create(wheel, index, index_debounce, index_debounce, wait, max_wait);
  hashmap_init(&index->ranks, (void*)hash_uint32, (void*)compare_uint32);
  hashmap_set_key_alloc_funcs(&index->ranks, duplicate_uint32, (void*)free);
  refcounter_init((refcounter_t*) index);
  index_entry_vec_t* entries = index_to_array(index);
  for (size_t i = 0; i < entries->length; i++) {
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
/*
int index_check_integrity(index_t* index, wal_t* wal) {
 uint64_t cursor = 0;
 buffer_t* log_entry;
 wal_type_e type;
 int result = 0;
 int32_t wal_size;
 int read_result = wal_read(wal, &type, log_entry, &cursor, &wal_size);
 do {
   cbor_item_t* cbor;
   struct cbor_load_result cbor_result;
   cbor = cbor_load(log_entry->data, log_entry->size, &cbor_result);
   if (cbor_result.error.code != CBOR_ERR_NONE) {
     return 1;
   }
   switch(type) {
     case 'a':
       index_entry_t* addition_entry = cbor_to_index_entry(cbor);
       index_entry_t* found = index_find(index, addition_entry->hash);
       if ((found == NULL) || (buffer_compare(addition_entry->hash, found->hash))) {
         result = 2;
       }
       index_entry_destroy(addition_entry);
       if (found != NULL) {
         index_entry_destroy(found);
       }
       break;
     case 'r':
       buffer_t* hash = cbor_to_buffer(cbor);
       index_entry_t* removed = index_find(index, hash);
       if (removed != NULL) {
         result = 3;
         DESTROY(removed, index_entry);
       }
       DESTROY(hash, buffer);
       break;
     case 'i':
       index_entry_t* inc_entry = cbor_to_index_entry(cbor);
       index_entry_t* incremented = index_find(index, inc_entry->hash);
       if (incremented == NULL) {
         result = 4;
       }
       if (fibonacci_hit_counter_compare(&incremented->counter, &inc_entry->counter) == -1) {
          result = 5;
       }
       index_entry_destroy(inc_entry);
       if (incremented != NULL) {
         index_entry_destroy(incremented);
       }
       break;
     case 'e':
       break;
   }
   cbor_decref(&cbor);
   if (result != 0) {
     return result;
   }
 } while()

}*/
size_t index_node_count(index_node_t* node) {
  if (node->bucket == NULL) {
    return index_node_count(node->left) + index_node_count(node->right);
  } else {
    return node->bucket->length;
  }
}

void index_node_to_array(index_node_t* node, index_entry_vec_t* entries) {
  if(node->bucket == NULL) {
    index_node_to_array(node->left, entries);
    index_node_to_array(node->right, entries);
  } else {
    for (size_t i =0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      vec_push(entries, (index_entry_t*) refcounter_reference((refcounter_t*) cur_entry));
    }
  }
}

cbor_item_t* index_node_to_cbor(index_node_t* node) {
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
    for (size_t i = 0; i < node->bucket->length; i++) {
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
    cbor_item_t* cbor_left = cbor_move(cbor_array_get(cbor,0));
    index_node_t* left = cbor_to_index_node(cbor_left, bucket_size);
    if (left == NULL) {
      return NULL;
    }
    cbor_item_t* cbor_right = cbor_move(cbor_array_get(cbor, 1));
    index_node_t* right = cbor_to_index_node(cbor_right, bucket_size);
    if (right == NULL) {
      return NULL;
    }
    refcounter_yield((refcounter_t*) left);
    refcounter_yield((refcounter_t*) right);
    index_node_t* node = index_node_create_from_leaves(left, right);
    return node;
  } else if (size == 1) {
    cbor_item_t* cbor_bucket = cbor_move(cbor_array_get(cbor, 0));
    index_node_t* node = index_node_create(bucket_size);
    size_t length = cbor_array_size(cbor_bucket);

    for (size_t i = 0; i < length; i++) {
      cbor_item_t* cbor_entry = cbor_move(cbor_array_get(cbor_bucket, i));
      index_entry_t* entry = cbor_to_index_entry(cbor_entry);
      if (entry == NULL) {
        index_node_destroy(node);
        return NULL;
      }
      vec_push(node->bucket, entry);
    }
    return node;
  } else {
    return NULL;
  }
}

int _index_node_to_crc(index_node_t* node, XXH64_state_t* const state) {
  if (node->bucket == NULL) {
    int result = _index_node_to_crc(node->left, state);
    if (result != 0) {
      return result;
    } else {
      return _index_node_to_crc(node->right, state);
    }
  } else {
    for (size_t i =0; i < node->bucket->length; i++) {
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
  platform_lock(&index->lock);
  index_add_to_node(index, entry, index->root, 0);
  platform_unlock(&index->lock);
}

void index_add_to_node(index_t* index, index_entry_t* entry, index_node_t* node, size_t current) {
   if (node->bucket == NULL) {
     if (get_bit(entry->hash, current + 1)) {
       index_add_to_node(index, entry, node->right, current + 1);
     } else {
       index_add_to_node(index, entry, node->left, current + 1);
     }
   } else {
     for (size_t i = 0; i < node->bucket->length; i++) {
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

     if (node->bucket->length < index->bucket_size) {
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
       if(!index->is_rebuilding) {
         debouncer_debounce(index->debouncer);
       }
     } else {
       index_split_node(index, node, current);
       index_add_to_node(index, entry, node, current);
     }
   }
}
void index_increment(index_t* index, index_entry_t* entry) {
  platform_lock(&index->lock);
  _index_increment(index, entry);
  platform_unlock(&index->lock);
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
    if (!index->is_rebuilding) {
       debouncer_debounce(index->debouncer);
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
  platform_lock(&index->lock);
  entry = index_get_from_node(index, hash, index->root, 0);
  if (entry != NULL) {
    entry = (index_entry_t*) refcounter_reference((refcounter_t*) entry);
    refcounter_yield((refcounter_t*) entry);
  }
  platform_unlock(&index->lock);
  return entry;
}

index_entry_t* index_find(index_t* index, buffer_t* hash) {
  index_entry_t* entry;
  platform_lock(&index->lock);
  entry = index_find_in_node(index, hash, index->root, 0);
  if (entry != NULL) {
    entry = (index_entry_t*) refcounter_reference((refcounter_t*) entry);
    refcounter_yield((refcounter_t*) entry);
  }
  platform_unlock(&index->lock);
  return entry;
}

index_entry_t* index_get_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current) {
  if (node->bucket == NULL) {
    if (get_bit(hash, current + 1)) {
      return index_get_from_node(index, hash, node->right, current + 1);
    } else {
      return index_get_from_node(index, hash, node->left, current + 1);
    }
  } else {
    for (size_t i = 0; i < node->bucket->length; i++) {
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
  if (node->bucket == NULL) {
    if (get_bit(hash, current + 1)) {
      return index_find_in_node(index, hash, node->right, current + 1);
    } else {
      return index_find_in_node(index, hash, node->left, current + 1);
    }
  } else {
    for (size_t i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      if (buffer_compare(cur_entry->hash, hash) == 0) {
        return cur_entry;
      }
    }
    return NULL;
  }
}

void index_remove(index_t* index, buffer_t* hash) {
  platform_lock(&index->lock);
  index_remove_from_node(index, hash, index->root, 0);
  platform_unlock(&index->lock);
}
void index_remove_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current) {
  if (node->bucket == NULL) {
    if (get_bit(hash, current + 1)) {
      index_remove_from_node(index, hash, node->right, current + 1);
    } else {
      index_remove_from_node(index, hash, node->left, current + 1);
    }
  } else {
    for (size_t i = 0; i < node->bucket->length; i++) {
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
  refcounter_dereference((refcounter_t*) index);
  if (refcounter_count((refcounter_t*) index) == 0) {
    debouncer_flush(index->debouncer);
    refcounter_destroy_lock((refcounter_t*) index);
    platform_lock_destroy(&index->lock);
    index_destroy_node(index, index->root);
    index_entry_vec_t *rank;
    hashmap_foreach_data(rank, &index->ranks) {
      for (size_t i = 0; i < rank->length; i++) {
        index_entry_destroy(rank->data[i]);
      }
      vec_deinit(rank);
      free(rank);
    }
    hashmap_cleanup(&index->ranks);
    wal_destroy(index->wal);
    free(index->location);
    free(index->current_file);
    free(index->parent_location);
    if (index->last_file != NULL) {
      free(index->last_file);
    }
    debouncer_destroy(index->debouncer);
    free(index);
  }
}
void index_destroy_node(index_t* index, index_node_t* node) {
  if (node->bucket == NULL) {
    index_destroy_node(index, node->left);
    index_destroy_node(index, node->right);
  } else {
    for (size_t i = 0; i < node->bucket->length; i++) {
      index_entry_t* cur_entry = node->bucket->data[i];
      index_entry_destroy(cur_entry);
    }
    vec_deinit(node->bucket);
    free(node->bucket);
  }
  free(node);
}

size_t index_count(index_t* index) {
  platform_lock(&index->lock);
  size_t count = _index_count(index);
  platform_unlock(&index->lock);
  return count;
}

size_t _index_count(index_t* index) {
  return index_node_count(index->root);
}

index_entry_vec_t* index_to_array(index_t* index) {
 index_entry_vec_t* entries = get_clear_memory(sizeof(index_entry_vec_t));
 vec_init(entries);
 platform_lock(&index->lock);
 vec_reserve(entries, _index_count(index));
 index_node_to_array(index->root, entries);
 platform_unlock(&index->lock);
 return entries;
}

cbor_item_t* index_to_cbor(index_t* index) {
  platform_lock(&index->lock);
  cbor_item_t* array = _index_to_cbor(index);
  platform_unlock(&index->lock);
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


index_t* cbor_to_index(cbor_item_t* cbor, char* location, hierarchical_timing_wheel_t* wheel, uint64_t wait, uint64_t max_wait) {
  cbor_item_t* cbor_root = cbor_move(cbor_array_get(cbor,0));
  size_t bucket_size = cbor_get_uint64(cbor_move(cbor_array_get(cbor, 1)));
  index_node_t* root = cbor_to_index_node(cbor_root, bucket_size);
  if (root == NULL) {
    return NULL;
  }
  index_t* index= index_create_from(bucket_size, root, location, wheel, wait, max_wait);
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

void index_debounce(void* ctx) {
  index_t *index = (index_t *) ctx;
  platform_lock(&index->lock);
  cbor_item_t *cbor = _index_to_cbor(index);
  uint64_t crc = 0;
  int result = _index_to_crc(index, &crc);
  char file[strlen(index->current_file) + 22];
  if (result == 0) {
    sprintf(file, "%s-%lu", index->current_file, crc);
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
  platform_unlock(&index->lock);

  uint8_t *cbor_data;
  size_t cbor_size;
  cbor_serialize_alloc(cbor, &cbor_data, &cbor_size);
#ifdef _WIN32
  int index_file = open(file, _O_WRONLY | _O_BINARY | _O_CREAT, 0644);
#else
  int index_file = open(file, O_WRONLY | O_CREAT, 0644);
#endif

  write(index_file, cbor_data, cbor_size);
  close(index_file);
  free(cbor_data);
  wal_destroy(wal);
  cbor_intermediate_decref(cbor);
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
  platform_lock(&index->lock);
  int result = _index_to_crc(index, crc);
  platform_unlock(&index->lock);
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