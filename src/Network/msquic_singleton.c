//
// Created by victor on 5/16/26.
//

#include "msquic_singleton.h"

#ifdef HAS_MSQUIC

#include "../Util/log.h"
#include "../Platform/platform.h"
#include <stdint.h>
#include <stdbool.h>

static const struct QUIC_API_TABLE* g_msquic = NULL;
static uint32_t g_msquic_refcount = 0;
static platform_mutex_t* g_msquic_lock = NULL;

static void _ensure_msquic_lock_initialized(void) {
  if (g_msquic_lock == NULL) {
    g_msquic_lock = platform_mutex_create();
  }
}

const struct QUIC_API_TABLE* offs_msquic_open(void) {
  _ensure_msquic_lock_initialized();
  platform_mutex_lock(g_msquic_lock);
  if (g_msquic == NULL) {
    const struct QUIC_API_TABLE* table = NULL;
    QUIC_STATUS status;
    if (QUIC_FAILED(status = MsQuicOpen2(&table))) {
      log_error("MsQuicOpen2 failed: 0x%x", status);
      platform_mutex_unlock(g_msquic_lock);
      return NULL;
    }
    g_msquic = table;
    g_msquic_refcount = 1;
    platform_mutex_unlock(g_msquic_lock);
    return g_msquic;
  }
  g_msquic_refcount++;
  const struct QUIC_API_TABLE* result = g_msquic;
  platform_mutex_unlock(g_msquic_lock);
  return result;
}

void offs_msquic_close(void) {
  _ensure_msquic_lock_initialized();
  platform_mutex_lock(g_msquic_lock);
  if (g_msquic == NULL || g_msquic_refcount == 0) {
    platform_mutex_unlock(g_msquic_lock);
    return;
  }
  g_msquic_refcount--;
  /* MsQuic is a PROCESS-LIFETIME singleton: once opened, it is kept open for
     the life of the process and reused across network create/destroy cycles
     (including offsd config-reload restarts, which destroy and re-create the
     network every cycle). Do NOT call MsQuicClose when the refcount reaches 0.

     MsQuicOpen2/MsQuicClose are not idempotent-balanced in a tight loop:
     MsQuicClose returns before its internal worker/timer threads fully exit,
     so a close-then-immediately-reopen cycle (refcount 1->0->1) leaks those
     thread handles -- observed as +1..+3 OS handles per offs_node_restart.
     Keeping MsQuic open across cycles eliminates the reopen and the leak.
     The single MsQuic table + this lock are reclaimed by the OS at process
     exit; a long-running offsd pays the one-time open cost exactly once. */
  platform_mutex_unlock(g_msquic_lock);
}

#else /* !HAS_MSQUIC */

const struct QUIC_API_TABLE* offs_msquic_open(void) {
  return NULL;
}

void offs_msquic_close(void) {
}

#endif /* HAS_MSQUIC */