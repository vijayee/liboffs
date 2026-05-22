#include "platform_local.h"
#include "platform_file.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <string.h>
  #include <unistd.h>

  platform_socket_t* platform_local_listen(const char* path) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    platform_file_unlink(path); /* remove stale socket file */

    if (platform_socket_bind(sock, &addr) != 0 ||
        platform_socket_listen(sock, 128) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  platform_socket_t* platform_local_accept(platform_socket_t* listener) {
    return platform_socket_accept(listener, NULL);
  }

  platform_socket_t* platform_local_connect(const char* path) {
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

    if (platform_socket_connect(sock, &addr) != 0) {
      platform_socket_destroy(sock);
      return NULL;
    }
    return sock;
  }

  void platform_local_cleanup(const char* path) {
    platform_file_unlink(path);
  }
#else
  /* Windows implementation deferred */
#endif
