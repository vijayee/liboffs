//
// Created by victor on 5/26/26.
//

#ifndef OFFS_OFD_RESOLVER_H
#define OFFS_OFD_RESOLVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A parsed OFD file entry used for donor matching */
typedef struct {
  char* name;
  char* url;            /* pre-built offs URL for this ORI */
  size_t final_byte;
  int block_type;       /* block_size_e value */
  size_t tuple_size;
  uint8_t* file_hash;   /* raw hash bytes for dedup */
  size_t hash_len;
} offs_ofd_donor_t;

/* Fetch and parse an OFD from a URL, recursively resolving sub-OFDs.
   Returns an array of donor entries (all file ORIs from the OFD tree).
   out_count receives the array length. Returns NULL on error.
   Caller frees with offs_ofd_free_donors(). */
offs_ofd_donor_t* offs_ofd_resolve(const char* ofd_url, size_t* out_count);

/* Build a per-file recycler URL list using the greedy donor algorithm.
   matched: the matching OFD entry for this file, or NULL if no match.
   file_size: size of the local file being uploaded.
   fallback_urls/fallback_count: original non-OFD recycler URLs.
   out_count: receives the number of URLs in the returned array.
   Returns a malloc'd array of malloc'd URL strings. Caller frees each string
   then the array. */
char** offs_ofd_build_recyclers(
    const offs_ofd_donor_t* donors, size_t donor_count,
    const offs_ofd_donor_t* matched,
    size_t file_size,
    const char** fallback_urls, size_t fallback_count,
    size_t* out_count);

/* Free an array of donor entries returned by offs_ofd_resolve(). */
void offs_ofd_free_donors(offs_ofd_donor_t* donors, size_t count);

/* Calculate total blocks (data + descriptor) for a file/ORI.
   Used internally and exposed for testing. */
size_t offs_ofd_total_blocks(size_t final_byte, int block_type, size_t tuple_size);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_OFD_RESOLVER_H */
