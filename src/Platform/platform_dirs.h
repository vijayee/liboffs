#ifndef OFFS_PLATFORM_DIRS_H
#define OFFS_PLATFORM_DIRS_H

#include <stddef.h>

/* Context-appropriate default directories for the node's config-dir
 * (identity: certs, peer store, pending config) and cache-dir (block
 * cache). Resolution order everywhere: explicit flag -> system context
 * -> per-user default. These functions only fill the DEFAULTS; callers
 * pass explicit paths when the operator configured one.
 *
 * System context (per platform):
 *   Linux   config=/etc/offs                cache=/var/lib/offs
 *   macOS   config=/Library/Application Support/offs
 *           cache=/Library/Caches/offs
 *   Windows config=%ProgramData%\offs       cache=%ProgramData%\offs\cache
 *           (always — the node's identity is machine state, service or
 *            not; there is no per-user daemon context)
 * Per-user context (POSIX only):
 *   Linux   config=$XDG_CONFIG_HOME|$HOME/.config /offs
 *           cache=$XDG_CACHE_HOME|$HOME/.cache /offs
 *   macOS   config=$HOME/Library/Application Support/offs
 *           cache=$HOME/Library/Caches/offs */

typedef struct {
  char config_dir[1024];
  char cache_dir[1024];
} offs_default_dirs_t;

/* Returns 0 on success, -1 when the context cannot be resolved (e.g. no
 * HOME/USERPROFILE). Buffers are always NUL-terminated. */
int offs_default_dirs_get(offs_default_dirs_t* out);

/* The daemon's default config file: $OFFS_CONFIG when set (and
 * non-empty), else the platform machine-wide default —
 * %ProgramData%\offs\offs.json on Windows, /etc/offs/offs.json
 * elsewhere — the same machine-wide root offs_default_dirs_get() puts
 * the default state dirs under. Returns 0 with out NUL-terminated, -1
 * when neither source resolves or the result would not fit. Whether
 * the file exists (and is regular) is the caller's stat gate. */
int offs_default_config_path_get(char* out, size_t out_size);

#endif /* OFFS_PLATFORM_DIRS_H */