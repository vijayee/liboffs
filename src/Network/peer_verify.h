//
// Created by victor on 5/22/26.
//

#ifndef OFFS_PEER_VERIFY_H
#define OFFS_PEER_VERIFY_H

#include <stdint.h>
#include <stddef.h>

typedef struct peer_verify_ctx_t peer_verify_ctx_t;

// Create a verification context from DER-encoded CA certificate bytes.
// Converts DER to PEM and writes it to a temporary file for MsQuic's
// QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE credential config.
// Returns NULL if data is NULL, data_len is 0, or DER parsing fails.
peer_verify_ctx_t* peer_verify_ctx_create(const uint8_t* ca_cert_data, size_t ca_cert_len);

// Create a verification context from a PEM-encoded CA certificate file.
// Reads the file, parses the PEM, and delegates to peer_verify_ctx_create
// after converting to DER. Returns NULL if pem_path is NULL, the file
// cannot be opened/parsed, or DER conversion fails. This is the file-based
// counterpart to peer_verify_ctx_create — callers that hold a PEM CA path
// (e.g., relay_server_main, wt_transport, offs_client) use this helper
// instead of having to read+convert the file themselves.
peer_verify_ctx_t* peer_verify_ctx_create_from_pem_file(const char* pem_path);

void peer_verify_ctx_destroy(peer_verify_ctx_t* ctx);

// Returns the path to the temporary PEM file, or NULL if ctx is NULL.
const char* peer_verify_ctx_path(const peer_verify_ctx_t* ctx);

#endif // OFFS_PEER_VERIFY_H
