//
// Created by victor on 6/20/26.
//

#ifndef OFFS_CONFIG_JSON_H
#define OFFS_CONFIG_JSON_H

#include "config.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The declared type of a settable config field. Used by both the HTTP config
   routes and the Unix config handlers so the two paths agree on validation. */
typedef enum {
  CONFIG_FIELD_STRING = 0, /* api_key_hash, https_cert_path, https_key_path,
                              tcp_tls_cert_path, tcp_tls_key_path */
  CONFIG_FIELD_BOOL = 1,    /* *_enabled, tcp_tls_enabled */
  CONFIG_FIELD_NUMBER = 2   /* ports, sizes, counts */
} config_field_type_t;

/* True if name is a known, settable config field. */
bool config_is_known_field(const char* name);

/* The declared type of a known field. Returns CONFIG_FIELD_NUMBER for any
   known field that is neither string nor bool. Behavior is undefined for
   unknown fields — callers should check config_is_known_field first. */
config_field_type_t config_field_type(const char* name);

/* Serialize the full config_t to a cJSON object (caller must cJSON_Delete). */
cJSON* config_to_json(const config_t* config);

/* Parse a CLI/Unix-style string value into the typed cJSON node for a field.
   - STRING fields: the literal value becomes a cJSON string; the token "null"
     becomes a cJSON null (revert to default).
   - BOOL fields: "true"/"1" -> true, "false"/"0" -> false, "null" -> null.
   - NUMBER fields: parsed as an integer; "null" -> null.
   Returns the cJSON node on success, or NULL on a type error (with a short
   reason string written to err_buf). The caller owns the returned node. */
cJSON* config_field_value_from_string(const char* field, const char* value,
                                      char* err_buf, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_CONFIG_JSON_H */