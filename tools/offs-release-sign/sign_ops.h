#ifndef OFFS_RELEASE_SIGN_OPS_H
#define OFFS_RELEASE_SIGN_OPS_H

#include <stddef.h>
#include <stdint.h>

/* Generate a new ed25519 keypair. Writes the private key PEM to priv_path and
 * the public key PEM to pub_path. Returns 0 on success, -1 on failure. */
int release_sign_keygen(const char* priv_path, const char* pub_path);

/* Sign a manifest file with an ed25519 private key (PEM). Writes the raw 64-byte
 * signature to <manifest_path>.sig. Returns 0 on success, -1 on failure. */
int release_sign_sign(const char* key_path, const char* manifest_path);

#endif /* OFFS_RELEASE_SIGN_OPS_H */