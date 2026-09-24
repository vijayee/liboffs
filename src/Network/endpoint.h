//
// Created by victor on 9/24/26.
//

#ifndef OFFS_ENDPOINT_H
#define OFFS_ENDPOINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a "host:port" or "[ipv6-literal]:port" endpoint string.
 *
 * Accepts IPv4 or hostname with any port separator, and IPv6 literals only
 * in brackets (bare unbracketed IPv6 is ambiguous and stays unsupported
 * until the IPv6 cycle). Port must be 1..65535.
 *
 * The parser is structural only: it does not validate host characters beyond
 * structure. Hostnames are validated at resolution/connect time.
 *
 * Returns 0 on success and fills host_out (NUL-terminated) + port_out.
 * Returns -1 on malformed input. */
int endpoint_parse(const char* input, char* host_out, size_t host_len,
                   uint16_t* port_out);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_ENDPOINT_H */
