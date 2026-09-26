#ifndef OFFS_PLATFORM_DIRS_H
#define OFFS_PLATFORM_DIRS_H

#include <stddef.h>

/* Context-appropriate default directories for the node's config-dir
 * (identity: certs, peer store, pending config) and cache-dir (block
 * cache). Resolution order everywhere: explicit flag -> service context
 * -> per-user default. These functions only fill the DEFAULTS; callers
 * pass explicit paths when the operator configured one.
 *
 * Service/root context (per platform):
 *   Linux   config=/etc/offs                cache=/var/lib/offs
 *   macOS   config=/Library/Application Support/offs
 *           cache=/Library/Caches/offs
 *   Windows config=%ProgramData%\offs       cache=%ProgramData%\offs\cache
 *           (service detected via the SESSIONNAME=Services heuristic)
 * Per-user context:
 *   Linux   config=$XDG_CONFIG_HOME|$HOME/.config /offs
 *           cache=$XDG_CACHE_HOME|$HOME/.cache /offs
 *   macOS   config=$HOME/Library/Application Support/offs
 *           cache=$HOME/Library/Caches/offs
 *   Windows config=%LOCALAPPDATA%\offs      cache=%LOCALAPPDATA%\offs\cache
 *           (not %APPDATA% — node identity is machine-local, it must not
 *            roam between machines) */

typedef struct {
  char config_dir[1024];
  char cache_dir[1024];
} offs_default_dirs_t;

/* Returns 0 on success, -1 when the context cannot be resolved (e.g. no
 * HOME/USERPROFILE). Buffers are always NUL-terminated. */
int offs_default_dirs_get(offs_default_dirs_t* out);

#endif /* OFFS_PLATFORM_DIRS_H */