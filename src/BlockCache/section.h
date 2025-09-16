//
// Created by victor on 7/19/25.
//

#ifndef OFFS_SECTION_H
#define OFFS_SECTION_H
#include <stddef.h>
#include <stdio.h>
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
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
  PLATFORMLOCKTYPE(lock);
  int fd;
  size_t id;
  char* meta_path;
  char* path;
  fragment_list_t* fragments;
  size_t size;
  block_size_e block_size;
} section_t;

section_t* section_create(char* path, char* meta_path, size_t size, size_t id, block_size_e type);
void section_destroy(section_t* section);
int section_write(section_t* section, buffer_t* data, size_t* index, uint8_t* full);
buffer_t* section_read(section_t* section, size_t index);
int section_deallocate(section_t* section, size_t index);
uint8_t section_full(section_t* section);
#endif //OFFS_SECTION_H
