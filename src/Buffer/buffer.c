//
// Created by victor on 3/18/25.
//
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "buffer.h"
#include "../RefCounter/refcounter.h"
#include "../RefCounter/refcounter.p.h"
#include <string.h>


struct buffer_t {
  refcounter_t refcounter;
  uint8_t* data;
  size_t size;
};

buffer_t* buffer_create(size_t size) {
  buffer_t* buf = calloc(1, sizeof(buffer_t));
  buf->data = calloc(size, sizeof(uint8_t));
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return refcounter_reference((refcounter_t*) buf);
}

buffer_t* buffer_create_from_pointer(uint8_t* data, size_t size) {
  buffer_t* buf = malloc(sizeof(buffer_t));
  buf->data = data;
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return refcounter_reference((refcounter_t*) buf);
}

buffer_t* buffer_create_from_existing_memory(uint8_t* data, size_t size) {
  buffer_t* buf = calloc(1, sizeof(buffer_t));
  buf->data = calloc(size, sizeof(uint8_t));
  buf->size = size;
  refcounter_init((refcounter_t*) buf);
  return refcounter_reference((refcounter_t*) buf);
}

void buffer_copy_from_pointer(buffer_t* buf, uint8_t* data, size_t size) {
  if (buf->size > size) {
    memcpy(buf->data, data, size);
  } else {
    memcpy(buf->data, data, buf->size);
  }
}

void buffer_destroy(buffer_t* buf) {
  refcounter_dereference((refcounter_t*)buf);
  if (refcounter_count((refcounter_t*)buf) == 0) {
    refcounter_destroy_lock(&buf->refcounter);
    free(buf->data);
    free(buf);
  }
}

uint8_t buffer_index(buffer_t* buf, size_t index) {
  return buf->data[index];
}

uint8_t buffer_set_index(buffer_t* buf, size_t index, uint8_t value) {
  return buf->data[index] = value;
}

buffer_t* buffer_xor(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
   result->data[i] = buf1->data[i] ^ buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] ^ 0;
  }
  return result;
}

buffer_t* buffer_or(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
    result->data[i] = buf1->data[i] | buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] | 0;
  }
  return result;
}

buffer_t* buffer_and(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
    result->data[i] = buf1->data[i] & buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] & 0;
  }
  return result;
}