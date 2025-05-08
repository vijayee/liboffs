//
// Created by victor on 4/29/25.
//
#include "index.h"
#include "../Util/allocator.h"
#include "../Util/hash.h"

void index_add_to_node(index_t* index, index_entry_t* entry, index_node_t* node, size_t current);
void index_split_node(index_t* index, index_node_t* node, size_t current);
void index_increment(index_t* index, index_entry_t* entry);
index_entry_t* index_get_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current);
void index_remove_from_node(index_t* index, buffer_t* hash, index_node_t* node, size_t current);
void index_destroy_node(index_t* index, index_node_t* node);

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

index_node_t* index_node_create(size_t bucket_size) {
  index_node_t* node = get_clear_memory(sizeof(index_node_t));
  refcounter_init((refcounter_t*) node);
  node->bucket = get_clear_memory(sizeof(index_entry_vec_t));
  vec_init(node->bucket);
  vec_reserve(node->bucket, bucket_size);
  node->left = NULL;
  node->right = NULL;
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

index_t* index_create(size_t bucket_size) {
  index_t* index = get_clear_memory(sizeof(index_t));
  index->bucket_size = bucket_size;
  index->root = index_node_create(bucket_size);
  hashmap_init(&index->ranks, (void*)hash_uint32, (void*)compare_uint32);
  hashmap_set_key_alloc_funcs(&index->ranks, duplicate_uint32, (void*)free);
  refcounter_init((refcounter_t*) index);
  return index;
}

void index_add(index_t* index, index_entry_t* entry) {
  index_add_to_node(index, entry, index->root, 0);
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
         index_increment(index, cur_entry);
         return;
       }
     }
     if (node->bucket->length < index->bucket_size) {
       vec_push(node->bucket, (index_entry_t*) refcounter_reference((refcounter_t*) entry));
     } else {
       index_split_node(index, node, current);
       index_add_to_node(index, entry, node, current);
     }
   }
}

void index_increment(index_t* index, index_entry_t* entry) {
  if (index_entry_increment(entry)) {
    uint32_t key = entry->counter.fib - 1;
    index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
     if (rank != NULL) {
       for (int i = 0; i < rank->length; i++) {
         if (buffer_compare(rank->data[i]->hash, entry->hash) == 0) {
           vec_splice(rank, i, 1);
           refcounter_yield((refcounter_t*) rank->data[i]);
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
  return index_get_from_node(index, hash, index->root, 0);
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
        index_increment(index, cur_entry);
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
        vec_splice(node->bucket, i, 1);
      }
      index_entry_destroy(cur_entry);

      uint32_t key = cur_entry->counter.fib;
      index_entry_vec_t* rank = hashmap_get(&index->ranks, &key);
      if (rank == NULL) {
        for (size_t j = 0; j < rank->length; i++) {
          if (buffer_compare(cur_entry->hash, node->bucket->data[j]->hash) == 0) {
            vec_splice(rank, j, 1);
            index_entry_destroy(node->bucket->data[j]);
            break;
          }
        }
      }
      break;
    }
  }
}
void index_destroy(index_t* index) {
  refcounter_dereference((refcounter_t*) index);
  if (refcounter_count((refcounter_t*) index) == 0) {
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