//
// Created by victor on 9/28/26.
//
// File copy / move / directory-emptiness primitives shared by the update
// stager and the cache-move command. Copy is a streamed 64 KiB loop; move
// is copy → verify → unlink so a failed copy never destroys the source
// (rename is same-volume only, and cache relocation is cross-volume).

#ifndef OFFS_FILE_COPY_H
#define OFFS_FILE_COPY_H

/* Verification mode for file_move_verified. */
#define FILE_VERIFY_NONE   0  /* delete the source as soon as the copy closes */
#define FILE_VERIFY_SHA256 1  /* hash both files and compare before unlinking */

/* Copy a single file in 64 KiB chunks. 0 on success, -1 on open/short
   write/read/close failure (a partial destination is removed on failure). */
int file_copy(const char* src, const char* dst);

/* Copy a directory tree recursively (regular files + subdirectories).
   Creates dst (and nested dirs) as needed. 0 on success, -1 on the first
   failure. */
int copy_tree(const char* src, const char* dst);

/* 1 = path exists and contains no entries besides "." / "..";
   0 = path exists and is not empty;
   -1 = path does not exist or cannot be opened. */
int dir_is_empty(const char* path);

/* Copy src → dst, verify, then unlink src.
   verify_mode: FILE_VERIFY_SHA256 (default) or FILE_VERIFY_NONE.
   0 on success. On copy/verify failure the destination partial is removed
   and the source is left intact; on unlink failure (after a verified copy)
   the destination remains and -1 is returned. */
int file_move_verified(const char* src, const char* dst, int verify_mode);

/* Streamed SHA256 of a file, lowercase hex, NUL-terminated.
   0 on success, -1 on open/read/digest failure. */
int file_sha256_hex(const char* path, char out_hex[65]);

#endif // OFFS_FILE_COPY_H