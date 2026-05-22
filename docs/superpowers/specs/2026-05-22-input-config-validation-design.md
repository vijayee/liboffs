# Input Validation & Configuration Validation — Design

Date: 2026-05-22

## Overview

Add configurable network parameters to `config_t`, validate all config fields at node startup, and
harden input validation at API boundaries (CBOR wire decoder, HTTP handlers, URL parser).

Covers OFFS-134 (input validation) and OFFS-135 (configuration validation) from the Reliability
epic, plus 12 newly identified configurable network parameters found during audit.

## Part 1: New config_t Fields

Twelve new fields in `src/Configuration/config.h`, replacing hardcoded `#define` constants with
tunable runtime values:

| Field | Type | Default | Replaces |
|-------|------|---------|----------|
| `scheduler_thread_count` | `size_t` | 4 | Hardcoded literal in `offs_node_create` |
| `gossip_init_interval_s` | `uint32_t` | 2 | `GOSSIP_INIT_INTERVAL_S` |
| `gossip_init_count` | `size_t` | 5 | `GOSSIP_INIT_COUNT` |
| `gossip_steady_interval_s` | `uint32_t` | 30 | `GOSSIP_STEADY_INTERVAL_S` |
| `gossip_timeout_ms` | `uint32_t` | 5000 | `GOSSIP_TIMEOUT_MS` |
| `hebbian_decay_factor` | `float` | 0.999 | `HEBBIAN_DECAY_FACTOR` |
| `eabf_base_ttl_ms` | `uint32_t` | 3600000 | `EABF_BASE_TTL_MS` |
| `eabf_maintenance_ms` | `uint32_t` | 60000 | `EABF_MAINTENANCE_MS` |
| `respiration_tau_min_ms` | `uint32_t` | 5000 | `RESPIRATION_TAU_MIN_MS` |
| `respiration_tau_max_ms` | `uint32_t` | 300000 | `RESPIRATION_TAU_MAX_MS` |
| `relay_max_retries` | `size_t` | 5 | `RELAY_CLIENT_MAX_RETRIES` |
| `relay_retry_delay_ms` | `uint32_t` | 500 | `RELAY_CLIENT_RETRY_DELAY_MS` |

Defaults match current hardcoded values exactly — no behavioral change.

## Part 2: Config Validation

New function `int config_validate(const config_t* config)` in `src/Configuration/config.c`.
Returns 0 on success, -1 on first violation (logging each failure).

### Validation Rules

**Existing fields:**
| Field | Rule |
|-------|------|
| `index_bucket_size` | > 0 |
| `index_wait` | > 0 |
| `index_max_wait` | >= index_wait |
| `section_size` | > 0 |
| `section_cache_count` | > 0 |
| `section_wait` | > 0 |
| `section_max_wait` | >= section_wait |
| `cache_size` | > 0 |
| `max_tuple_size` | >= min_tuple_size |
| `min_tuple_size` | > 0 |
| `lru_size` | > 0, <= cache_size |
| `descriptor_pad` | > 0 |
| `max_snapshots` | > 0 |
| `max_wals` | > 0 |
| `max_capacity_bytes` | > 0 |
| `shutdown_timeout_ms` | any value valid (0 = no timeout) |

**New fields:**
| Field | Rule |
|-------|------|
| `scheduler_thread_count` | > 0, <= 256 |
| `gossip_init_interval_s` | > 0 |
| `gossip_init_count` | > 0 |
| `gossip_steady_interval_s` | >= gossip_init_interval_s |
| `gossip_timeout_ms` | > 0 |
| `hebbian_decay_factor` | > 0.0, < 1.0 |
| `eabf_base_ttl_ms` | >= eabf_maintenance_ms |
| `eabf_maintenance_ms` | > 0 |
| `respiration_tau_min_ms` | > 0 |
| `respiration_tau_max_ms` | >= respiration_tau_min_ms |
| `relay_max_retries` | > 0 |
| `relay_retry_delay_ms` | > 0 |

### Call Site

Called at the top of `offs_node_start()` in `src/Node/node.c`, before any subsystem initialization.
If validation fails, node start returns -1 immediately.

## Part 3: Wiring New Config Fields to Consumers

Remove the corresponding `#define` constants and pass values through constructors:

### network_t additions

Add fields to `network_t` in `src/Network/network.h` to hold the config values:

```c
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

### network_create() signature change

Add config values as parameters. `network_create()` stores them in `network_t` fields and passes
them to subsystem constructors:
- `gossip_handle_start()` — interval, count, steady interval, timeout
- `hebbian_table_init()` — decay factor
- `eabf_table_init()` — base TTL, maintenance interval
- `respiration_actor_create()` — tau min, tau max
- `relay_client_create()` — max retries, retry delay

### Defines to remove

- `src/Network/gossip.h` — `GOSSIP_INIT_INTERVAL_S`, `GOSSIP_INIT_COUNT`, `GOSSIP_STEADY_INTERVAL_S`, `GOSSIP_TIMEOUT_MS`
- `src/Network/hebbian.h` — `HEBBIAN_DECAY_FACTOR`
- `src/Network/eabf.h` — `EABF_BASE_TTL_MS`, `EABF_MAINTENANCE_MS`
- `src/Network/respiration.h` — `RESPIRATION_TAU_MIN_MS`, `RESPIRATION_TAU_MAX_MS`
- `src/Network/relay_client.h` — `RELAY_CLIENT_MAX_RETRIES`, `RELAY_CLIENT_RETRY_DELAY_MS`

### node.c change

Replace `scheduler_pool_create(4)` with `scheduler_pool_create(node->config->scheduler_thread_count)`.

## Part 4: Input Validation

### New File: `src/Util/validation.h` + `src/Util/validation.c`

Constants and helper functions for input validation:

```c
#define OFFS_MAX_CONTENT_TYPE_LEN    256
#define OFFS_MAX_FILE_NAME_LEN       1024
#define OFFS_MAX_ORI_STRING_LEN      2048
#define OFFS_MAX_CBOR_MESSAGE_SIZE   (64 * 1024 * 1024)  // 64MB

int validate_content_type(const char* content_type);
int validate_file_name(const char* file_name);
int validate_ori_string(const char* ori);
```

- `validate_content_type`: non-NULL, non-empty, length <= 256, printable ASCII + `/` `-` `+` `.` only
- `validate_file_name`: non-NULL, non-empty, length <= 1024, no `..` or `/` path traversal, printable chars
- `validate_ori_string`: non-NULL, non-empty, length <= 2048, starts with `http://` or `https://`, contains `/offsystem/v3/`

### Wire Decoder Hardening (`src/ClientAPI/client_api_wire.c`)

- `_decode_string()`: add max length parameter, reject strings exceeding limit
- `client_api_put_request_decode()`:
  - Validate `content_type` and `file_name` after decode, reject if invalid
  - Reject empty `content_type` or `file_name`
  - Reject `stream_length` > `OFFS_MAX_CBOR_MESSAGE_SIZE`
  - Reject `data_size` > `OFFS_MAX_CBOR_MESSAGE_SIZE`
- `client_api_get_request_decode()`: validate `ori_string` after decode
- `client_api_get_response_start_decode()`: validate `content_type` after decode
- Add overflow guards on all `cbor_get_uint64` → `size_t` casts

### HTTP Handler Hardening (`src/ClientAPI/HTTP/off_routes.c`)

In PUT handlers (`_handle_put_start` and `_handle_put_multipart_start`):
- Validate `Content-Type` header against `validate_content_type()`
- Validate `file-name` header against `validate_file_name()`
- Validate `Stream-Length` header parses as positive integer
- Return 400 Bad Request on validation failure

### URL Parser Hardening (`src/OFFStreams/off_url.c`)

- `off_url_parse()`: validate decoded `content_type` length <= `OFFS_MAX_CONTENT_TYPE_LEN` and `file_name` length <= `OFFS_MAX_FILE_NAME_LEN`, return NULL if exceeded
