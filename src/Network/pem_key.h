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

// Sign a 32-byte nonce with the private key loaded from key_path. Returns 0
// on success and writes a freshly-allocated signature buffer to *out_sig
// (caller frees with free()). Sets *out_sig_len to the signature length.
// Returns -1 on failure. Uses EVP_DigestSign (works for Ed25519, RSA, EC).
// See audit #8.
int pem_key_sign_nonce(const char* key_path, const uint8_t nonce[32],
                       uint8_t** out_sig, size_t* out_sig_len);

// Verify a signature of a 32-byte nonce under a public key (raw Ed25519/X25519
// bytes or DER SPKI — the same format pem_extract_public_key produces).
// Reconstructs the EVP_PKEY from the raw bytes (EVP_PKEY_new_raw_public_key)
// or DER (d2i_PUBKEY), then EVP_DigestVerify. Returns 0 if valid, -1 if
// invalid or on error. See audit #8.
int pem_key_verify_nonce(const uint8_t* public_key, size_t public_key_len,
                         const uint8_t nonce[32],
                         const uint8_t* signature, size_t signature_len);

#endif