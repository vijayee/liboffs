//
// Created by victor on 5/28/25.
//

#ifndef OFFS_VERSION_H
#define OFFS_VERSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  char prerelease[32];
} version_t;

typedef enum {
  channel_stable = 0,
  channel_rc = 1,
  channel_dev = 2
} update_channel_e;

bool          version_parse(const char* tag, version_t* out);
int           version_compare(const version_t* a, const version_t* b);
size_t        version_to_string(const version_t* v, char* buf, size_t buf_size);
update_channel_e version_channel(const version_t* v);
const char*   channel_to_string(update_channel_e channel);

#ifndef OFFS_VERSION
#define OFFS_VERSION "0.0.0"
#endif

#endif // OFFS_VERSION_H
