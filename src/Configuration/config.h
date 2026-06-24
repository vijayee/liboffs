//
// Created by victor on 9/16/25.
//

#ifndef OFFS_CONFIG_H
#define OFFS_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
  size_t index_bucket_size;
  size_t index_wait;
  size_t index_max_wait;
  size_t section_size;
  size_t section_cache_count;
  size_t section_wait;
  size_t section_max_wait;
  size_t cache_size;
  size_t max_tuple_size;
  size_t min_tuple_size;
  size_t lru_size;
  size_t descriptor_pad;
  size_t max_snapshots;
  size_t max_wals;
  size_t max_capacity_bytes;
  uint32_t shutdown_timeout_ms;
  size_t scheduler_thread_count;
  uint32_t gossip_init_interval_s;
  size_t gossip_init_count;
  uint32_t gossip_steady_interval_s;
  uint32_t gossip_timeout_ms;
  float hebbian_decay_factor;
  uint32_t eabf_base_ttl_ms;
  uint32_t eabf_maintenance_ms;
  uint32_t respiration_tau_min_ms;
  uint32_t respiration_tau_max_ms;
  size_t relay_max_retries;
  uint32_t relay_retry_delay_ms;

  /* Client API enable flags and ports */
  bool     http_enabled;
  uint16_t http_port;
  bool     https_enabled;
  uint16_t https_port;
  char*    https_cert_path;
  char*    https_key_path;
  bool     unix_enabled;
  bool     tcp_enabled;
  uint16_t tcp_port;
  bool     ws_enabled;
  uint16_t ws_port;
  bool     wt_enabled;
  uint16_t wt_port;

  /* TCP TLS */
  bool     tcp_tls_enabled;
  char*    tcp_tls_cert_path;
  char*    tcp_tls_key_path;

  /* Auth */
  char*    api_key_hash;        // bcrypt hash ($2b$ prefix), NULL if auth disabled

  /* Logging */
  int      log_level;            // LOG_TRACE..LOG_FATAL from Util/log.h
  bool     log_structured;       // enable key=value structured output
  int      log_module_levels[9]; // per-module levels, indexed by log_module_t
} config_t;
config_t config_default();
int config_validate(const config_t* config);
config_t* config_deep_copy(const config_t* src);
void config_free(config_t* config);

/* Free only the owning char* members of a config (api_key_hash, https_*_path,
   tcp_tls_*_path) without freeing the struct itself. For embedded config_t
   values (e.g. offsd's server->config) where config_free would erroneously
   free() the surrounding storage. Safe on a default/zeroed config (all NULL). */
void config_free_members(config_t* config);
#endif //OFFS_CONFIG_H
