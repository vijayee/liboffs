//
// Created by victor on 5/28/25.
//

#include "version.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool version_parse(const char* tag, version_t* out) {
  if (tag == NULL || out == NULL) {
    return false;
  }

  const char* cursor = tag;

  // Skip optional leading 'v'
  if (*cursor == 'v') {
    cursor++;
  }

  // Parse major
  char* end = NULL;
  unsigned long major = strtoul(cursor, &end, 10);
  if (end == cursor || major > UINT16_MAX) {
    return false;
  }
  if (*end != '.') {
    return false;
  }
  out->major = (uint16_t)major;

  // Parse minor
  cursor = end + 1;
  unsigned long minor = strtoul(cursor, &end, 10);
  if (end == cursor || minor > UINT16_MAX) {
    return false;
  }
  if (*end != '.') {
    return false;
  }
  out->minor = (uint16_t)minor;

  // Parse patch
  cursor = end + 1;
  unsigned long patch = strtoul(cursor, &end, 10);
  if (end == cursor || patch > UINT16_MAX) {
    return false;
  }
  out->patch = (uint16_t)patch;

  // Initialize prerelease to empty
  out->prerelease[0] = '\0';

  // Check for prerelease suffix after '-'
  if (*end == '-') {
    cursor = end + 1;
    if (*cursor == '\0') {
      return false;
    }

    size_t prerelease_len = 0;
    while (*cursor != '\0' && prerelease_len < sizeof(out->prerelease) - 1) {
      out->prerelease[prerelease_len++] = *cursor++;
    }
    out->prerelease[prerelease_len] = '\0';

    // Reject if prerelease was truncated (input too long)
    if (*cursor != '\0') {
      out->prerelease[0] = '\0';
      return false;
    }
  } else if (*end != '\0') {
    // Trailing characters that are not a prerelease suffix
    return false;
  }

  return true;
}

int version_compare(const version_t* a, const version_t* b) {
  if (a == NULL || b == NULL) {
    return 0;
  }

  // Compare major
  if (a->major < b->major) {
    return -1;
  }
  if (a->major > b->major) {
    return 1;
  }

  // Compare minor
  if (a->minor < b->minor) {
    return -1;
  }
  if (a->minor > b->minor) {
    return 1;
  }

  // Compare patch
  if (a->patch < b->patch) {
    return -1;
  }
  if (a->patch > b->patch) {
    return 1;
  }

  // Compare prerelease: stable > prerelease
  bool a_has_prerelease = a->prerelease[0] != '\0';
  bool b_has_prerelease = b->prerelease[0] != '\0';

  if (!a_has_prerelease && b_has_prerelease) {
    return 1;
  }
  if (a_has_prerelease && !b_has_prerelease) {
    return -1;
  }
  if (!a_has_prerelease && !b_has_prerelease) {
    return 0;
  }

  // Both have prerelease: lexicographic comparison
  int cmp = strcmp(a->prerelease, b->prerelease);
  if (cmp < 0) {
    return -1;
  }
  if (cmp > 0) {
    return 1;
  }
  return 0;
}

size_t version_to_string(const version_t* v, char* buf, size_t buf_size) {
  if (v == NULL || buf == NULL || buf_size == 0) {
    return 0;
  }

  if (v->prerelease[0] != '\0') {
    return (size_t)snprintf(buf, buf_size, "%u.%u.%u-%s",
                            v->major, v->minor, v->patch, v->prerelease);
  }

  return (size_t)snprintf(buf, buf_size, "%u.%u.%u",
                          v->major, v->minor, v->patch);
}

update_channel_e version_channel(const version_t* v) {
  if (v == NULL) {
    return channel_stable;
  }

  if (v->prerelease[0] == '\0') {
    return channel_stable;
  }

  if (strstr(v->prerelease, "rc") != NULL) {
    return channel_rc;
  }

  if (strstr(v->prerelease, "dev") != NULL ||
      strstr(v->prerelease, "alpha") != NULL) {
    return channel_dev;
  }

  return channel_dev;
}

const char* channel_to_string(update_channel_e channel) {
  switch (channel) {
    case channel_stable:
      return "stable";
    case channel_rc:
      return "rc";
    case channel_dev:
      return "dev";
  }
  return "stable";
}
