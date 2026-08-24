//
// update_manifest.h — signed release manifest fetch + parse.
//
// Replaces the "sha256:" markdown scraping in update_check.c with a
// CBOR-encoded manifest signed with ed25519. The manifest carries the
// per-file SHA256 list that update_download verifies against.
//

#ifndef OFFS_UPDATE_MANIFEST_H
#define OFFS_UPDATE_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "update_check.h" /* for update_check_config_t */

typedef struct {
  char path[256];
  char sha256[65];
  uint64_t size;
} manifest_file_t;

typedef struct update_manifest_t {
  uint32_t version; /* must be 1 */
  char release_tag[64];
  manifest_file_t* files;
  size_t file_count;
} update_manifest_t;

/* Parse a CBOR manifest buffer into an update_manifest_t. The expected shape
 * is a definite array of 3: [version:uint, release_tag:tstr, files:array of
 * [path:tstr, sha256:tstr, size:uint]]. Returns NULL on any validation
 * failure (bad version, missing fields, malformed file entry, bad CBOR).
 * Caller frees with update_manifest_free. */
update_manifest_t* update_manifest_parse(const uint8_t* data, size_t len);

/* Fetch + verify the signed manifest for a release. Queries the GitHub
 * releases API for the release tagged `release_tag`, locates the
 * manifest.cbor + manifest.cbor.sig assets, downloads both, verifies the
 * ed25519 signature with the compiled-in release key via
 * update_verify_manifest, and parses the CBOR. Returns NULL on any failure
 * (network, TLS, bad sig, bad format). Caller frees with
 * update_manifest_free. */
update_manifest_t* update_manifest_fetch(const char* release_tag,
                                         const update_check_config_t* config);

void update_manifest_free(update_manifest_t* manifest);

#endif /* OFFS_UPDATE_MANIFEST_H */