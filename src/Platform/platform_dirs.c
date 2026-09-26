#include "platform_dirs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#else
#include <unistd.h>
#endif

/* Copies src into dst, appending suffix. Returns -1 when the result would
 * not fit or src is empty, so partial results are never reported as OK. */
static int dirs_append(char* dst, size_t dst_size, const char* src,
                       const char* suffix) {
  if (src == NULL || src[0] == '\0') return -1;
  int written = snprintf(dst, dst_size, "%s%s", src, suffix);
  if (written < 0 || (size_t)written >= dst_size) return -1;
  return 0;
}

#if defined(_WIN32)

static const char* env_or_empty(const char* name) {
  const char* value = getenv(name);
  return value != NULL ? value : "";
}

/* Services run in session "Services" (SESSIONNAME=Services); interactive
 * logons have RDP-Tcp#n / Console. This is the standard service-context
 * heuristic on Windows. */
static int offs_dirs_is_service_context(void) {
  const char* session = getenv("SESSIONNAME");
  return session != NULL &&
         _stricmp(session, "Services") == 0;
}

int offs_default_dirs_get(offs_default_dirs_t* out) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  char program_data[MAX_PATH];
  char local_app_data[MAX_PATH];
  if (offs_dirs_is_service_context() &&
      GetEnvironmentVariableA("ProgramData", program_data, MAX_PATH) > 0) {
    if (dirs_append(out->config_dir, sizeof(out->config_dir), program_data,
                    "\\offs") != 0 ||
        dirs_append(out->cache_dir, sizeof(out->cache_dir), program_data,
                    "\\offs\\cache") != 0) {
      return -1;
    }
    return 0;
  }
  UINT length = GetEnvironmentVariableA("LOCALAPPDATA", local_app_data,
                                        MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return -1;
  if (dirs_append(out->config_dir, sizeof(out->config_dir), local_app_data,
                  "\\offs") != 0 ||
      dirs_append(out->cache_dir, sizeof(out->cache_dir), local_app_data,
                  "\\offs\\cache") != 0) {
    return -1;
  }
  return 0;
}

#else

static void dirs_env_or(char* dst, size_t dst_size, const char* env_name,
                        const char* home_relative) {
  const char* value = getenv(env_name);
  if (value != NULL && value[0] != '\0' && value[0] == '/') {
    snprintf(dst, dst_size, "%s", value);
  } else {
    const char* home = getenv("HOME");
    snprintf(dst, dst_size, "%s/%s", home != NULL ? home : "",
             home_relative);
  }
}

int offs_default_dirs_get(offs_default_dirs_t* out) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  if (geteuid() == 0) {
#if defined(__APPLE__)
    if (dirs_append(out->config_dir, sizeof(out->config_dir),
                    "/Library/Application Support", "/offs") != 0 ||
        dirs_append(out->cache_dir, sizeof(out->cache_dir), "/Library/Caches",
                    "/offs") != 0) {
      return -1;
    }
#else
    if (dirs_append(out->config_dir, sizeof(out->config_dir), "/etc",
                    "/offs") != 0 ||
        dirs_append(out->cache_dir, sizeof(out->cache_dir), "/var/lib",
                    "/offs") != 0) {
      return -1;
    }
#endif
    return 0;
  }
  const char* home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return -1;
#if defined(__APPLE__)
  if (dirs_append(out->config_dir, sizeof(out->config_dir), home,
                  "/Library/Application Support/offs") != 0 ||
      dirs_append(out->cache_dir, sizeof(out->cache_dir), home,
                  "/Library/Caches/offs") != 0) {
    return -1;
  }
#else
  dirs_env_or(out->config_dir, sizeof(out->config_dir), "XDG_CONFIG_HOME",
              ".config/offs");
  dirs_env_or(out->cache_dir, sizeof(out->cache_dir), "XDG_CACHE_HOME",
              ".cache/offs");
#endif
  return 0;
}

#endif