/*
 * topology_report.h — CBOR wire protocol for topology metrics reports
 *
 * Each offsd node periodically serializes its topology_metrics_t snapshot
 * and POSTs it to the offs-metrics server. This header defines the
 * CBOR encode/decode functions for that payload.
 */

#ifndef OFFS_TOPOLOGY_REPORT_H
#define OFFS_TOPOLOGY_REPORT_H

#include "topology_metrics.h"
#include "peer_connection.h"
#include "node_id.h"
#include <cbor.h>
#include <stdint.h>

/* Top-level report payload */
cbor_item_t* topology_report_encode(const node_id_t* reporter_id,
                                    uint64_t timestamp_ms,
                                    const topology_metrics_t* metrics);

int topology_report_decode(cbor_item_t* item,
                           node_id_t* reporter_id_out,
                           uint64_t* timestamp_ms_out,
                           topology_metrics_t* metrics_out);

/* POST a CBOR-encoded topology report to the metrics server URL.
 * Returns 0 on success (HTTP 200), -1 on failure. */
int topology_report_post(const char* url, cbor_item_t* report);

#endif
