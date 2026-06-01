/*
 * metrics.h — Lightweight Prometheus-style metrics for liboffs
 *
 * Counter:   atomic increment-only, monotonic
 * Gauge:     atomic set/inc/dec, can go up and down
 * Histogram: atomic bucket increments + sum + count
 *
 * All operations are lock-free (atomics). The registry provides
 * iteration for exposure via health endpoints.
 */

#ifndef METRICS_H
#define METRICS_H

#include "../Util/atomic_compat.h"
#include <stdint.h>

#define METRICS_MAX_REGISTRATIONS 64
#define METRICS_HISTOGRAM_MAX_BUCKETS 16

/* --- Counter --- */

typedef struct {
  ATOMIC(uint64_t) value;
  const char *name;
  const char *help;
} metrics_counter_t;

void metrics_counter_init(metrics_counter_t *counter, const char *name,
                          const char *help);
void metrics_counter_inc(metrics_counter_t *counter);
uint64_t metrics_counter_value(const metrics_counter_t *counter);

/* --- Gauge --- */

typedef struct {
  ATOMIC(int64_t) value;
  const char *name;
  const char *help;
} metrics_gauge_t;

void metrics_gauge_init(metrics_gauge_t *gauge, const char *name,
                        const char *help);
void metrics_gauge_set(metrics_gauge_t *gauge, int64_t value);
void metrics_gauge_inc(metrics_gauge_t *gauge);
void metrics_gauge_dec(metrics_gauge_t *gauge);
int64_t metrics_gauge_value(const metrics_gauge_t *gauge);

/* --- Histogram --- */

typedef struct {
  ATOMIC(uint64_t) buckets[METRICS_HISTOGRAM_MAX_BUCKETS];
  ATOMIC(uint64_t) sum_int;    /* sum * 1000 (milli-units) to keep it integral */
  ATOMIC(uint64_t) count;
  const char *name;
  const char *help;
  double bucket_bounds[METRICS_HISTOGRAM_MAX_BUCKETS];
  int num_buckets;
} metrics_histogram_t;

void metrics_histogram_init(metrics_histogram_t *histogram, const char *name,
                            const char *help, const double *bounds, int num_bounds);
void metrics_histogram_observe(metrics_histogram_t *histogram, double value);
uint64_t metrics_histogram_count(const metrics_histogram_t *histogram);
double metrics_histogram_sum(const metrics_histogram_t *histogram);

/* --- Registry --- */

typedef enum {
  METRICS_TYPE_COUNTER,
  METRICS_TYPE_GAUGE,
  METRICS_TYPE_HISTOGRAM,
} metrics_type_t;

typedef struct {
  metrics_type_t type;
  union {
    const metrics_counter_t *counter;
    const metrics_gauge_t *gauge;
    const metrics_histogram_t *histogram;
  } ptr;
} metrics_entry_t;

void metrics_registry_register_counter(const metrics_counter_t *counter);
void metrics_registry_register_gauge(const metrics_gauge_t *gauge);
void metrics_registry_register_histogram(const metrics_histogram_t *histogram);

int metrics_registry_count(void);
const metrics_entry_t *metrics_registry_get(int index);

/* Append all registered metrics to an existing cJSON object under "metrics" */
struct cJSON;
void metrics_registry_to_json(struct cJSON *root);

#endif
