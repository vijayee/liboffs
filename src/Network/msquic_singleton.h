//
// Created by victor on 5/16/26.
//

#ifndef MSQUIC_SINGLETON_H
#define MSQUIC_SINGLETON_H

#ifdef HAS_MSQUIC
#include <msquic.h>
#endif

/**
 * Gets the process-wide MsQuic API table, opening it if necessary.
 * Thread-safe. Reference-counted: each call that returns a non-NULL
 * pointer must be paired with a later offs_msquic_close().
 *
 * @return  QUIC_API_TABLE pointer, or NULL on failure (or when
 *          HAS_MSQUIC is not defined)
 */
const struct QUIC_API_TABLE* offs_msquic_open(void);

/**
 * Releases a reference to the process-wide MsQuic API table.
 * When the reference count drops to zero, MsQuicClose is called.
 * Must be paired with a prior offs_msquic_open() that succeeded.
 */
void offs_msquic_close(void);

#endif // MSQUIC_SINGLETON_H