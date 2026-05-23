//
// Created by victor on 5/22/26.
//

#ifndef OFFS_PEER_VERIFY_H
#define OFFS_PEER_VERIFY_H

#include <stdint.h>
#include <stddef.h>

typedef struct peer_verify_ctx_t peer_verify_ctx_t;

// Create verification context from DER-encoded CA certificate bytes.
// Returns NULL if data is NULL, data_len is 0, or parsing fails.
peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len);

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx);

// Validate a peer certificate against the trusted CA store.
// certificate is a QUIC_CERTIFICATE* from MsQuic's CERTIFICATE_RECEIVED event.
// Returns 0 on success, -1 on failure.
int peer_verify_validate(peer_verify_ctx_t* ctx, void* certificate);

#endif // OFFS_PEER_VERIFY_H
