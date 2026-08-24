#ifndef OFFS_UPDATE_EXTRACT_H
#define OFFS_UPDATE_EXTRACT_H

#include <stdint.h>

/* Extract a USTAR tar archive into dest_dir with path containment. Regular
   files and directories only. Rejects absolute paths, ".." components,
   symlinks, hardlinks, devices, FIFOs, unknown entry types, and archives
   whose total extracted size exceeds max_bytes. Returns 0 on success, -1 on
   any violation or I/O error. dest_dir must exist. The archive's overall
   integrity is verified by the caller via SHA256 against the signed manifest
   before this function is called, so the extractor does not validate USTAR
   header checksums (the manifest hash is a stronger guarantee). */
int update_extract(const char* archive_path, const char* dest_dir, uint64_t max_bytes);

#endif /* OFFS_UPDATE_EXTRACT_H */
