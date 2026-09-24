//
// Created by victor on 9/24/26.
//

#include "endpoint.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int endpoint_parse(const char* input, char* host_out, size_t host_len,
                   uint16_t* port_out) {
  if (input == NULL || host_out == NULL || port_out == NULL) return -1;

  while (isspace((unsigned char)*input)) input++;
  const char* input_end = input + strlen(input);
  while (input_end > input && isspace((unsigned char)input_end[-1])) input_end--;
  if (input_end == input) return -1;

  const char* host_start;
  size_t host_length;
  const char* port_start;

  if (*input == '[') {
    const char* closing = memchr(input, ']', (size_t)(input_end - input));
    if (closing == NULL) return -1;
    host_start = input + 1;
    host_length = (size_t)(closing - host_start);
    if (host_length == 0) return -1;
    if (closing + 1 == input_end || closing[1] != ':') return -1;
    port_start = closing + 2;
  } else {
    /* Hostname/IPv4 cannot contain ':', so the last separator is the port.
     * A host portion still holding ':' is an unbracketed IPv6 literal, which
     * is ambiguous with a host:port split and stays unsupported. */
    const char* colon = NULL;
    for (const char* scan = input_end - 1; scan >= input; scan--) {
      if (*scan == ':') {
        colon = scan;
        break;
      }
    }
    if (colon == NULL) return -1;
    host_start = input;
    host_length = (size_t)(colon - input);
    if (host_length == 0) return -1;
    if (memchr(host_start, ':', host_length) != NULL) return -1;
    port_start = colon + 1;
  }

  if (port_start == input_end) return -1;
  for (const char* digit = port_start; digit != input_end; digit++) {
    if (!isdigit((unsigned char)*digit)) return -1;
  }
  char* end = NULL;
  long port = strtol(port_start, &end, 10);
  if (end != input_end || port < 1 || port > 65535) return -1;

  if (host_length + 1 > host_len) return -1;
  memcpy(host_out, host_start, host_length);
  host_out[host_length] = '\0';
  *port_out = (uint16_t)port;
  return 0;
}