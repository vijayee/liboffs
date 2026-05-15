#include "hebbian_config.h"
#include "wire.h"
#include <string.h>

void hebbian_config_init(hebbian_config_t* config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(hebbian_config_t));
  config->initial_weight = 0.1f;
  config->drop_threshold = 0.01f;
  config->decay_rate = 0.001f;
  config->decay_tick_ms = 60000;
  config->base_reward = 0.1f;
  config->failure_penalty = 0.2f;
  config->rate_limit_penalty = 0.1f;
  config->recall_reward = 2.0f;

  for (size_t index = 0; index < HEBBIAN_RPC_MULTIPLIER_COUNT; index++) {
    config->rpc_multipliers[index] = 0.3f;
  }
  config->rpc_multipliers[WIRE_FIND_BLOCK] = 1.0f;
  config->rpc_multipliers[WIRE_FIND_BLOCK_RESPONSE] = 1.0f;
  config->rpc_multipliers[WIRE_STORE_BLOCK] = 1.5f;
  config->rpc_multipliers[WIRE_STORE_BLOCK_RESPONSE] = 1.5f;
  config->rpc_multipliers[WIRE_PING_BLOCK] = 0.8f;
  config->rpc_multipliers[WIRE_PING_BLOCK_RESPONSE] = 0.8f;
  config->rpc_multipliers[WIRE_SEEKING_BLOCKS] = 0.5f;
  config->rpc_multipliers[WIRE_SEEKING_BLOCKS_RESPONSE] = 0.5f;
  config->rpc_multipliers[WIRE_PING_CAPACITY] = 0.3f;
  config->rpc_multipliers[WIRE_PING_CAPACITY_RESPONSE] = 0.3f;
}

void hebbian_config_init_production(hebbian_config_t* config) {
  hebbian_config_init(config);
  config->decay_rate = 0.002f;
  config->drop_threshold = 0.05f;
}