//
// Created by victor on 5/26/26.
//

#include "offs_ofd_resolver.h"
#include "offs_client.h"
#include "../../OFFStreams/ofd.h"
#include "../../OFFStreams/off_url.h"
#include "../../OFFStreams/ori.h"
#include "../../Buffer/buffer.h"
#include "../../Util/allocator.h"
#include "../../RefCounter/refcounter.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Block sizing ---- */

static size_t _block_size_for_type(int type) {
  switch ((block_size_e)type) {
    case mega:     return 1000000;
    case standard: return 128000;
    case mini:     return 64000;
    case nano:     return 136;
  }
  return 128000;
}

size_t offs_ofd_total_blocks(size_t final_byte, int block_type, size_t tuple_size) {
  size_t bs = _block_size_for_type(block_type);
  size_t descriptor_pad = 32;  /* SHA-256 hash size */

  size_t data_blocks = (final_byte + bs - 1) / bs;
  size_t cut_point = (bs / descriptor_pad) * descriptor_pad;
  size_t desc_data_per_blk = cut_point - descriptor_pad;
  size_t desc_bytes = data_blocks * descriptor_pad * tuple_size;
  size_t desc_blocks = (desc_bytes + desc_data_per_blk - 1) / desc_data_per_blk;

  return data_blocks + desc_blocks;
}

/* ---- URL building from OFD entries ---- */

static char* _build_ori_url(const char* server_base, const char* content_type,
                             size_t stream_length, const buffer_t* file_hash,
                             const buffer_t* descriptor_hash, const char* file_name) {
  /* Build an off_url_t and serialize it */
  off_url_t url;
  memset(&url, 0, sizeof(url));
  url.server_address = (char*)server_base;
  url.content_type = (char*)content_type;
  url.stream_length = stream_length;
  url.stream_offset = 0;
  url.file_hash = (buffer_t*)file_hash;
  url.descriptor_hash = (buffer_t*)descriptor_hash;
  url.file_name = (char*)file_name;

  return off_url_to_string(&url);
}

/* Extract server base from an offs URL like "http://host:port/offsystem/v3/..."
   Returns "http://host:port"  */
static char* _extract_server_base(const char* url) {
  const char* prefix = "http://";
  if (strncmp(url, prefix, 7) != 0) return NULL;
  const char* host_start = url + 7;
  const char* slash = strchr(host_start, '/');
  size_t base_len;
  if (slash) {
    base_len = (size_t)(slash - url);
  } else {
    base_len = strlen(url);
  }
  char* base = get_clear_memory(base_len + 1);
  memcpy(base, url, base_len);
  return base;
}

/* ---- OFD resolution ---- */

/* Build a sub-OFD URL from a directory entry's hash */
static char* _build_sub_ofd_url(const char* server_base, const buffer_t* dir_hash,
                                 const char* entry_name) {
  /* Build filename: <entry_name>.ofd */
  size_t base_name_len = strlen(entry_name);
  char* ofd_name = get_clear_memory(base_name_len + 5);
  memcpy(ofd_name, entry_name, base_name_len);
  memcpy(ofd_name + base_name_len, ".ofd", 4);

  off_url_t url;
  memset(&url, 0, sizeof(url));
  url.server_address = (char*)server_base;
  url.content_type = "offsystem/directory";
  url.stream_length = 0;
  url.stream_offset = 0;
  url.file_hash = (buffer_t*)dir_hash;
  url.descriptor_hash = (buffer_t*)dir_hash;
  url.file_name = ofd_name;

  char* url_str = off_url_to_string(&url);
  free(ofd_name);
  return url_str;
}

/* Recursive helper: append file entries from an OFD to the donors array */
static int _resolve_ofd_entries(ofd_t* ofd, const char* server_base,
                                 offs_ofd_donor_t** donors, size_t* donor_count,
                                 size_t* donor_cap) {
  for (size_t i = 0; i < ofd->entries.length; i++) {
    ofd_entry_t* entry = &ofd->entries.data[i];

    if (entry->type == OFD_ENTRY_FILE && entry->file_ori) {
      ori_t* ori = entry->file_ori;

      /* Build ORI URL */
      char* ori_url = _build_ori_url(server_base, "application/octet-stream",
                                      ori->final_byte, ori->file_hash,
                                      ori->descriptor_hash, entry->name);
      if (!ori_url) continue;

      /* Grow donors array if needed */
      if (*donor_count >= *donor_cap) {
        size_t new_cap = *donor_cap ? *donor_cap * 2 : 16;
        offs_ofd_donor_t* new_donors = realloc(*donors, new_cap * sizeof(offs_ofd_donor_t));
        if (!new_donors) {
          free(ori_url);
          return -1;
        }
        *donors = new_donors;
        *donor_cap = new_cap;
      }

      offs_ofd_donor_t* donor = &(*donors)[*donor_count];
      memset(donor, 0, sizeof(*donor));
      donor->name = strdup(entry->name);
      if (!donor->name) {
        free(ori_url);
        return -1;
      }
      if (!ori->file_hash || !ori->file_hash->data) {
        free(donor->name);
        free(ori_url);
        continue;
      }
      donor->url = ori_url;
      donor->final_byte = ori->final_byte;
      donor->block_type = (int)ori->block_type;
      donor->tuple_size = ori->tuple_size;
      donor->hash_len = ori->file_hash->size;
      donor->file_hash = get_clear_memory(ori->file_hash->size);
      if (!donor->file_hash) {
        free(donor->name);
        free(ori_url);
        return -1;
      }
      memcpy(donor->file_hash, ori->file_hash->data, ori->file_hash->size);
      (*donor_count)++;
    }
  }

  /* Recurse into subdirectories */
  for (size_t i = 0; i < ofd->entries.length; i++) {
    ofd_entry_t* entry = &ofd->entries.data[i];
    if (entry->type == OFD_ENTRY_DIRECTORY && entry->dir_hash) {
      char* sub_url = _build_sub_ofd_url(server_base, entry->dir_hash, entry->name);
      if (!sub_url) continue;

      /* Append ?ofd=raw */
      size_t sub_url_len = strlen(sub_url);
      char* raw_url = get_clear_memory(sub_url_len + 10);
      memcpy(raw_url, sub_url, sub_url_len);
      memcpy(raw_url + sub_url_len, "?ofd=raw", 9);
      free(sub_url);

      buffer_t* raw_data = offs_http_get(raw_url);
      free(raw_url);

      if (raw_data) {
        ofd_t* sub_ofd = ofd_decode(raw_data);
        if (sub_ofd) {
          if (_resolve_ofd_entries(sub_ofd, server_base, donors, donor_count, donor_cap) != 0) {
            fprintf(stderr, "[WARN] Failed to resolve sub-OFD entry '%s'\n", entry->name);
          }
          DESTROY(sub_ofd, ofd);
        }
        DESTROY(raw_data, buffer);
      }
    }
  }

  return 0;
}

offs_ofd_donor_t* offs_ofd_resolve(const char* ofd_url, size_t* out_count) {
  if (!ofd_url || !out_count) return NULL;
  *out_count = 0;

  /* Append ?ofd=raw */
  size_t url_len = strlen(ofd_url);
  char* raw_url = get_clear_memory(url_len + 10);
  memcpy(raw_url, ofd_url, url_len);
  if (strchr(ofd_url, '?')) {
    memcpy(raw_url + url_len, "&ofd=raw", 9);
  } else {
    memcpy(raw_url + url_len, "?ofd=raw", 9);
  }

  buffer_t* raw_data = offs_http_get(raw_url);
  free(raw_url);
  if (!raw_data) return NULL;

  ofd_t* ofd = ofd_decode(raw_data);
  DESTROY(raw_data, buffer);
  if (!ofd) return NULL;

  char* server_base = _extract_server_base(ofd_url);
  if (!server_base) {
    DESTROY(ofd, ofd);
    return NULL;
  }

  offs_ofd_donor_t* donors = NULL;
  size_t donor_count = 0;
  size_t donor_cap = 0;

  int rc = _resolve_ofd_entries(ofd, server_base, &donors, &donor_count, &donor_cap);
  free(server_base);
  DESTROY(ofd, ofd);

  if (rc != 0) {
    offs_ofd_free_donors(donors, donor_count);
    return NULL;
  }

  *out_count = donor_count;
  return donors;
}

/* ---- Donor selection ---- */

/* Compare donors by total_blocks descending for qsort */
static int _donor_block_cmp(const void* a, const void* b) {
  const offs_ofd_donor_t* da = (const offs_ofd_donor_t*)a;
  const offs_ofd_donor_t* db = (const offs_ofd_donor_t*)b;
  size_t ba = offs_ofd_total_blocks(da->final_byte, da->block_type, da->tuple_size);
  size_t bb = offs_ofd_total_blocks(db->final_byte, db->block_type, db->tuple_size);
  if (ba > bb) return -1;
  if (ba < bb) return 1;
  return 0;
}

/* Check if two raw hashes are equal (constant-time). */
static int _hash_eq(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
  if (a_len != b_len) return 0;
  uint8_t result = 0;
  for (size_t i = 0; i < a_len; i++) {
    result |= a[i] ^ b[i];
  }
  return result == 0;
}

char** offs_ofd_build_recyclers(
    const offs_ofd_donor_t* donors, size_t donor_count,
    const offs_ofd_donor_t* matched,
    size_t file_size,
    const char** fallback_urls, size_t fallback_count,
    size_t* out_count)
{
  if (!out_count) return NULL;
  *out_count = 0;
  if (!donors && donor_count > 0) return NULL;

  /* Estimate max output size: matched + all donors + fallbacks */
  size_t max_out = (matched ? 1 : 0) + donor_count + fallback_count;
  char** result = get_clear_memory(max_out * sizeof(char*));
  if (!result) return NULL;
  size_t result_count = 0;

  /* Default block parameters for the file being uploaded */
  int file_block_type = matched ? matched->block_type : (int)standard;
  size_t file_tuple_size = matched ? matched->tuple_size : (size_t)3;

  size_t blocks_needed = offs_ofd_total_blocks(file_size, file_block_type, file_tuple_size);
  size_t blocks_covered = 0;

  /* 1. Matched ORI */
  if (matched) {
    result[result_count] = strdup(matched->url);
    if (!result[result_count]) goto oom;
    result_count++;
    blocks_covered += offs_ofd_total_blocks(matched->final_byte, matched->block_type,
                                             matched->tuple_size);
  }

  /* 2. Greedy donors sorted by block count descending */
  if (blocks_covered < blocks_needed && donor_count > 0) {
    /* Create a sortable copy of donor pointers */
    offs_ofd_donor_t* sorted = get_clear_memory(donor_count * sizeof(offs_ofd_donor_t));
    if (!sorted) goto oom;
    memcpy(sorted, donors, donor_count * sizeof(offs_ofd_donor_t));
    qsort(sorted, donor_count, sizeof(offs_ofd_donor_t), _donor_block_cmp);

    for (size_t i = 0; i < donor_count && blocks_covered < blocks_needed; i++) {
      /* Skip self (the matched donor) */
      if (matched && _hash_eq(sorted[i].file_hash, sorted[i].hash_len,
                               matched->file_hash, matched->hash_len)) {
        continue;
      }
      result[result_count] = strdup(sorted[i].url);
      if (!result[result_count]) {
        free(sorted);
        goto oom;
      }
      result_count++;
      blocks_covered += offs_ofd_total_blocks(sorted[i].final_byte, sorted[i].block_type,
                                               sorted[i].tuple_size);
    }
    free(sorted);
  }

  /* 3. Fallback recyclers */
  for (size_t i = 0; i < fallback_count; i++) {
    result[result_count] = strdup(fallback_urls[i]);
    if (!result[result_count]) goto oom;
    result_count++;
  }

  *out_count = result_count;
  return result;

oom:
  for (size_t i = 0; i < result_count; i++) free(result[i]);
  free(result);
  return NULL;
}

void offs_ofd_free_donors(offs_ofd_donor_t* donors, size_t count) {
  if (!donors) return;
  for (size_t i = 0; i < count; i++) {
    free(donors[i].name);
    free(donors[i].url);
    free(donors[i].file_hash);
  }
  free(donors);
}
