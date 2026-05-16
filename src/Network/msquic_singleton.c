//
// Created by victor on 5/16/26.
//

#include "msquic_singleton.h"
#include <stddef.h>

#ifdef HAS_MSQUIC

#include "../Util/log.h"
#include "../Util/threadding.h"

static const struct QUIC_API_TABLE* g_msquic = NULL;
static uint32_t g_msquic_refcount = 0;
static PLATFORMLOCKTYPE(g_msquic_lock);
static bool g_msquic_lock_initialized = false;

static void _ensure_msquic_lock_initialized(void) {
  if (!g_msquic_lock_initialized) {
    platform_lock_init(&g_msquic_lock);
    g_msquic_lock_initialized = true;
  }
}

const struct QUIC_API_TABLE* offs_msquic_open(void) {
  _ensure_msquic_lock_initialized();
  platform_lock(&g_msquic_lock);
  if (g_msquic == NULL) {
    const struct QUIC_API_TABLE* table = NULL;
    QUIC_STATUS status;
    if (QUIC_FAILED(status = MsQuicOpen2(&table))) {
      log_error("MsQuicOpen2 failed: 0x%x", status);
      platform_unlock(&g_msquic_lock);
      return NULL;
    }
    g_msquic = table;
    g_msquic_refcount = 1;
    platform_unlock(&g_msquic_lock);
    return g_msquic;
  }
  g_msquic_refcount++;
  const struct QUIC_API_TABLE* result = g_msquic;
  platform_unlock(&g_msquic_lock);
  return result;
}

void offs_msquic_close(void) {
  _ensure_msquic_lock_initialized();
  platform_lock(&g_msquic_lock);
  if (g_msquic == NULL || g_msquic_refcount == 0) {
    platform_unlock(&g_msquic_lock);
    return;
  }
  g_msquic_refcount--;
  if (g_msquic_refcount == 0) {
    MsQuicClose(g_msquic);
    g_msquic = NULL;
  }
  platform_unlock(&g_msquic_lock);
}

#else /* !HAS_MSQUIC */

const struct QUIC_API_TABLE* offs_msquic_open(void) {
  return NULL;
}

void offs_msquic_close(void) {
}

#endif /* HAS_MSQUIC */