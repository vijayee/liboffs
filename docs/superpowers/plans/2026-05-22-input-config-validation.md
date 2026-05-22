# Input Validation & Configuration Validation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 12 configurable network parameters to config_t, validate all 26 config fields at node startup, harden input validation at API boundaries (CBOR wire decoder, HTTP handlers, URL parser), and remove hardcoded `#define` constants.

**Architecture:** Config values flow from `config_t` → `network_t` (via `network_create()` parameters) → subsystem structs (gossip_handle_t, hebbian_table_t, relay_client_t, etc.). Functions that previously read global `#define` constants instead read from struct fields or function parameters. Input validation helpers in `src/Util/validation.h` are applied at three boundaries: CBOR wire decoder, HTTP PUT handlers, and URL parser.

**Tech Stack:** C11, CBOR (libcbor), Google Test (C++), CMake

---

### Task 1: Add 12 new fields to config_t

**Files:**
- Modify: `src/Configuration/config.h:7-27`
- Modify: `src/Configuration/config.c:6-25`

- [ ] **Step 1: Add new fields to config_t struct**

In `src/Configuration/config.h`, add the 12 new fields after the existing `shutdown_timeout_ms` field:

```c
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
} config_t;
```

- [ ] **Step 2: Add defaults for new fields in config_default()**

In `src/Configuration/config.c`, add defaults after `config.shutdown_timeout_ms = 30000;`:

```c
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
```

- [ ] **Step 3: Build to verify compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c
git commit -m "feat: add 12 configurable network parameters to config_t"
```

---

### Task 2: Add config_validate() function

**Files:**
- Modify: `src/Configuration/config.h:28`
- Modify: `src/Configuration/config.c:25`

- [ ] **Step 1: Declare config_validate in config.h**

Add after the `config_default()` declaration in `src/Configuration/config.h`:

```c
#include "../Util/log.h"
int config_validate(const config_t* config);
```

Wait — the `log.h` include should go in the .c file, not the header. Just add the declaration:

```c
int config_validate(const config_t* config);
```

- [ ] **Step 2: Implement config_validate in config.c**

Add after `config_default()` in `src/Configuration/config.c`, using `log_error()` for each violation:

```c
#include "../Util/log.h"
#include <stdbool.h>

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

  return valid ? 0 : -1;
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/Configuration/config.h src/Configuration/config.c
git commit -m "feat: add config_validate() with rules for all 26 config fields"
```

---

### Task 3: Call config_validate() at node startup

**Files:**
- Modify: `src/Node/node.c:37-67`

- [ ] **Step 1: Add config_validate call in offs_node_start()**

In `src/Node/node.c`, add validation as the first check in `offs_node_start()`, right after the NULL check. Also use `node->config->scheduler_thread_count` instead of hardcoded `4`:

```c
int offs_node_start(offs_node_t* node) {
  if (node == NULL) return -1;

  if (node->config == NULL || config_validate(node->config) != 0) {
    log_error("offs_node_start: invalid configuration");
    return -1;
  }

  scheduler_pool_start(node->scheduler);

  node->block_cache = block_cache_create(
    *node->config,
    "sections",
    standard,
    node->timer,
    node->scheduler,
    node->authority,
    node->config->max_capacity_bytes
  );
  // ... rest unchanged
```

And update the `scheduler_pool_create` call in `offs_node_create()`:

```c
  node->scheduler = scheduler_pool_create(config->scheduler_thread_count);
```

Make sure to add `#include "../Util/log.h"` if not already present.

- [ ] **Step 2: Build and run shutdown tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5 && ./test/testliboffs --gtest_filter='*Shutdown*' 2>&1`
Expected: Build succeeds, all shutdown tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/Node/node.c
git commit -m "feat: call config_validate() at node startup, use config->scheduler_thread_count"
```

---

### Task 4: Add config fields to network_t + update network_create()

**Files:**
- Modify: `src/Network/network.h:51-90`
- Modify: `src/Network/network.c:92-185`

- [ ] **Step 1: Add config fields to network_t struct**

In `src/Network/network.h`, add the 11 config fields to `network_t` (all but `scheduler_thread_count` which stays in node.c):

```c
  ATOMIC(uint8_t) running;
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
```

- [ ] **Step 2: Update network_create() to accept and store config values**

Update the signature in `network.h`:

```c
network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool,
                          const config_t* config);
```

In `network.c`, update `network_create()` to accept `const config_t* config` and store fields:

```c
network_t* network_create(authority_t* authority, block_cache_t* block_cache,
                          timer_actor_t* timer, scheduler_pool_t* pool,
                          const config_t* config) {
  // ... existing initialization ...

  network->gossip_init_interval_s = config->gossip_init_interval_s;
  network->gossip_init_count = config->gossip_init_count;
  network->gossip_steady_interval_s = config->gossip_steady_interval_s;
  network->gossip_timeout_ms = config->gossip_timeout_ms;
  network->hebbian_decay_factor = config->hebbian_decay_factor;
  network->eabf_base_ttl_ms = config->eabf_base_ttl_ms;
  network->eabf_maintenance_ms = config->eabf_maintenance_ms;
  network->respiration_tau_min_ms = config->respiration_tau_min_ms;
  network->respiration_tau_max_ms = config->respiration_tau_max_ms;
  network->relay_max_retries = config->relay_max_retries;
  network->relay_retry_delay_ms = config->relay_retry_delay_ms;
  // ... rest unchanged
```

Add `#include "../Configuration/config.h"` if not already present.

- [ ] **Step 3: Update call sites of network_create()**

In `src/Node/node.c`, update the call:

```c
  node->network = network_create(node->authority, node->block_cache,
                                 node->timer, node->scheduler,
                                 node->config);
```

Search for any other `network_create` call sites and update them:
Run: `grep -rn "network_create" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`

If found in test files, update those too with a default config.

- [ ] **Step 4: Build to verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -20`
Expected: Build succeeds (may have compilation errors from downstream consumers still using old defines — those will be fixed in Tasks 5-9).

- [ ] **Step 5: Commit**

```bash
git add src/Network/network.h src/Network/network.c src/Node/node.c
git commit -m "feat: add config fields to network_t, accept config_t in network_create()"
```

---

### Task 5: Wire gossip config values

**Files:**
- Modify: `src/Network/network.c:146-162`

- [ ] **Step 1: Replace hardcoded gossip constants with network_t fields**

In `network_create()` in `src/Network/network.c`, update the `gossip_handle_init` call and gossip timer setup to use `network->` fields instead of `GOSSIP_*` defines:

```c
  gossip_handle_init(&network->gossip,
                     network->gossip_init_interval_s,
                     network->gossip_init_count,
                     network->gossip_steady_interval_s,
                     network->gossip_timeout_ms);

  // ... later, gossip timer setup:
  network->gossip_timer_id = timer_actor_set(timer,
      (uint64_t)network->gossip_init_interval_s * 1000,
      (uint64_t)network->gossip_init_interval_s * 1000,
      &network->actor,
      NETWORK_GOSSIP_TICK);
```

Also update any other references to `GOSSIP_TIMEOUT_MS` in `network.c` (found at lines 584, 1299, 1410) to use `network->gossip_timeout_ms`.

- [ ] **Step 2: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/Network/network.c
git commit -m "refactor: use config-based gossip values instead of GOSSIP_* defines"
```

---

### Task 6: Wire hebbian decay factor

**Files:**
- Modify: `src/Network/hebbian.h:42-43`
- Modify: `src/Network/hebbian.c:138-147`
- Modify: `src/Network/network.c:682`

- [ ] **Step 1: Update hebbian_table_init() signature to accept decay factor**

In `src/Network/hebbian.h`, add `decay_factor` field to `hebbian_table_t`:

```c
typedef struct hebbian_table_t {
  hebbian_weight_t* entries;
  size_t capacity;
  size_t count;
  float decay_factor;
} hebbian_table_t;
```

Update `hebbian_table_init` declaration:

```c
void hebbian_table_init(hebbian_table_t* table, size_t capacity, float decay_factor);
```

- [ ] **Step 2: Update hebbian_table_init() implementation**

In `src/Network/hebbian.c`, update `hebbian_table_init`:

```c
void hebbian_table_init(hebbian_table_t* table, size_t capacity, float decay_factor) {
  if (table == NULL) return;
  table->entries = capacity > 0 ? get_clear_memory(capacity * sizeof(hebbian_weight_t)) : NULL;
  table->capacity = capacity;
  table->count = 0;
  table->decay_factor = decay_factor;
}
```

- [ ] **Step 3: Update hebbian_decay() to use table->decay_factor**

In `src/Network/hebbian.c`, replace `HEBBIAN_DECAY_FACTOR` with `table->decay_factor`:

```c
void hebbian_decay(hebbian_table_t* table) {
  if (table == NULL) return;
  for (size_t index = 0; index < table->count; index++) {
    table->entries[index].weight *= table->decay_factor;
    if (table->entries[index].weight < HEBBIAN_MIN_WEIGHT) {
      table->entries[index].weight = HEBBIAN_MIN_WEIGHT;
    }
  }
}
```

- [ ] **Step 4: Update network_create() to pass decay factor to hebbian**

In `src/Network/network.c`, update the call:

```c
  hebbian_table_init(&network->hebbian, 32, network->hebbian_decay_factor);
```

Also update `connection_manager_init` call if it initializes a hebbian table internally. Search:
Run: `grep -rn "hebbian_table_init" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`

Update all call sites with appropriate decay factor values.

- [ ] **Step 5: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -10`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/Network/hebbian.h src/Network/hebbian.c src/Network/network.c
git commit -m "refactor: use config-based decay_factor in hebbian instead of HEBBIAN_DECAY_FACTOR define"
```

---

### Task 7: Wire EABF TTL and maintenance values

**Files:**
- Modify: `src/Network/eabf.h:93`
- Modify: `src/Network/eabf.c:194-198`
- Modify: `src/Network/network.c:165-167`

- [ ] **Step 1: Add base_ttl_ms and maintenance_ms fields to eabf_table_t**

In `src/Network/eabf.h`, add fields to `eabf_table_t`:

```c
typedef struct eabf_table_t {
  eabf_entry_t* entries;
  size_t capacity;
  size_t count;
  uint64_t base_ttl_ms;
  uint64_t maintenance_ms;
} eabf_table_t;
```

Update `eabf_table_init` declaration to accept the values:

```c
void eabf_table_init(eabf_table_t* table, size_t capacity,
                     uint64_t base_ttl_ms, uint64_t maintenance_ms);
```

Update `eabf_ttl_for_level` declaration to accept `base_ttl_ms`:

```c
uint64_t eabf_ttl_for_level(uint32_t level, uint64_t base_ttl_ms);
```

- [ ] **Step 2: Update eabf_table_init() implementation**

In `src/Network/eabf.c`:

```c
void eabf_table_init(eabf_table_t* table, size_t capacity,
                     uint64_t base_ttl_ms, uint64_t maintenance_ms) {
  table->entries = capacity > 0 ? get_clear_memory(capacity * sizeof(eabf_entry_t)) : NULL;
  table->capacity = capacity;
  table->count = 0;
  table->base_ttl_ms = base_ttl_ms;
  table->maintenance_ms = maintenance_ms;
}
```

- [ ] **Step 3: Update eabf_ttl_for_level() to use parameter**

In `src/Network/eabf.c`:

```c
uint64_t eabf_ttl_for_level(uint32_t level, uint64_t base_ttl_ms) {
  if (level >= EABF_LEVELS) return base_ttl_ms / 4;
  float divisor = (float)(1 << level);
  uint64_t ttl = (uint64_t)((float)base_ttl_ms / divisor);
  if (ttl < 1) ttl = 1;
  return ttl;
}
```

Update all callers of `eabf_ttl_for_level` to pass base_ttl_ms:
Run: `grep -rn "eabf_ttl_for_level" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`
Update each call site with the appropriate base_ttl_ms value from its eabf table.

- [ ] **Step 4: Update network_create() to pass EABF values**

In `src/Network/network.c`:

```c
  eabf_table_init(&network->eabf_table, 16,
                  network->eabf_base_ttl_ms, network->eabf_maintenance_ms);

  network->eabf_maintenance_timer_id = timer_actor_set(timer,
      network->eabf_maintenance_ms,
      network->eabf_maintenance_ms,
      &network->actor,
      NETWORK_EABF_EXPIRE);
```

Update all other `eabf_table_init` call sites:
Run: `grep -rn "eabf_table_init" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`

- [ ] **Step 5: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -10`
Expected: Build succeeds after updating all call sites.

- [ ] **Step 6: Commit**

```bash
git add src/Network/eabf.h src/Network/eabf.c src/Network/network.c
git commit -m "refactor: use config-based TTL and maintenance in EABF instead of defines"
```

---

### Task 8: Wire respiration tau values

**Files:**
- Modify: `src/Network/respiration.h:23`
- Modify: `src/Network/respiration.c:11-22`
- Modify: `src/Network/respiration_actor.c` (if seek_interval is called there)

- [ ] **Step 1: Update respiration_seek_interval() signature**

In `src/Network/respiration.h`:

```c
uint64_t respiration_seek_interval(float capacity,
                                   uint32_t tau_min_ms,
                                   uint32_t tau_max_ms);
```

In `src/Network/respiration.c`:

```c
uint64_t respiration_seek_interval(float capacity,
                                   uint32_t tau_min_ms,
                                   uint32_t tau_max_ms) {
  if (capacity < RESPIRATION_INHALE_THRESHOLD) {
    float scaled = powf(capacity / RESPIRATION_INHALE_THRESHOLD, RESPIRATION_ALPHA);
    float interval = (float)tau_min_ms +
                     ((float)tau_max_ms - (float)tau_min_ms) * scaled;
    return (uint64_t)interval;
  }
  return UINT64_MAX;
}
```

Add `#include <math.h>` if not present (for `powf`). Actually, `powf` may already be available — check the current implementation to see if it uses a custom computation or `powf`.

Looking at the current implementation at `respiration.c:19-20`:
```c
  float interval = (float)RESPIRATION_TAU_MIN_MS +
                   ((float)RESPIRATION_TAU_MAX_MS - (float)RESPIRATION_TAU_MIN_MS) * scaled;
```

So the current version doesn't use `powf`. I'll keep it as-is but replace the defines.

- [ ] **Step 2: Find and update all call sites**

Run: `grep -rn "respiration_seek_interval" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`

For each call site, pass the respiration actor's stored tau values. These values need to be stored in `respiration_actor_t`. If the respiration actor doesn't already have tau_min_ms / tau_max_ms fields, add them and set them during `respiration_actor_create()`.

Let `respiration_actor_t` store the values:

In `src/Network/respiration_actor.h` or wherever `respiration_actor_t` is defined, add:
```c
  uint32_t tau_min_ms;
  uint32_t tau_max_ms;
```

Update `respiration_actor_create()` to accept and store them, then update its call in `network.c`:

```c
  network->respiration = respiration_actor_create(network, pool,
      network->respiration_tau_min_ms, network->respiration_tau_max_ms);
```

- [ ] **Step 3: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -10`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/Network/respiration.h src/Network/respiration.c src/Network/respiration_actor.c src/Network/network.c
git commit -m "refactor: use config-based tau values in respiration instead of defines"
```

---

### Task 9: Wire relay client retry values

**Files:**
- Modify: `src/Network/relay_client.h:79-87`
- Modify: `src/Network/relay_client.c`
- Modify: `src/Network/network.c`

- [ ] **Step 1: Store retry values in relay_client_t struct**

In `src/Network/relay_client.h`, add fields to `relay_client_t`:

```c
  uint8_t max_retries;
  uint32_t retry_delay_ms;
```

- [ ] **Step 2: Update relay_client_create() signature**

In `src/Network/relay_client.h`:

```c
relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool,
                                    uint8_t max_retries, uint32_t retry_delay_ms);
```

Update `relay_client_create()` implementation in `relay_client.c` to store these values and use them instead of the `RELAY_CLIENT_MAX_RETRIES` and `RELAY_CLIENT_RETRY_DELAY_MS` defines:

```c
relay_client_t* relay_client_create(network_t* network, scheduler_pool_t* pool,
                                    uint8_t max_retries, uint32_t retry_delay_ms) {
  // ... existing init ...
  client->max_retries = max_retries;
  client->retry_delay_ms = retry_delay_ms;
  // ...
}
```

Replace all uses of `RELAY_CLIENT_MAX_RETRIES` with `client->max_retries` and `RELAY_CLIENT_RETRY_DELAY_MS` with `client->retry_delay_ms` in `relay_client.c`.

- [ ] **Step 3: Update relay_client_create call in network.c**

```c
  // When creating relay client in network.c:
  client->relay = relay_client_create(network, pool,
      network->relay_max_retries, network->relay_retry_delay_ms);
```

Update all other `relay_client_create` call sites:
Run: `grep -rn "relay_client_create" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c"`

- [ ] **Step 4: Build and verify**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -10`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/Network/relay_client.h src/Network/relay_client.c src/Network/network.c
git commit -m "refactor: use config-based retry values in relay_client instead of defines"
```

---

### Task 10: Remove old #define constants

**Files:**
- Modify: `src/Network/gossip.h:15-18`
- Modify: `src/Network/hebbian.h:18`
- Modify: `src/Network/eabf.h:22-23`
- Modify: `src/Network/respiration.h:15-16`
- Modify: `src/Network/relay_client.h:27-28`

- [ ] **Step 1: Remove the 11 defines that are now configurable**

In each file, remove the specified lines:
- `src/Network/gossip.h`: Remove lines 15-18 (`GOSSIP_INIT_INTERVAL_S`, `GOSSIP_INIT_COUNT`, `GOSSIP_STEADY_INTERVAL_S`, `GOSSIP_TIMEOUT_MS`)
- `src/Network/hebbian.h`: Remove line 18 (`HEBBIAN_DECAY_FACTOR`)
- `src/Network/eabf.h`: Remove lines 22-23 (`EABF_BASE_TTL_MS`, `EABF_MAINTENANCE_MS`)
- `src/Network/respiration.h`: Remove lines 15-16 (`RESPIRATION_TAU_MIN_MS`, `RESPIRATION_TAU_MAX_MS`)
- `src/Network/relay_client.h`: Remove lines 27-28 (`RELAY_CLIENT_MAX_RETRIES`, `RELAY_CLIENT_RETRY_DELAY_MS`)

- [ ] **Step 2: Verify no remaining references to removed defines**

Run: `grep -rn "GOSSIP_INIT_INTERVAL_S\|GOSSIP_INIT_COUNT\|GOSSIP_STEADY_INTERVAL_S\|GOSSIP_TIMEOUT_MS\|HEBBIAN_DECAY_FACTOR\|EABF_BASE_TTL_MS\|EABF_MAINTENANCE_MS\|RESPIRATION_TAU_MIN_MS\|RESPIRATION_TAU_MAX_MS\|RELAY_CLIENT_MAX_RETRIES\|RELAY_CLIENT_RETRY_DELAY_MS" /home/victor/Workspace/src/github.com/vijayee/liboffs/src --include="*.c" --include="*.h" 2>/dev/null`
Expected: No output (no remaining references in source files outside the removed lines).

Note: `GOSSIP_TIMEOUT_MS` may still appear as a fallback in `gossip.c` (lines 40, 41, 44, 88) which use the defines as fallback values. Since we're now always providing config values through `network_create()`, these fallbacks should be updated to use the caller-provided values directly. Update `gossip.c` if needed.

- [ ] **Step 3: Build and verify full compilation**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds with no warnings about undefined constants.

- [ ] **Step 4: Commit**

```bash
git add src/Network/gossip.h src/Network/hebbian.h src/Network/eabf.h src/Network/respiration.h src/Network/relay_client.h src/Network/gossip.c
git commit -m "refactor: remove hardcoded network defines replaced by config_t fields"
```

---

### Task 11: Create validation.h and validation.c

**Files:**
- Create: `src/Util/validation.h`
- Create: `src/Util/validation.c`

- [ ] **Step 1: Create validation.h**

```c
#ifndef OFFS_VALIDATION_H
#define OFFS_VALIDATION_H

#define OFFS_MAX_CONTENT_TYPE_LEN    256
#define OFFS_MAX_FILE_NAME_LEN       1024
#define OFFS_MAX_ORI_STRING_LEN      2048
#define OFFS_MAX_CBOR_MESSAGE_SIZE   (64 * 1024 * 1024)

int validate_content_type(const char* content_type);
int validate_file_name(const char* file_name);
int validate_ori_string(const char* ori);

#endif // OFFS_VALIDATION_H
```

- [ ] **Step 2: Create validation.c**

```c
#include "validation.h"
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

int validate_content_type(const char* content_type) {
  if (content_type == NULL || content_type[0] == '\0') return -1;
  size_t len = strlen(content_type);
  if (len > OFFS_MAX_CONTENT_TYPE_LEN) return -1;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)content_type[i];
    if (!isprint(c) && c != '/' && c != '-' && c != '+' && c != '.') return -1;
  }
  return 0;
}

int validate_file_name(const char* file_name) {
  if (file_name == NULL || file_name[0] == '\0') return -1;
  size_t len = strlen(file_name);
  if (len > OFFS_MAX_FILE_NAME_LEN) return -1;
  if (strstr(file_name, "..") != NULL) return -1;
  if (file_name[0] == '/') return -1;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)file_name[i];
    if (!isprint(c)) return -1;
  }
  return 0;
}

int validate_ori_string(const char* ori) {
  if (ori == NULL || ori[0] == '\0') return -1;
  size_t len = strlen(ori);
  if (len > OFFS_MAX_ORI_STRING_LEN) return -1;
  if (strncmp(ori, "http://", 7) != 0 && strncmp(ori, "https://", 8) != 0) return -1;
  if (strstr(ori, "/offsystem/v3/") == NULL) return -1;
  return 0;
}
```

- [ ] **Step 3: Add validation.c to CMakeLists.txt**

Find where other Util .c files are listed in `CMakeLists.txt`:
Run: `grep -n "Util/" /home/victor/Workspace/src/github.com/vijayee/liboffs/CMakeLists.txt`

Add `src/Util/validation.c` alongside the other Util source files.

- [ ] **Step 4: Build**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/Util/validation.h src/Util/validation.c CMakeLists.txt
git commit -m "feat: add input validation helpers (content_type, file_name, ori)"
```

---

### Task 12: Harden CBOR wire decoder

**Files:**
- Modify: `src/ClientAPI/client_api_wire.c`

- [ ] **Step 1: Add include and update _decode_string with max length**

Add `#include "../Util/validation.h"` at the top of `client_api_wire.c`.

Update `_decode_string` to accept a max length parameter:

```c
static char* _decode_string(cbor_item_t* item, size_t max_len) {
  if (!cbor_isa_string(item)) return NULL;
  size_t len = cbor_string_length(item);
  if (len == 0 || len > max_len) return NULL;
  char* str = get_memory(len + 1);
  memcpy(str, cbor_string_handle(item), len);
  str[len] = '\0';
  return str;
}
```

- [ ] **Step 2: Update all _decode_string call sites**

Replace all calls to `_decode_string(item)` with `_decode_string(item, 65536)` (64K general string limit). Then add specific validation for each field based on its type:

In `client_api_put_request_decode`:
```c
  msg->content_type = _decode_string(content_type, OFFS_MAX_CONTENT_TYPE_LEN);
  cbor_decref(&content_type);
  if (msg->content_type == NULL || validate_content_type(msg->content_type) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }

  msg->file_name = _decode_string(file_name, OFFS_MAX_FILE_NAME_LEN);
  cbor_decref(&file_name);
  if (msg->file_name == NULL || validate_file_name(msg->file_name) != 0) {
    client_api_put_request_destroy(msg);
    return -1;
  }

  cbor_item_t* stream_length = cbor_array_get(item, 3);
  msg->stream_length = (size_t)cbor_get_uint64(stream_length);
  cbor_decref(&stream_length);
  if (msg->stream_length > OFFS_MAX_CBOR_MESSAGE_SIZE) {
    client_api_put_request_destroy(msg);
    return -1;
  }

  // ... server_address with 65536 limit ...

  if (cbor_array_size(item) >= 6) {
    // ... data with OFFS_MAX_CBOR_MESSAGE_SIZE limit ...
```

In `client_api_get_request_decode`:
```c
  cbor_item_t* ori = cbor_array_get(item, 1);
  msg->ori_string = _decode_string(ori, OFFS_MAX_ORI_STRING_LEN);
  cbor_decref(&ori);
  if (msg->ori_string == NULL || validate_ori_string(msg->ori_string) != 0) {
    client_api_get_request_destroy(msg);
    return -1;
  }
```

In `client_api_get_response_start_decode`:
```c
  cbor_item_t* content_type = cbor_array_get(item, 1);
  msg->content_type = _decode_string(content_type, OFFS_MAX_CONTENT_TYPE_LEN);
  cbor_decref(&content_type);
  if (msg->content_type == NULL || validate_content_type(msg->content_type) != 0) {
    client_api_get_response_start_destroy(msg);
    return -1;
  }
```

In `client_api_error_decode`:
```c
  cbor_item_t* message = cbor_array_get(item, 2);
  msg->message = _decode_string(message, 65536);
  cbor_decref(&message);
```

- [ ] **Step 3: Build**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -10`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPI/client_api_wire.c
git commit -m "feat: add input validation in CBOR wire decoder"
```

---

### Task 13: Harden HTTP PUT handlers

**Files:**
- Modify: `src/ClientAPI/HTTP/off_routes.c:639-718, 732-798`

- [ ] **Step 1: Add include**

Add `#include "../../Util/validation.h"` near the top of `off_routes.c`.

- [ ] **Step 2: Add validation in _off_put_handler()**

After the existing NULL check for type/file_name/stream_length_str (line 648-652), add content validation before proceeding:

```c
    if (!type || !file_name || !stream_length_str) {
        http_response_set_status(response, 400);
        http_response_write(response, "Missing required headers: type, file-name, stream-length", 56);
        http_response_end(response);
        return;
    }

    if (validate_content_type(type) != 0) {
        http_response_set_status(response, 400);
        http_response_write(response, "Invalid content type", 20);
        http_response_end(response);
        return;
    }
    if (validate_file_name(file_name) != 0) {
        http_response_set_status(response, 400);
        http_response_write(response, "Invalid file name", 17);
        http_response_end(response);
        return;
    }
```

Also validate `stream_length_str` is a valid positive integer by checking that `atol` produced a non-zero result and that the string only contains digits (it already checks `stream_length == 0` on line 656, which catches that case).

- [ ] **Step 3: Add same validation in _off_put_headers_complete()**

After the existing NULL check (line 746-748), add the same content_type and file_name validation before the streaming setup.

- [ ] **Step 4: Build and run relevant tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds.

Note: Don't run `TestOffRoutes` — it has a pre-existing crash (SIGSEGV) unrelated to our changes.

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPI/HTTP/off_routes.c
git commit -m "feat: validate content_type and file_name headers in HTTP PUT handlers"
```

---

### Task 14: Harden URL parser

**Files:**
- Modify: `src/OFFStreams/off_url.c:36-169`

- [ ] **Step 1: Add include and add length validation**

Add `#include "../Util/validation.h"` at the top of `off_url.c`.

In `off_url_parse()`, after decoding `content_type` (around line 89-91), validate:

```c
    decoded_type[decoded_type_len] = '\0';
    free(content_type_raw);

    if (validate_content_type(decoded_type) != 0) {
        free(decoded_type);
        free(server_address);
        return NULL;
    }
```

After decoding `file_name` (around line 132), validate:

```c
    file_name[decoded_name_len] = '\0';
    free(name_raw);

    if (validate_file_name(file_name) != 0) {
        buffer_destroy(file_hash);
        free(file_hash_b58);
        free(descriptor_hash_b58);
        free(file_name);
        free(decoded_type);
        free(server_address);
        return NULL;
    }
```

- [ ] **Step 2: Build**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/OFFStreams/off_url.c
git commit -m "feat: validate content_type and file_name lengths in URL parser"
```

---

### Task 15: Test config_validate()

**Files:**
- Create: `test/test_config_validate.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create test file**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Configuration/config.h"
}

TEST(TestConfigValidate, DefaultConfigIsValid) {
  config_t config = config_default();
  EXPECT_EQ(config_validate(&config), 0);
}

TEST(TestConfigValidate, NullConfigFails) {
  EXPECT_EQ(config_validate(NULL), -1);
}

TEST(TestConfigValidate, ZeroIndexBucketSizeFails) {
  config_t config = config_default();
  config.index_bucket_size = 0;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, MaxWaitLessThanWaitFails) {
  config_t config = config_default();
  config.index_max_wait = 3;
  config.index_wait = 5;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, MinTupleLargerThanMaxFails) {
  config_t config = config_default();
  config.min_tuple_size = 10;
  config.max_tuple_size = 5;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, LruLargerThanCacheFails) {
  config_t config = config_default();
  config.lru_size = 100;
  config.cache_size = 50;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, ZeroSchedulerThreadsFails) {
  config_t config = config_default();
  config.scheduler_thread_count = 0;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, TooManySchedulerThreadsFails) {
  config_t config = config_default();
  config.scheduler_thread_count = 257;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, ZeroGossipIntervalFails) {
  config_t config = config_default();
  config.gossip_init_interval_s = 0;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, SteadyLessThanInitIntervalFails) {
  config_t config = config_default();
  config.gossip_init_interval_s = 60;
  config.gossip_steady_interval_s = 30;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, HebDecayOutOfRangeFails) {
  config_t config = config_default();
  config.hebbian_decay_factor = 1.5f;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, EabfBaseTtlLessThanMaintenanceFails) {
  config_t config = config_default();
  config.eabf_base_ttl_ms = 30000;
  config.eabf_maintenance_ms = 60000;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, RespirationTauMaxLessThanMinFails) {
  config_t config = config_default();
  config.respiration_tau_min_ms = 100000;
  config.respiration_tau_max_ms = 50000;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, ZeroRelayRetriesFails) {
  config_t config = config_default();
  config.relay_max_retries = 0;
  EXPECT_EQ(config_validate(&config), -1);
}

TEST(TestConfigValidate, ZeroRelayRetryDelayFails) {
  config_t config = config_default();
  config.relay_retry_delay_ms = 0;
  EXPECT_EQ(config_validate(&config), -1);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

In `test/CMakeLists.txt`, find the `add_executable(testliboffs` block and add `test_config_validate.cpp` to the list.

- [ ] **Step 3: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make -j$(nproc) 2>&1 | tail -5 && ./test/testliboffs --gtest_filter='*ConfigValidate*' 2>&1`
Expected: All 14 tests pass.

- [ ] **Step 4: Run under valgrind**

Run: `valgrind --leak-check=full ./test/testliboffs --gtest_filter='*ConfigValidate*' 2>&1 | tail -10`
Expected: No leaks.

- [ ] **Step 5: Commit**

```bash
git add test/test_config_validate.cpp test/CMakeLists.txt
git commit -m "test: add config_validate tests for all 26 fields"
```

---

### Task 16: Test input validation helpers

**Files:**
- Create: `test/test_validation.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create test file**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Util/validation.h"
}

TEST(TestValidation, ContentTypeNullFails) {
  EXPECT_EQ(validate_content_type(NULL), -1);
}

TEST(TestValidation, ContentTypeEmptyFails) {
  EXPECT_EQ(validate_content_type(""), -1);
}

TEST(TestValidation, ContentTypeTooLongFails) {
  char long_type[258];
  memset(long_type, 'a', 257);
  long_type[257] = '\0';
  EXPECT_EQ(validate_content_type(long_type), -1);
}

TEST(TestValidation, ContentTypeValidPasses) {
  EXPECT_EQ(validate_content_type("application/octet-stream"), 0);
  EXPECT_EQ(validate_content_type("text/plain"), 0);
  EXPECT_EQ(validate_content_type("image/png"), 0);
}

TEST(TestValidation, ContentTypeControlCharsFails) {
  EXPECT_EQ(validate_content_type("text\nplain"), -1);
}

TEST(TestValidation, FileNameNullFails) {
  EXPECT_EQ(validate_file_name(NULL), -1);
}

TEST(TestValidation, FileNameEmptyFails) {
  EXPECT_EQ(validate_file_name(""), -1);
}

TEST(TestValidation, FileNameTooLongFails) {
  char long_name[1026];
  memset(long_name, 'f', 1025);
  long_name[1025] = '\0';
  EXPECT_EQ(validate_file_name(long_name), -1);
}

TEST(TestValidation, FileNameDotDotFails) {
  EXPECT_EQ(validate_file_name("../etc/passwd"), -1);
  EXPECT_EQ(validate_file_name("foo/../../bar"), -1);
}

TEST(TestValidation, FileNameLeadingSlashFails) {
  EXPECT_EQ(validate_file_name("/etc/passwd"), -1);
}

TEST(TestValidation, FileNameValidPasses) {
  EXPECT_EQ(validate_file_name("document.pdf"), 0);
  EXPECT_EQ(validate_file_name("my file.txt"), 0);
  EXPECT_EQ(validate_file_name("report-2026.csv"), 0);
}

TEST(TestValidation, OriStringNullFails) {
  EXPECT_EQ(validate_ori_string(NULL), -1);
}

TEST(TestValidation, OriStringEmptyFails) {
  EXPECT_EQ(validate_ori_string(""), -1);
}

TEST(TestValidation, OriStringNoOffsystemFails) {
  EXPECT_EQ(validate_ori_string("http://localhost/test"), -1);
}

TEST(TestValidation, OriStringNoHttpFails) {
  EXPECT_EQ(validate_ori_string("ftp://localhost/offsystem/v3/foo/1/a/b/c"), -1);
}

TEST(TestValidation, OriStringValidPasses) {
  EXPECT_EQ(validate_ori_string("http://localhost:23402/offsystem/v3/standard/123/abc/def/file.txt"), 0);
  EXPECT_EQ(validate_ori_string("https://example.com/offsystem/v3/app/image/456/ghi/jkl/photo.jpg"), 0);
}

TEST(TestValidation, OriStringTooLongFails) {
  char long_ori[2050];
  memset(long_ori, 'x', 2049);
  long_ori[2049] = '\0';
  memcpy(long_ori, "http://x/offsystem/v3/", 21);
  EXPECT_EQ(validate_ori_string(long_ori), -1);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add `test_validation.cpp` to the testliboffs sources in `test/CMakeLists.txt`.

- [ ] **Step 3: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make -j$(nproc) 2>&1 | tail -5 && ./test/testliboffs --gtest_filter='*Validation*' 2>&1`
Expected: All 18 tests pass.

- [ ] **Step 4: Run under valgrind**

Run: `valgrind --leak-check=full ./test/testliboffs --gtest_filter='*Validation*' 2>&1 | tail -10`
Expected: No leaks.

- [ ] **Step 5: Commit**

```bash
git add test/test_validation.cpp test/CMakeLists.txt
git commit -m "test: add input validation helper tests"
```

---

### Task 17: Test wire decoder validation rejection

**Files:**
- Create: `test/test_wire_validation.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create test file**

```cpp
#include <gtest/gtest.h>
#include <cbor.h>
extern "C" {
#include "../src/ClientAPI/client_api_wire.h"
}

TEST(TestWireValidation, PutRequestRejectsEmptyContentType) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_PUT_REQUEST));
  cbor_array_push(array, cbor_build_string(""));       // empty content_type
  cbor_array_push(array, cbor_build_string("file.txt"));
  cbor_array_push(array, cbor_build_uint64(100));
  client_api_put_request_t msg;
  int result = client_api_put_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}

TEST(TestWireValidation, PutRequestRejectsEmptyFileName) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_PUT_REQUEST));
  cbor_array_push(array, cbor_build_string("text/plain"));
  cbor_array_push(array, cbor_build_string(""));       // empty file_name
  cbor_array_push(array, cbor_build_uint64(100));
  client_api_put_request_t msg;
  int result = client_api_put_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}

TEST(TestWireValidation, PutRequestRejectsOversizedStreamLength) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_PUT_REQUEST));
  cbor_array_push(array, cbor_build_string("text/plain"));
  cbor_array_push(array, cbor_build_string("file.txt"));
  cbor_array_push(array, cbor_build_uint64(OFFS_MAX_CBOR_MESSAGE_SIZE + 1));
  client_api_put_request_t msg;
  int result = client_api_put_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}

TEST(TestWireValidation, GetRequestRejectsInvalidOri) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_GET_REQUEST));
  cbor_array_push(array, cbor_build_string("not-a-valid-ori"));
  client_api_get_request_t msg;
  int result = client_api_get_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}

TEST(TestWireValidation, GetRequestAcceptsValidOri) {
  cbor_item_t* array = cbor_new_definite_array(2);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_GET_REQUEST));
  cbor_array_push(array, cbor_build_string("http://localhost:23402/offsystem/v3/standard/123/abc/def/file.txt"));
  client_api_get_request_t msg;
  int result = client_api_get_request_decode(array, &msg);
  EXPECT_EQ(result, 0);
  client_api_get_request_destroy(&msg);
  cbor_decref(&array);
}

TEST(TestWireValidation, PutRequestRejectsInvalidContentType) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_PUT_REQUEST));
  cbor_array_push(array, cbor_build_string("text\0plain"));  // embedded null
  cbor_array_push(array, cbor_build_string("file.txt"));
  cbor_array_push(array, cbor_build_uint64(100));
  client_api_put_request_t msg;
  int result = client_api_put_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}

TEST(TestWireValidation, PutRequestRejectsPathTraversalFileName) {
  cbor_item_t* array = cbor_new_definite_array(4);
  cbor_array_push(array, cbor_build_uint8(CLIENT_API_PUT_REQUEST));
  cbor_array_push(array, cbor_build_string("text/plain"));
  cbor_array_push(array, cbor_build_string("../etc/passwd"));
  cbor_array_push(array, cbor_build_uint64(100));
  client_api_put_request_t msg;
  int result = client_api_put_request_decode(array, &msg);
  EXPECT_EQ(result, -1);
  cbor_decref(&array);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add `test_wire_validation.cpp` to the testliboffs sources in `test/CMakeLists.txt`.

- [ ] **Step 3: Build and run tests**

Run: `cd /home/victor/Workspace/src/github.com/vijayee/liboffs/build && cmake .. && make -j$(nproc) 2>&1 | tail -5 && ./test/testliboffs --gtest_filter='*WireValidation*' 2>&1`
Expected: All 7 tests pass.

- [ ] **Step 4: Run under valgrind**

Run: `valgrind --leak-check=full ./test/testliboffs --gtest_filter='*WireValidation*' 2>&1 | tail -10`
Expected: No leaks.

- [ ] **Step 5: Commit**

```bash
git add test/test_wire_validation.cpp test/CMakeLists.txt
git commit -m "test: add wire decoder validation rejection tests"
```
