#ifndef OFFS_PLATFORM_SOCKET_H
#define OFFS_PLATFORM_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PLATFORM_AF_INET,
  PLATFORM_AF_INET6,
  PLATFORM_AF_LOCAL
} platform_address_family_e;

typedef struct {
  platform_address_family_e family;
  union {
    struct { uint32_t addr; uint16_t port; } inet;
    struct { uint8_t addr[16]; uint16_t port; } inet6;
    struct { char path[108]; } local;
  };
} platform_address_t;

typedef struct platform_socket_t platform_socket_t;

#define PLATFORM_SHUT_RD    0
#define PLATFORM_SHUT_WR    1
#define PLATFORM_SHUT_RDWR  2

/* Lifecycle */
platform_socket_t* platform_socket_create(platform_address_family_e family, int stream);
void platform_socket_destroy(platform_socket_t* sock);
int platform_socket_fd(platform_socket_t* sock);

/* Server */
int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr);
int platform_socket_listen(platform_socket_t* sock, int backlog);
platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote);

/* Client */
int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr);

/* Configuration */
int platform_socket_set_nonblocking(platform_socket_t* sock);
int platform_socket_set_reuseaddr(platform_socket_t* sock);

/* I/O */
ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len);
ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len);
int platform_socket_shutdown(platform_socket_t* sock, int how);

/* Address helpers */
int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port);
int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_SOCKET_H */
