//
// Created by victor on 5/22/26.
//

#define _DEFAULT_SOURCE

#include "auth_middleware.h"
#include "http_request.h"
#include "http_response.h"
#include "http_headers.h"
#include "../../Util/bcrypt.h"
#include "../../Util/allocator.h"

#include <stdlib.h>
#include <string.h>

struct auth_middleware_t {
  char* api_key;
  char* bcrypt_hash;
};

auth_middleware_t* auth_middleware_create(const char* api_key, const char* bcrypt_hash) {
  if (api_key == NULL || bcrypt_hash == NULL) {
    return NULL;
  }
  auth_middleware_t* auth = get_clear_memory(sizeof(auth_middleware_t));
  auth->api_key = strdup(api_key);
  auth->bcrypt_hash = strdup(bcrypt_hash);
  if (auth->api_key == NULL || auth->bcrypt_hash == NULL) {
    free(auth->api_key);
    free(auth->bcrypt_hash);
    free(auth);
    return NULL;
  }
  return auth;
}

void auth_middleware_destroy(auth_middleware_t* auth) {
  if (auth == NULL) return;
  /* Zero the API key before freeing */
  if (auth->api_key != NULL) {
    memset(auth->api_key, 0, strlen(auth->api_key));
    free(auth->api_key);
  }
  free(auth->bcrypt_hash);
  free(auth);
}

static int _auth_handler(http_request_t* request, http_response_t* response, void* user_data) {
  auth_middleware_t* auth = (auth_middleware_t*)user_data;

  const char* auth_header = http_headers_get(&request->headers, "Authorization");
  if (auth_header == NULL) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Authentication required", 22);
    http_response_end(response);
    return 1;
  }

  /* Check for "Bearer " prefix (case-sensitive, per RFC 6750) */
  if (strncmp(auth_header, "Bearer ", 7) != 0) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid authentication scheme", 29);
    http_response_end(response);
    return 1;
  }

  const char* token = auth_header + 7;
  if (*token == '\0') {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Empty token", 11);
    http_response_end(response);
    return 1;
  }

  if (bcrypt_check(token, auth->bcrypt_hash) != 0) {
    http_response_set_status(response, HTTP_STATUS_FORBIDDEN);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid API key", 15);
    http_response_end(response);
    return 1;
  }

  request->is_authenticated = 1;
  return 0;
}

http_middleware_t auth_middleware_handler(void) {
  return _auth_handler;
}
