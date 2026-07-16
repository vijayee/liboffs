//
// Created by victor on 6/20/26.
//

#include "config_json.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* The three field sets. Kept as plain tables so the HTTP routes and the Unix
   handlers validate against the exact same lists (lifted out of
   HTTP/config_routes.c so the two paths cannot drift). */

static const char* _string_fields[] = {
  "api_key_hash", "https_cert_path", "https_key_path",
  "tcp_tls_cert_path", "tcp_tls_key_path", NULL
};

static const char* _bool_fields[] = {
  "http_enabled", "https_enabled", "unix_enabled", "tcp_enabled",
  "ws_enabled", "wt_enabled", "tcp_tls_enabled", "allow_insecure", NULL
};

static const char* _number_fields[] = {
  "cache_size", "max_snapshots", "max_wals", "max_capacity_bytes",
  "scheduler_thread_count", "http_port", "https_port",
  "tcp_port", "ws_port", "wt_port", NULL
};

static bool _in_list(const char* name, const char* const* list) {
  for (size_t i = 0; list[i] != NULL; i++) {
    if (strcmp(name, list[i]) == 0) return true;
  }
  return false;
}

bool config_is_known_field(const char* name) {
  if (name == NULL) return false;
  return _in_list(name, _string_fields) ||
         _in_list(name, _bool_fields) ||
         _in_list(name, _number_fields);
}

config_field_type_t config_field_type(const char* name) {
  if (_in_list(name, _string_fields)) return CONFIG_FIELD_STRING;
  if (_in_list(name, _bool_fields)) return CONFIG_FIELD_BOOL;
  return CONFIG_FIELD_NUMBER;
}

cJSON* config_to_json(const config_t* config) {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddNumberToObject(root, "cache_size", (double)config->cache_size);
  cJSON_AddNumberToObject(root, "max_snapshots", (double)config->max_snapshots);
  cJSON_AddNumberToObject(root, "max_wals", (double)config->max_wals);
  cJSON_AddNumberToObject(root, "max_capacity_bytes", (double)config->max_capacity_bytes);
  cJSON_AddNumberToObject(root, "scheduler_thread_count", (double)config->scheduler_thread_count);

  cJSON_AddBoolToObject(root, "http_enabled", config->http_enabled);
  cJSON_AddNumberToObject(root, "http_port", (double)config->http_port);
  cJSON_AddBoolToObject(root, "https_enabled", config->https_enabled);
  cJSON_AddNumberToObject(root, "https_port", (double)config->https_port);
  if (config->https_cert_path)
    cJSON_AddStringToObject(root, "https_cert_path", config->https_cert_path);
  else
    cJSON_AddNullToObject(root, "https_cert_path");
  if (config->https_key_path)
    cJSON_AddStringToObject(root, "https_key_path", config->https_key_path);
  else
    cJSON_AddNullToObject(root, "https_key_path");

  cJSON_AddBoolToObject(root, "unix_enabled", config->unix_enabled);
  cJSON_AddBoolToObject(root, "tcp_enabled", config->tcp_enabled);
  cJSON_AddNumberToObject(root, "tcp_port", (double)config->tcp_port);
  cJSON_AddBoolToObject(root, "ws_enabled", config->ws_enabled);
  cJSON_AddNumberToObject(root, "ws_port", (double)config->ws_port);
  cJSON_AddBoolToObject(root, "wt_enabled", config->wt_enabled);
  cJSON_AddNumberToObject(root, "wt_port", (double)config->wt_port);

  cJSON_AddBoolToObject(root, "tcp_tls_enabled", config->tcp_tls_enabled);
  if (config->tcp_tls_cert_path)
    cJSON_AddStringToObject(root, "tcp_tls_cert_path", config->tcp_tls_cert_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_cert_path");
  if (config->tcp_tls_key_path)
    cJSON_AddStringToObject(root, "tcp_tls_key_path", config->tcp_tls_key_path);
  else
    cJSON_AddNullToObject(root, "tcp_tls_key_path");

  cJSON_AddBoolToObject(root, "allow_insecure", config->allow_insecure);

  if (config->api_key_hash)
    cJSON_AddStringToObject(root, "api_key_hash", config->api_key_hash);
  else
    cJSON_AddNullToObject(root, "api_key_hash");

  return root;
}

cJSON* config_field_value_from_string(const char* field, const char* value,
                                      char* err_buf, size_t err_len) {
  if (field == NULL || value == NULL) {
    if (err_buf && err_len) snprintf(err_buf, err_len, "missing field or value");
    return NULL;
  }

  config_field_type_t type = config_field_type(field);

  /* "null" token means revert-to-default for every field type; config_pending
     interprets a JSON null as "remove from pending", which restores the
     config_default value on the next load. */
  if (strcmp(value, "null") == 0) {
    return cJSON_CreateNull();
  }

  switch (type) {
    case CONFIG_FIELD_STRING:
      return cJSON_CreateString(value);
    case CONFIG_FIELD_BOOL:
      if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
        return cJSON_CreateBool(1);
      if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
        return cJSON_CreateBool(0);
      if (err_buf && err_len)
        snprintf(err_buf, err_len, "expected true/false (bool)");
      return NULL;
    case CONFIG_FIELD_NUMBER: {
      char* end = NULL;
      long long parsed = strtoll(value, &end, 10);
      if (end == value || (end != NULL && *end != '\0')) {
        if (err_buf && err_len)
          snprintf(err_buf, err_len, "expected integer (number)");
        return NULL;
      }
      return cJSON_CreateNumber((double)parsed);
    }
  }
  /* Unreachable for known fields. */
  if (err_buf && err_len) snprintf(err_buf, err_len, "unknown field");
  return NULL;
}