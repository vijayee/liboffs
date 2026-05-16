//
// Created by victor on 5/16/26.
//

#ifndef OFFS_PEM_KEY_H
#define OFFS_PEM_KEY_H

#include <stdint.h>
#include <stddef.h>

// Extract the raw public key bytes from a PEM certificate file.
// Returns allocated buffer with key bytes, caller must free().
// Sets *out_len to the number of bytes returned.
// Returns NULL on error.
uint8_t* pem_extract_public_key(const char* cert_path, size_t* out_len);

#endif