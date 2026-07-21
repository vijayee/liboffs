#include "config_pending.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cJSON.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char* _pending_path(const char* data_dir) {
  size_t len = strlen(data_dir) + 32;
  char* path = get_clear_memory(len);
  snprintf(path, len, "%s/pending_config.json", data_dir);
  return path;
}

int config_pending_save(const char* data_dir, const char* json_body, size_t body_len) {
  /* Parse incoming JSON */
  cJSON* incoming = cJSON_ParseWithLength(json_body, body_len);
  if (incoming == NULL) {
    log_error("config_pending_save: failed to parse JSON body");
    return -1;
  }

  /* Load existing pending config if any */
  cJSON* existing = NULL;
  char* pending_path = _pending_path(data_dir);
  FILE* file = fopen(pending_path, "r");
  if (file != NULL) {
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size > 0) {
      char* existing_json = get_clear_memory((size_t)(file_size + 1));
      size_t read_count = fread(existing_json, 1, (size_t)file_size, file);
      if (read_count > 0) {
        existing = cJSON_Parse(existing_json);
      }
      free(existing_json);
    }
    fclose(file);
  }

  /* Merge: copy existing, then overlay incoming */
  cJSON* merged = existing ? cJSON_Duplicate(existing, 1) : cJSON_CreateObject();
  if (existing) cJSON_Delete(existing);

  /* For each field in incoming, set or delete in merged */
  cJSON* item = incoming->child;
  while (item != NULL) {
    if (cJSON_IsNull(item)) {
      /* Null means revert to default — remove from pending */
      cJSON_DeleteItemFromObject(merged, item->string);
    } else {
      /* Remove old key if present, then add new value */
      cJSON* old = cJSON_DetachItemFromObject(merged, item->string);
      if (old) cJSON_Delete(old);
      cJSON_AddItemToObject(merged, item->string, cJSON_Duplicate(item, 1));
    }
    item = item->next;
  }
  cJSON_Delete(incoming);

  /* Serialize and write */
  char* merged_str = cJSON_Print(merged);
  cJSON_Delete(merged);
  if (merged_str == NULL) {
    free(pending_path);
    return -1;
  }

  file = fopen(pending_path, "w");
  if (file == NULL) {
    log_error("config_pending_save: failed to open %s for writing", pending_path);
    free(merged_str);
    free(pending_path);
    return -1;
  }
  fputs(merged_str, file);
  fclose(file);
  free(merged_str);
  free(pending_path);
  return 0;
}

config_t* config_pending_load(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  FILE* file = fopen(pending_path, "r");
  free(pending_path);
  if (file == NULL) return NULL;

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0) { fclose(file); return NULL; }

  char* json_str = get_clear_memory((size_t)(file_size + 1));
  size_t read_count = fread(json_str, 1, (size_t)file_size, file);
  fclose(file);
  if (read_count == 0) { free(json_str); return NULL; }

  cJSON* root = cJSON_Parse(json_str);
  free(json_str);
  if (root == NULL) {
    log_error("config_pending_load: failed to parse pending config");
    return NULL;
  }

  config_t* config = get_clear_memory(sizeof(config_t));
  *config = config_default();

  /* Apply overrides from JSON — each key maps to config_t field */
  cJSON* item = root->child;
  while (item != NULL) {
    /* size_t fields */
    if (strcmp(item->string, "cache_size") == 0 && cJSON_IsNumber(item))
      config->cache_size = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_snapshots") == 0 && cJSON_IsNumber(item))
      config->max_snapshots = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_wals") == 0 && cJSON_IsNumber(item))
      config->max_wals = (size_t)item->valuedouble;
    else if (strcmp(item->string, "scheduler_thread_count") == 0 && cJSON_IsNumber(item))
      config->scheduler_thread_count = (size_t)item->valuedouble;
    else if (strcmp(item->string, "max_capacity_bytes") == 0 && cJSON_IsNumber(item))
      config->max_capacity_bytes = (size_t)item->valuedouble;
    /* bool fields */
    else if (strcmp(item->string, "http_enabled") == 0 && cJSON_IsBool(item))
      config->http_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "https_enabled") == 0 && cJSON_IsBool(item))
      config->https_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "unix_enabled") == 0 && cJSON_IsBool(item))
      config->unix_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "tcp_enabled") == 0 && cJSON_IsBool(item))
      config->tcp_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "ws_enabled") == 0 && cJSON_IsBool(item))
      config->ws_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "wt_enabled") == 0 && cJSON_IsBool(item))
      config->wt_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "tcp_tls_enabled") == 0 && cJSON_IsBool(item))
      config->tcp_tls_enabled = cJSON_IsTrue(item);
    else if (strcmp(item->string, "allow_secure") == 0 && cJSON_IsBool(item))
      config->allow_secure = cJSON_IsTrue(item);
    /* uint16_t fields */
    else if (strcmp(item->string, "http_port") == 0 && cJSON_IsNumber(item))
      config->http_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "https_port") == 0 && cJSON_IsNumber(item))
      config->https_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "tcp_port") == 0 && cJSON_IsNumber(item))
      config->tcp_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "ws_port") == 0 && cJSON_IsNumber(item))
      config->ws_port = (uint16_t)item->valuedouble;
    else if (strcmp(item->string, "wt_port") == 0 && cJSON_IsNumber(item))
      config->wt_port = (uint16_t)item->valuedouble;
    /* string fields */
    else if (strcmp(item->string, "api_key_hash") == 0 && cJSON_IsString(item))
      config->api_key_hash = strdup(item->valuestring);
    else if (strcmp(item->string, "https_cert_path") == 0 && cJSON_IsString(item))
      config->https_cert_path = strdup(item->valuestring);
    else if (strcmp(item->string, "https_key_path") == 0 && cJSON_IsString(item))
      config->https_key_path = strdup(item->valuestring);
    else if (strcmp(item->string, "tcp_tls_cert_path") == 0 && cJSON_IsString(item))
      config->tcp_tls_cert_path = strdup(item->valuestring);
    else if (strcmp(item->string, "tcp_tls_key_path") == 0 && cJSON_IsString(item))
      config->tcp_tls_key_path = strdup(item->valuestring);

    item = item->next;
  }

  cJSON_Delete(root);

  if (config_validate(config) != 0) {
    log_error("config_pending_load: pending config failed validation");
    free(config->api_key_hash);
    free(config->https_cert_path);
    free(config->https_key_path);
    free(config->tcp_tls_cert_path);
    free(config->tcp_tls_key_path);
    free(config);
    return NULL;
  }

  return config;
}

int config_pending_mark_applied(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  size_t len = strlen(data_dir) + 48;
  char* applied_path = get_clear_memory(len);
  snprintf(applied_path, len, "%s/pending_config.applied", data_dir);

  int result = rename(pending_path, applied_path);
  free(pending_path);
  free(applied_path);
  return result;
}

int config_pending_exists(const char* data_dir) {
  char* pending_path = _pending_path(data_dir);
  FILE* file = fopen(pending_path, "r");
  free(pending_path);
  if (file == NULL) return 0;
  fclose(file);
  return 1;
}
