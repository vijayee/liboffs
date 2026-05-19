//
// Created by victor on 5/18/25.
//

#ifndef OFFS_MEASURE_NODES_H
#define OFFS_MEASURE_NODES_H

#include "latency_cache.h"
#include "wire.h"
#include <stdint.h>
#include <stddef.h>

// Execute Measure-N handler logic
// For each target in the query, looks up latency in cache.
// Populates latencies_us with results (0 for not-found targets).
// Returns the number of targets processed.
size_t measure_nodes_execute(
    latency_cache_t* latency_cache,
    const wire_measure_nodes_t* query,
    uint32_t* latencies_us);

#endif // OFFS_MEASURE_NODES_H