//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_BUFFER_H
#define LIBOFFS_BUFFER_H
struct buffer_t;
typedef struct buffer_t buffer_t;


buffer_t* buffer_create(size_t size);
buffer_t* buffer_create_from_pointer(uint8_t* data, size_t size);
buffer_t* buffer_create_from_existing_memory(uint8_t* data, size_t size);
void buffer_copy_from_pointer(buffer_t* buf, uint8_t* data, size_t size);
void buffer_destroy(buffer_t* buf);
uint8_t buffer_index(buffer_t* buf, size_t index);
uint8_t buffer_set_index(buffer_t* buf, size_t index, uint8_t value);
buffer_t* buffer_xor(buffer_t* buf1, buffer_t* buf2);


#endif //LIBOFFS_BUFFER_H
