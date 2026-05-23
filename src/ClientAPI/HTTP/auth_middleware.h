//
// Created by victor on 5/22/26.
//

#ifndef OFFS_AUTH_MIDDLEWARE_H
#define OFFS_AUTH_MIDDLEWARE_H

#include "http_server.h"

typedef struct auth_middleware_t auth_middleware_t;

/* Create an auth middleware that checks Bearer tokens against a bcrypt hash.
   api_key is the expected plaintext key, bcrypt_hash is the stored hash.
   Returns NULL if either argument is NULL. */
auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash);

void auth_middleware_destroy(auth_middleware_t* auth);

/* Returns the middleware handler function for http_server_use() */
http_middleware_t auth_middleware_handler(void);

#endif /* OFFS_AUTH_MIDDLEWARE_H */
