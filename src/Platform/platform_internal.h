#ifndef OFFS_PLATFORM_INTERNAL_H
#define OFFS_PLATFORM_INTERNAL_H

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>

  /* One-shot WSAStartup */
  static inline int _platform_winsock_init(void) {
    static int initialized = 0;
    if (!initialized) {
      WSADATA wsa_data;
      if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return -1;
      initialized = 1;
    }
    return 0;
  }

  /* Windows AF_UNIX detection */
  static inline int _platform_has_af_unix(void) {
    static int checked = 0;
    static int available = 0;
    if (!checked) {
      SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
      if (s != INVALID_SOCKET) {
        available = 1;
        closesocket(s);
      }
      checked = 1;
    }
    return available;
  }
#endif

#endif /* OFFS_PLATFORM_INTERNAL_H */
