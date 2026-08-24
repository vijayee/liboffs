#ifndef OFFS_UPDATE_VERIFY_H
#define OFFS_UPDATE_VERIFY_H

#include <openssl/ssl.h>

/* Create an SSL_CTX configured for TLS peer verification: loads default CA
 * paths, enables SSL_VERIFY_PEER, sets verify depth 4. Returns NULL on
 * failure. Caller frees with SSL_CTX_free. */
SSL_CTX* update_ssl_context_create(void);

#endif /* OFFS_UPDATE_VERIFY_H */