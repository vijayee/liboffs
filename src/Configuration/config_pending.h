#ifndef OFFS_CONFIG_PENDING_H
#define OFFS_CONFIG_PENDING_H

#include "config.h"
#include <stddef.h>

/* Save a partial config (only the fields present in updates) as JSON.
   Merged with any existing pending config on disk.
   Returns 0 on success, -1 on error. */
int config_pending_save(const char* data_dir, const char* json_body, size_t body_len);

/* Load pending config from disk and merge into defaults.
   Returns a new config_t (caller must free string fields and the struct itself).
   Returns NULL if no pending config exists or it fails validation. */
config_t* config_pending_load(const char* data_dir);

/* Mark pending config as applied (rename .json to .applied).
   Returns 0 on success, -1 if no pending config exists. */
int config_pending_mark_applied(const char* data_dir);

/* Check if a pending config file exists.
   Returns 1 if exists, 0 if not, -1 on error. */
int config_pending_exists(const char* data_dir);

#endif
