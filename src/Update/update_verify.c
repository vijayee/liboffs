#include "update_verify.h"

#include <openssl/err.h>

SSL_CTX* update_ssl_context_create(void) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) {
    return NULL;
  }
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_verify_depth(ctx, 4);
  return ctx;
}