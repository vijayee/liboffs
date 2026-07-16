#ifndef OFFS_HEBBIAN_CONFIG_H
#define OFFS_HEBBIAN_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// Size of the rpc_multipliers array. The wire type with the largest numeric
// id is WIRE_RELAY_CHALLENGE_RESPONSE = 35, so the array must hold at least
// 36 entries (indexes 0..35) — otherwise an RPC with type 35 reads past the
// end of rpc_multipliers. See audit #33.
#define HEBBIAN_RPC_MULTIPLIER_COUNT 36

typedef struct hebbian_config_t {
  float initial_weight;
  float drop_threshold;
  float decay_rate;
  uint64_t decay_tick_ms;
  float base_reward;
  float failure_penalty;
  float rate_limit_penalty;
  float recall_reward;
  float rpc_multipliers[HEBBIAN_RPC_MULTIPLIER_COUNT];
} hebbian_config_t;

void hebbian_config_init(hebbian_config_t* config);
void hebbian_config_init_production(hebbian_config_t* config);

#endif