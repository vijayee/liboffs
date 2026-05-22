#define _DEFAULT_SOURCE

#include "platform_socket.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <string.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <errno.h>

  struct platform_socket_t {
    int fd;
    int family; /* platform_address_family_e */
  };

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return AF_LOCAL;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    int native_family = _family_to_native(family);
    if (native_family < 0) {
      return NULL;
    }
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    int fd = socket(native_family, type, 0);
    if (fd < 0) {
      return NULL;
    }
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      close(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->family = family;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) {
      return;
    }
    close(sock->fd);
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    return sock != NULL ? sock->fd : -1;
  }

  static int _address_to_native(const platform_address_t* addr,
                                 struct sockaddr_storage* storage, socklen_t* addrlen) {
    memset(storage, 0, sizeof(*storage));
    switch (addr->family) {
      case PLATFORM_AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->inet.port);
        sin->sin_addr.s_addr = addr->inet.addr;
        *addrlen = sizeof(*sin);
        return 0;
      }
      case PLATFORM_AF_INET6: {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->inet6.port);
        memcpy(&sin6->sin6_addr, addr->inet6.addr, 16);
        *addrlen = sizeof(*sin6);
        return 0;
      }
      case PLATFORM_AF_LOCAL: {
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_LOCAL;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        sun->sun_path[sizeof(sun->sun_path) - 1] = '\0';
        *addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(sun->sun_path) + 1);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog);
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    int client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd < 0) {
      return NULL;
    }
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      close(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) {
      return -1;
    }
    return fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
  #ifdef __APPLE__
    return send(sock->fd, buf, len, 0);
  #else
    return send(sock->fd, buf, len, MSG_NOSIGNAL);
  #endif
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    return recv(sock->fd, buf, len, 0);
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SHUT_RD; break;
      case PLATFORM_SHUT_WR:   native_how = SHUT_WR; break;
      case PLATFORM_SHUT_RDWR: native_how = SHUT_RDWR; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how);
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    struct in6_addr in6;
    if (inet_pton(AF_INET6, host, &in6) == 1) {
      addr->family = PLATFORM_AF_INET6;
      memcpy(addr->inet6.addr, &in6, 16);
      addr->inet6.port = port;
      return 0;
    }
    return -1;
  }

  int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len) {
    switch (addr->family) {
      case PLATFORM_AF_INET:
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_INET6:
        if (inet_ntop(AF_INET6, addr->inet6.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        if (len > 0) {
          buf[len - 1] = '\0';
        }
        return 0;
    }
    return -1;
  }
#else
  /* Windows implementation — Winsock2 */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <stdlib.h>
  #include <string.h>

  #include "platform_internal.h"

  struct platform_socket_t {
    SOCKET fd;
    int family; /* platform_address_family_e */
  };

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return _platform_has_af_unix() ? AF_UNIX : -1;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    if (_platform_winsock_init() != 0) {
      return NULL;
    }
    int native_family = _family_to_native(family);
    if (native_family < 0) {
      return NULL;
    }
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    SOCKET fd = socket(native_family, type, 0);
    if (fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      closesocket(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->family = family;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) {
      return;
    }
    closesocket(sock->fd);
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    if (sock == NULL) return -1;
    return (int)(intptr_t)sock->fd;
  }

  static int _address_to_native(const platform_address_t* addr,
                                 struct sockaddr_storage* storage, socklen_t* addrlen) {
    memset(storage, 0, sizeof(*storage));
    switch (addr->family) {
      case PLATFORM_AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->inet.port);
        sin->sin_addr.s_addr = addr->inet.addr;
        *addrlen = sizeof(*sin);
        return 0;
      }
      case PLATFORM_AF_INET6: {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->inet6.port);
        memcpy(&sin6->sin6_addr, addr->inet6.addr, 16);
        *addrlen = sizeof(*sin6);
        return 0;
      }
      case PLATFORM_AF_LOCAL: {
        if (!_platform_has_af_unix()) {
          return -1;
        }
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_UNIX;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        sun->sun_path[sizeof(sun->sun_path) - 1] = '\0';
        *addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(sun->sun_path) + 1);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog) == 0 ? 0 : -1;
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    SOCKET client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      closesocket(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    unsigned long mode = 1;
    return ioctlsocket(sock->fd, FIONBIO, &mode) == 0 ? 0 : -1;
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char*)&opt, sizeof(opt)) == 0 ? 0 : -1;
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
    int result = send(sock->fd, (const char*)buf, (int)len, 0);
    return (ssize_t)result;
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    int result = recv(sock->fd, (char*)buf, (int)len, 0);
    return (ssize_t)result;
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SD_RECEIVE; break;
      case PLATFORM_SHUT_WR:   native_how = SD_SEND; break;
      case PLATFORM_SHUT_RDWR: native_how = SD_BOTH; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how) == 0 ? 0 : -1;
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    struct in6_addr in6;
    if (inet_pton(AF_INET6, host, &in6) == 1) {
      addr->family = PLATFORM_AF_INET6;
      memcpy(addr->inet6.addr, &in6, 16);
      addr->inet6.port = port;
      return 0;
    }
    return -1;
  }

  int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len) {
    switch (addr->family) {
      case PLATFORM_AF_INET:
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_INET6:
        if (inet_ntop(AF_INET6, addr->inet6.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        if (len > 0) {
          buf[len - 1] = '\0';
        }
        return 0;
    }
    return -1;
  }
#endif
