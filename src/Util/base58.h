//
// Created by victor on 5/8/25.
//

#ifndef OFFS_BASE58_H
#define OFFS_BASE58_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int base58_encode(const uint8_t* input, size_t input_len, char* output, size_t output_size);
int base58_decode(const char* input, uint8_t* output, size_t output_size, size_t* bytes_written);
size_t base58_encoded_length(size_t input_len);
size_t base58_decoded_length(size_t str_len);

#ifdef __cplusplus
}
#endif

#endif //OFFS_BASE58_H