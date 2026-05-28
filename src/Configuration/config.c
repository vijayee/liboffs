//
// Created by victor on 9/16/25.
//

#include "config.h"
#include "../Util/log.h"
#include <stdbool.h>
#include <string.h>

config_t config_default() {
  config_t config;
  config.index_bucket_size = 25;
  config.index_wait = 5;
  config.index_max_wait = 5000;
  config.section_size = 78;
  config.section_cache_count = 15;
  config.section_wait = 5;
  config.section_max_wait = 5000;
  config.cache_size = 50;
  config.max_tuple_size = 5;
  config.min_tuple_size  = 2;
  config.lru_size = 30;
  config.descriptor_pad = 32;
  config.max_snapshots = 3;
  config.max_wals = 3;
  config.max_capacity_bytes = 5368709120;
  config.shutdown_timeout_ms = 30000;
  config.scheduler_thread_count = 4;
  config.gossip_init_interval_s = 2;
  config.gossip_init_count = 5;
  config.gossip_steady_interval_s = 30;
  config.gossip_timeout_ms = 5000;
  config.hebbian_decay_factor = 0.999f;
  config.eabf_base_ttl_ms = 3600000;
  config.eabf_maintenance_ms = 60000;
  config.respiration_tau_min_ms = 5000;
  config.respiration_tau_max_ms = 300000;
  config.relay_max_retries = 5;
  config.relay_retry_delay_ms = 500;
  config.http_enabled = false;
  config.http_port = 80;
  config.https_enabled = false;
  config.https_port = 443;
  config.https_cert_path = NULL;
  config.https_key_path = NULL;
  config.unix_enabled = false;
  config.tcp_enabled = false;
  config.tcp_port = 9000;
  config.ws_enabled = false;
  config.ws_port = 9001;
  config.wt_enabled = false;
  config.wt_port = 9002;
  config.tcp_tls_enabled = false;
  config.tcp_tls_cert_path = NULL;
  config.tcp_tls_key_path = NULL;
  config.api_key_hash = NULL;
  return config;
}

int config_validate(const config_t* config) {
  if (config == NULL) {
    log_error("config_validate: config is NULL");
    return -1;
  }

  bool valid = true;

  if (config->index_bucket_size == 0) {
    log_error("config_validate: index_bucket_size must be > 0");
    valid = false;
  }
  if (config->index_wait == 0) {
    log_error("config_validate: index_wait must be > 0");
    valid = false;
  }
  if (config->index_max_wait < config->index_wait) {
    log_error("config_validate: index_max_wait (%zu) must be >= index_wait (%zu)",
              config->index_max_wait, config->index_wait);
    valid = false;
  }
  if (config->section_size == 0) {
    log_error("config_validate: section_size must be > 0");
    valid = false;
  }
  if (config->section_cache_count == 0) {
    log_error("config_validate: section_cache_count must be > 0");
    valid = false;
  }
  if (config->section_wait == 0) {
    log_error("config_validate: section_wait must be > 0");
    valid = false;
  }
  if (config->section_max_wait < config->section_wait) {
    log_error("config_validate: section_max_wait (%zu) must be >= section_wait (%zu)",
              config->section_max_wait, config->section_wait);
    valid = false;
  }
  if (config->cache_size == 0) {
    log_error("config_validate: cache_size must be > 0");
    valid = false;
  }
  if (config->max_tuple_size < config->min_tuple_size) {
    log_error("config_validate: max_tuple_size (%zu) must be >= min_tuple_size (%zu)",
              config->max_tuple_size, config->min_tuple_size);
    valid = false;
  }
  if (config->min_tuple_size == 0) {
    log_error("config_validate: min_tuple_size must be > 0");
    valid = false;
  }
  if (config->lru_size == 0) {
    log_error("config_validate: lru_size must be > 0");
    valid = false;
  }
  if (config->lru_size > config->cache_size) {
    log_error("config_validate: lru_size (%zu) must be <= cache_size (%zu)",
              config->lru_size, config->cache_size);
    valid = false;
  }
  if (config->descriptor_pad == 0) {
    log_error("config_validate: descriptor_pad must be > 0");
    valid = false;
  }
  if (config->max_snapshots == 0) {
    log_error("config_validate: max_snapshots must be > 0");
    valid = false;
  }
  if (config->max_wals == 0) {
    log_error("config_validate: max_wals must be > 0");
    valid = false;
  }
  if (config->max_capacity_bytes == 0) {
    log_error("config_validate: max_capacity_bytes must be > 0");
    valid = false;
  }
  if (config->scheduler_thread_count == 0 || config->scheduler_thread_count > 256) {
    log_error("config_validate: scheduler_thread_count (%zu) must be 1-256",
              config->scheduler_thread_count);
    valid = false;
  }
  if (config->gossip_init_interval_s == 0) {
    log_error("config_validate: gossip_init_interval_s must be > 0");
    valid = false;
  }
  if (config->gossip_init_count == 0) {
    log_error("config_validate: gossip_init_count must be > 0");
    valid = false;
  }
  if (config->gossip_steady_interval_s < config->gossip_init_interval_s) {
    log_error("config_validate: gossip_steady_interval_s (%u) must be >= gossip_init_interval_s (%u)",
              config->gossip_steady_interval_s, config->gossip_init_interval_s);
    valid = false;
  }
  if (config->gossip_timeout_ms == 0) {
    log_error("config_validate: gossip_timeout_ms must be > 0");
    valid = false;
  }
  if (config->hebbian_decay_factor <= 0.0f || config->hebbian_decay_factor >= 1.0f) {
    log_error("config_validate: hebbian_decay_factor (%f) must be in (0.0, 1.0)",
              config->hebbian_decay_factor);
    valid = false;
  }
  if (config->eabf_base_ttl_ms < config->eabf_maintenance_ms) {
    log_error("config_validate: eabf_base_ttl_ms (%u) must be >= eabf_maintenance_ms (%u)",
              config->eabf_base_ttl_ms, config->eabf_maintenance_ms);
    valid = false;
  }
  if (config->eabf_maintenance_ms == 0) {
    log_error("config_validate: eabf_maintenance_ms must be > 0");
    valid = false;
  }
  if (config->respiration_tau_min_ms == 0) {
    log_error("config_validate: respiration_tau_min_ms must be > 0");
    valid = false;
  }
  if (config->respiration_tau_max_ms < config->respiration_tau_min_ms) {
    log_error("config_validate: respiration_tau_max_ms (%u) must be >= respiration_tau_min_ms (%u)",
              config->respiration_tau_max_ms, config->respiration_tau_min_ms);
    valid = false;
  }
  if (config->relay_max_retries == 0) {
    log_error("config_validate: relay_max_retries must be > 0");
    valid = false;
  }
  if (config->relay_retry_delay_ms == 0) {
    log_error("config_validate: relay_retry_delay_ms must be > 0");
    valid = false;
  }

  /* HTTPS requires cert and key paths */
  if (config->https_enabled) {
    if (config->https_cert_path == NULL || config->https_key_path == NULL) {
      log_error("https_enabled requires https_cert_path and https_key_path");
      valid = false;
    }
  }

  /* TCP TLS requires cert and key paths */
  if (config->tcp_tls_enabled) {
    if (config->tcp_tls_cert_path == NULL || config->tcp_tls_key_path == NULL) {
      log_error("tcp_tls_enabled requires tcp_tls_cert_path and tcp_tls_key_path");
      valid = false;
    }
  }

  /* API key hash format validation */
  if (config->api_key_hash != NULL) {
    size_t hash_len = strlen(config->api_key_hash);
    if (hash_len != 60 || strncmp(config->api_key_hash, "$2b$", 4) != 0) {
      log_error("api_key_hash must be a bcrypt $2b$ hash (60 characters)");
      valid = false;
    }
    /* API keys over plaintext remote transports are forbidden */
    if (config->tcp_enabled && !config->tcp_tls_enabled) {
      log_error("tcp_enabled without TLS cannot be used with api_key_hash (plaintext remote transport)");
      valid = false;
    }
    if (config->ws_enabled) {
      log_error("ws_enabled cannot be used with api_key_hash (plaintext remote transport)");
      valid = false;
    }
  }

  return valid ? 0 : -1;
}