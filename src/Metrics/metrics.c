/*
 * metrics.c — Lightweight Prometheus-style metrics implementation
 */

#include "metrics.h"
#include <string.h>

/* --- Counter --- */

void metrics_counter_init(metrics_counter_t *counter, const char *name,
                          const char *help) {
  memset(counter, 0, sizeof(*counter));
  counter->name = name;
  counter->help = help;
}

void metrics_counter_inc(metrics_counter_t *counter) {
  atomic_fetch_add(&counter->value, 1);
}

uint64_t metrics_counter_value(const metrics_counter_t *counter) {
  return atomic_load(&counter->value);
}

/* --- Gauge --- */

void metrics_gauge_init(metrics_gauge_t *gauge, const char *name,
                        const char *help) {
  memset(gauge, 0, sizeof(*gauge));
  gauge->name = name;
  gauge->help = help;
}

void metrics_gauge_set(metrics_gauge_t *gauge, int64_t value) {
  atomic_store(&gauge->value, value);
}

void metrics_gauge_inc(metrics_gauge_t *gauge) {
  atomic_fetch_add(&gauge->value, 1);
}

void metrics_gauge_dec(metrics_gauge_t *gauge) {
  atomic_fetch_sub(&gauge->value, 1);
}

int64_t metrics_gauge_value(const metrics_gauge_t *gauge) {
  return atomic_load(&gauge->value);
}

/* --- Histogram --- */

void metrics_histogram_init(metrics_histogram_t *histogram, const char *name,
                            const char *help, const double *bounds,
                            int num_bounds) {
  memset(histogram, 0, sizeof(*histogram));
  histogram->name = name;
  histogram->help = help;
  if (num_bounds > METRICS_HISTOGRAM_MAX_BUCKETS) {
    num_bounds = METRICS_HISTOGRAM_MAX_BUCKETS;
  }
  histogram->num_buckets = num_bounds;
  for (int index = 0; index < num_bounds; index++) {
    histogram->bucket_bounds[index] = bounds[index];
  }
}

void metrics_histogram_observe(metrics_histogram_t *histogram, double value) {
  /* Find the first bucket whose upper bound >= value, then +Inf bucket */
  int bucket_index = histogram->num_buckets; /* +Inf bucket is last */
  for (int index = 0; index < histogram->num_buckets; index++) {
    if (value <= histogram->bucket_bounds[index]) {
      bucket_index = index;
      break;
    }
  }
  atomic_fetch_add(&histogram->buckets[bucket_index], 1);
  /* Store sum as millis to keep it integral */
  atomic_fetch_add(&histogram->sum_int, (uint64_t)(value * 1000.0));
  atomic_fetch_add(&histogram->count, 1);
}

uint64_t metrics_histogram_count(const metrics_histogram_t *histogram) {
  return atomic_load(&histogram->count);
}

double metrics_histogram_sum(const metrics_histogram_t *histogram) {
  return (double)atomic_load(&histogram->sum_int) / 1000.0;
}

/* --- Registry --- */

static metrics_entry_t _registry[METRICS_MAX_REGISTRATIONS];
static int _registry_count = 0;

void metrics_registry_register_counter(const metrics_counter_t *counter) {
  if (_registry_count >= METRICS_MAX_REGISTRATIONS) return;
  _registry[_registry_count].type = METRICS_TYPE_COUNTER;
  _registry[_registry_count].ptr.counter = counter;
  _registry_count++;
}

void metrics_registry_register_gauge(const metrics_gauge_t *gauge) {
  if (_registry_count >= METRICS_MAX_REGISTRATIONS) return;
  _registry[_registry_count].type = METRICS_TYPE_GAUGE;
  _registry[_registry_count].ptr.gauge = gauge;
  _registry_count++;
}

void metrics_registry_register_histogram(const metrics_histogram_t *histogram) {
  if (_registry_count >= METRICS_MAX_REGISTRATIONS) return;
  _registry[_registry_count].type = METRICS_TYPE_HISTOGRAM;
  _registry[_registry_count].ptr.histogram = histogram;
  _registry_count++;
}

int metrics_registry_count(void) {
  return _registry_count;
}

const metrics_entry_t *metrics_registry_get(int index) {
  if (index < 0 || index >= _registry_count) return NULL;
  return &_registry[index];
}

#include <cJSON.h>

void metrics_registry_to_json(struct cJSON *root) {
  cJSON *metrics_json = cJSON_CreateObject();

  for (int index = 0; index < _registry_count; index++) {
    const metrics_entry_t *entry = &_registry[index];

    switch (entry->type) {
    case METRICS_TYPE_COUNTER: {
      const metrics_counter_t *c = entry->ptr.counter;
      cJSON *counter_json = cJSON_CreateObject();
      cJSON_AddStringToObject(counter_json, "type", "counter");
      if (c->help != NULL) cJSON_AddStringToObject(counter_json, "help", c->help);
      cJSON_AddNumberToObject(counter_json, "value",
                              (double)metrics_counter_value(c));
      cJSON_AddItemToObject(metrics_json, c->name, counter_json);
      break;
    }
    case METRICS_TYPE_GAUGE: {
      const metrics_gauge_t *g = entry->ptr.gauge;
      cJSON *gauge_json = cJSON_CreateObject();
      cJSON_AddStringToObject(gauge_json, "type", "gauge");
      if (g->help != NULL) cJSON_AddStringToObject(gauge_json, "help", g->help);
      cJSON_AddNumberToObject(gauge_json, "value",
                              (double)metrics_gauge_value(g));
      cJSON_AddItemToObject(metrics_json, g->name, gauge_json);
      break;
    }
    case METRICS_TYPE_HISTOGRAM: {
      const metrics_histogram_t *h = entry->ptr.histogram;
      cJSON *histogram_json = cJSON_CreateObject();
      cJSON_AddStringToObject(histogram_json, "type", "histogram");
      if (h->help != NULL) cJSON_AddStringToObject(histogram_json, "help", h->help);
      cJSON_AddNumberToObject(histogram_json, "count",
                              (double)metrics_histogram_count(h));
      cJSON_AddNumberToObject(histogram_json, "sum", metrics_histogram_sum(h));

      cJSON *buckets_json = cJSON_CreateArray();
      for (int bucket_index = 0; bucket_index <= h->num_buckets; bucket_index++) {
        cJSON *bucket = cJSON_CreateObject();
        if (bucket_index < h->num_buckets) {
          cJSON_AddNumberToObject(bucket, "le", h->bucket_bounds[bucket_index]);
        } else {
          cJSON_AddStringToObject(bucket, "le", "+Inf");
        }
        cJSON_AddNumberToObject(bucket, "value",
                                (double)atomic_load(&h->buckets[bucket_index]));
        cJSON_AddItemToArray(buckets_json, bucket);
      }
      cJSON_AddItemToObject(histogram_json, "buckets", buckets_json);
      cJSON_AddItemToObject(metrics_json, h->name, histogram_json);
      break;
    }
    }
  }

  cJSON_AddItemToObject(root, "metrics", metrics_json);
}
