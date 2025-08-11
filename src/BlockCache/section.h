//
// Created by victor on 7/19/25.
//

#ifndef OFFS_SECTION_H
#define OFFS_SECTION_H
#include <stddef.h>
#include <stdio.h>
#include "../Time/debouncer.h"
#include "../RefCounter/refcounter.h"
#include "cbor.h"
#include "block.h"

typedef struct {
  size_t start;
  size_t end;
} fragment_t;

fragment_t* fragment_create(size_t start, size_t end);
void fragment_destroy(fragment_t* fragment);
cbor_item_t* fragment_to_cbor(fragment_t* fragment);
fragment_t* cbor_to_fragment(cbor_item_t* cbor);

typedef struct fragment_list_node_t fragment_list_node_t;

struct fragment_list_node_t {
  fragment_t* fragment;
  fragment_list_node_t* next;
  fragment_list_node_t* previous;
};

fragment_list_node_t* fragment_list_node_create(fragment_t* fragment, fragment_list_node_t* next, fragment_list_node_t* previous);

typedef struct {
  fragment_list_node_t* first;
  fragment_list_node_t* last;
  size_t count;
} fragment_list_t;

fragment_list_t* fragment_list_create();
void fragment_list_destroy(fragment_list_t* list);
void fragment_list_enqueue(fragment_list_t* list, fragment_t* fragment);
fragment_t* fragment_list_dequeue(fragment_list_t* list);
fragment_t* fragment_list_remove(fragment_list_t* list, fragment_list_node_t* node);
cbor_item_t* fragment_list_to_cbor(fragment_list_t* list);
fragment_list_t* cbor_to_fragment_list(cbor_item_t* cbor);


typedef struct {
  refcounter_t refcounter;
  PLATFORMRWLOCKTYPE(lock);
  FILE* file;
  size_t id;
  char* meta_path;
  char* path;
  fragment_list_t* fragments;
  size_t size;
  block_size_e block_size;
  debouncer_t* debouncer;
} section_t;

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, hierarchical_timing_wheel_t* wheel, uint64_t wait, uint64_t max_wait, block_size_e type);
void section_destroy(section_t* section);
int section_write(section_t* section, block_t* block, size_t* index);
buffer_t* section_read(section_t* section, size_t index);
int section_deallocate(section_t* section, size_t index);
#endif //OFFS_SECTION_H
