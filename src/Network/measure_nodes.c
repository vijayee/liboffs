//
// Created by victor on 5/18/25.
//

#include "measure_nodes.h"
#include "latency_cache.h"
#include <stddef.h>

size_t measure_nodes_execute(
    latency_cache_t* latency_cache,
    const wire_measure_nodes_t* query,
    uint32_t* latencies_us) {
  if (query == NULL || latencies_us == NULL) return 0;

  size_t target_count = query->target_count;
  if (target_count > MEASURE_NODES_MAX_TARGETS) {
    target_count = MEASURE_NODES_MAX_TARGETS;
  }

  for (size_t index = 0; index < target_count; index++) {
    float latency_ms = 0.0f;
    int found = latency_cache_get(latency_cache, &query->targets[index], &latency_ms);
    if (found == 0) {
      latencies_us[index] = (uint32_t)(latency_ms * 1000.0f);
    } else {
      latencies_us[index] = 0;
    }
  }

  return target_count;
}