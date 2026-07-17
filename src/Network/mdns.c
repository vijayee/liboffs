//
// Created by victor on 7/16/26.
//

#include "mdns.h"
#include "network.h"
#include "peer_info.h"
#include "quic_listener.h"
#include "authority.h"
#include "node_id.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/base58.h"
#include "../Platform/platform_thread.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifndef _WIN32

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <ifaddrs.h>

/* mDNS multicast group + port (RFC 6762). */
#define MDNS_MULTICAST_ADDR  "224.0.0.251"
#define MDNS_MULTICAST_PORT   5353
#define MDNS_BROADCAST_INTERVAL_S 5
#define MDNS_RECV_TIMEOUT_S   1
#define MDNS_TTL_S            120

/* DNS record types we use. */
#define DNS_TYPE_A     1
#define DNS_TYPE_SRV   33
#define DNS_CLASS_IN   1
#define DNS_CACHE_FLUSH 0x8000  /* high bit of CLASS — mDNS cache-flush bit */

/* Service suffix appended to the node_id label. */
#define MDNS_SERVICE_SUFFIX ".offs._tcp.local"

/* Max DNS packet size we accept. RFC 6762 says mDNS packets must be ≤ 9000
   bytes; we cap at 1452 (typical Ethernet MTU minus IP+UDP headers). */
#define MDNS_MAX_PACKET 1452

/* Build a DNS name encoding: <label_len><label_bytes> ... 0x00. Writes to
   out_buf starting at *offset, advances *offset. Returns 0 on success,
   -1 if the name doesn't fit in out_len bytes. */
static int _dns_encode_name(const char* name, uint8_t* out_buf,
                            size_t out_len, size_t* offset) {
  if (name == NULL || out_buf == NULL || offset == NULL) return -1;
  size_t name_len = strlen(name);
  if (name_len == 0) return -1;
  /* Each label is preceded by a length byte. The encoding needs
     name_len + (number of dots + 1) bytes + the terminating 0x00. */
  size_t needed = name_len + 2;  /* lower bound */
  if (*offset + needed > out_len) return -1;

  const char* cursor = name;
  while (*cursor != '\0') {
    const char* dot = strchr(cursor, '.');
    size_t label_len = (dot != NULL) ? (size_t)(dot - cursor) : strlen(cursor);
    if (label_len == 0 || label_len > 63) return -1;
    if (*offset + 1 + label_len > out_len) return -1;
    out_buf[*offset] = (uint8_t)label_len;
    (*offset)++;
    memcpy(out_buf + *offset, cursor, label_len);
    (*offset) += label_len;
    if (dot == NULL) break;
    cursor = dot + 1;
  }
  if (*offset + 1 > out_len) return -1;
  out_buf[*offset] = 0x00;
  (*offset)++;
  return 0;
}

/* Parse a DNS name (with compression pointers) starting at offset. Writes
   the decoded dotted name (without trailing dot) into out_buf of size
   out_len. Advances *offset to the position right after the name. Returns
   0 on success, -1 on malformed/truncated input. */
static int _dns_decode_name(const uint8_t* pkt, size_t pkt_len,
                            size_t start_offset, size_t* end_offset,
                            char* out_buf, size_t out_len) {
  if (pkt == NULL || end_offset == NULL || out_buf == NULL || out_len == 0) {
    return -1;
  }
  if (start_offset >= pkt_len) return -1;

  size_t offset = start_offset;
  size_t out_pos = 0;
  int jumped = 0;
  size_t after_offset = 0;
  int hops = 0;  /* prevent compression loops */

  while (hops < 128) {
    if (offset >= pkt_len) return -1;
    uint8_t label_len = pkt[offset];
    if (label_len == 0) {
      offset++;
      if (!jumped) after_offset = offset;
      *end_offset = after_offset;
      if (out_pos == 0) {
        if (out_len < 1) return -1;
        out_buf[0] = '\0';
      } else {
        /* strip the trailing dot — out_pos points one past the last '.' */
        if (out_pos > 0 && out_buf[out_pos - 1] == '.') out_pos--;
        if (out_pos >= out_len) return -1;
        out_buf[out_pos] = '\0';
      }
      return 0;
    }

    /* Compression pointer: top two bits = 0b11. */
    if ((label_len & 0xC0) == 0xC0) {
      if (offset + 1 >= pkt_len) return -1;
      uint16_t ptr = ((uint16_t)(label_len & 0x3F) << 8) | pkt[offset + 1];
      if (!jumped) after_offset = offset + 2;
      offset = ptr;
      jumped = 1;
      hops++;
      continue;
    }

    /* Regular label. */
    if (label_len > 63) return -1;
    offset++;
    if (offset + label_len > pkt_len) return -1;
    if (out_pos + label_len + 1 >= out_len) return -1;
    memcpy(out_buf + out_pos, pkt + offset, label_len);
    out_pos += label_len;
    out_buf[out_pos] = '.';
    out_pos++;
    offset += label_len;
    if (!jumped) after_offset = offset;
  }
  return -1;  /* too many hops — likely a compression loop */
}

/* Read a 16-bit big-endian value from a buffer. */
static uint16_t _read_u16_be(const uint8_t* buf) {
  return (uint16_t)((buf[0] << 8) | buf[1]);
}

/* Write a 16-bit big-endian value to a buffer. */
static void _write_u16_be(uint8_t* buf, uint16_t value) {
  buf[0] = (uint8_t)((value >> 8) & 0xFF);
  buf[1] = (uint8_t)(value & 0xFF);
}

/* Write a 32-bit big-endian value to a buffer. */
static void _write_u32_be(uint8_t* buf, uint32_t value) {
  buf[0] = (uint8_t)((value >> 24) & 0xFF);
  buf[1] = (uint8_t)((value >> 16) & 0xFF);
  buf[2] = (uint8_t)((value >> 8) & 0xFF);
  buf[3] = (uint8_t)(value & 0xFF);
}

/* Find the first non-loopback IPv4 address on the local interfaces.
   Returns it in host byte order. Returns 0 if none found (caller should
   skip the mDNS broadcast — we have no useful LAN IP to announce). */
static uint32_t _find_lan_ipv4(void) {
  struct ifaddrs* ifaces = NULL;
  if (getifaddrs(&ifaces) != 0) return 0;
  uint32_t result = 0;
  for (struct ifaddrs* ifa = ifaces; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) continue;
    if (ifa->ifa_addr->sa_family != AF_INET) continue;
    struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
    uint32_t addr = ntohl(sa->sin_addr.s_addr);
    /* Skip loopback (127.0.0.0/8) and link-local (169.254.0.0/16) — the
       latter is auto-configured and not routable on the LAN. */
    if ((addr >> 24) == 127) continue;
    if ((addr >> 16) == 0xA9FE) continue;
    result = addr;
    break;
  }
  freeifaddrs(ifaces);
  return result;
}

/* Build a DNS response packet announcing our node. Layout:
   header(12) + A_record(name + type + class + ttl + rdlength + rdata)
                + SRV_record(name + type + class + ttl + rdlength + rdata)
   The name is <node_id_base58>.offs._tcp.local. Returns packet size, -1 on
   error. */
static int _mdns_build_announce(const char* node_id_b58, uint32_t lan_ip,
                                 uint16_t quic_port, uint8_t* out_buf,
                                 size_t out_len) {
  if (out_buf == NULL || out_len < 64) return -1;
  memset(out_buf, 0, out_len);

  /* Build the full name once into a stack buffer; reuse it for both
     records. */
  char name[256];
  int written = snprintf(name, sizeof(name), "%s%s",
                         node_id_b58, MDNS_SERVICE_SUFFIX);
  if (written <= 0 || (size_t)written >= sizeof(name)) return -1;

  size_t offset = 0;

  /* Header (12 bytes). ID=0 (mDNS). Flags: QR=1 (response), AA=1.
     QDCOUNT=0, ANCOUNT=2, NSCOUNT=0, ARCOUNT=0. */
  if (out_len < 12) return -1;
  _write_u16_be(out_buf + 0, 0);          /* ID */
  _write_u16_be(out_buf + 2, 0x8400);     /* flags: QR=1, AA=1 */
  _write_u16_be(out_buf + 4, 0);          /* QDCOUNT */
  _write_u16_be(out_buf + 6, 2);          /* ANCOUNT — A + SRV */
  _write_u16_be(out_buf + 8, 0);          /* NSCOUNT */
  _write_u16_be(out_buf + 10, 0);         /* ARCOUNT */
  offset = 12;

  /* A record: name + TYPE(1) + CLASS(IN|cache-flush) + TTL(120) + RDLENGTH(4) + RDATA(4) */
  if (_dns_encode_name(name, out_buf, out_len, &offset) != 0) return -1;
  if (offset + 10 > out_len) return -1;
  _write_u16_be(out_buf + offset, DNS_TYPE_A);
  offset += 2;
  _write_u16_be(out_buf + offset, DNS_CLASS_IN | DNS_CACHE_FLUSH);
  offset += 2;
  _write_u32_be(out_buf + offset, MDNS_TTL_S);
  offset += 4;
  _write_u16_be(out_buf + offset, 4);  /* RDLENGTH */
  offset += 2;
  if (offset + 4 > out_len) return -1;
  /* RDATA: 4-byte IPv4 in network byte order. */
  _write_u32_be(out_buf + offset, lan_ip);
  offset += 4;

  /* SRV record: name + TYPE(33) + CLASS(IN|cache-flush) + TTL + RDLENGTH + RDATA.
     RDATA: priority(2) + weight(2) + port(2) + target(name). */
  if (_dns_encode_name(name, out_buf, out_len, &offset) != 0) return -1;
  if (offset + 10 > out_len) return -1;
  _write_u16_be(out_buf + offset, DNS_TYPE_SRV);
  offset += 2;
  _write_u16_be(out_buf + offset, DNS_CLASS_IN | DNS_CACHE_FLUSH);
  offset += 2;
  _write_u32_be(out_buf + offset, MDNS_TTL_S);
  offset += 4;
  /* RDLENGTH placeholder — fill in after we know the RDATA size. */
  size_t rdlength_offset = offset;
  offset += 2;
  /* RDATA: priority(2) + weight(2) + port(2) + target(name). */
  if (offset + 6 > out_len) return -1;
  _write_u16_be(out_buf + offset, 0);  /* priority */
  offset += 2;
  _write_u16_be(out_buf + offset, 0);  /* weight */
  offset += 2;
  _write_u16_be(out_buf + offset, quic_port);
  offset += 2;
  size_t target_start = offset;
  if (_dns_encode_name(name, out_buf, out_len, &offset) != 0) return -1;
  size_t rdata_len = offset - target_start + 6;  /* +6 for priority+weight+port */
  _write_u16_be(out_buf + rdlength_offset, (uint16_t)rdata_len);

  return (int)offset;
}

/* Parse an incoming DNS response packet. Looks for A and SRV records in the
   answer section matching <node_id>.offs._tcp.local. On success, fills out
   node_id_b58 (NODE_ID_STRING_SIZE bytes), lan_ip (host byte order), and
   quic_port. Returns 0 on success, -1 on no match / malformed input. */
static int _mdns_parse_response(const uint8_t* pkt, size_t pkt_len,
                                 char* node_id_b58, size_t node_id_b58_len,
                                 uint32_t* lan_ip, uint16_t* quic_port) {
  if (pkt == NULL || pkt_len < 12 || node_id_b58 == NULL ||
      lan_ip == NULL || quic_port == NULL) {
    return -1;
  }
  /* Only parse responses (QR=1) — queries are ignored (we don't respond to
     them; we just broadcast periodically). */
  uint16_t flags = _read_u16_be(pkt + 2);
  if ((flags & 0x8000) == 0) return -1;

  uint16_t ancount = _read_u16_be(pkt + 6);
  if (ancount == 0 || ancount > 32) return -1;  /* sanity cap */

  size_t offset = 12;
  char found_name[256];
  found_name[0] = '\0';
  uint32_t found_ip = 0;
  uint16_t found_port = 0;
  int have_a = 0;
  int have_srv = 0;

  for (uint16_t i = 0; i < ancount; i++) {
    char name[256];
    size_t after_name;
    if (_dns_decode_name(pkt, pkt_len, offset, &after_name, name,
                         sizeof(name)) != 0) {
      return -1;
    }
    offset = after_name;
    if (offset + 10 > pkt_len) return -1;
    uint16_t rtype = _read_u16_be(pkt + offset);
    uint16_t rclass = _read_u16_be(pkt + offset + 2);
    uint32_t ttl = ((uint32_t)pkt[offset + 4] << 24) |
                   ((uint32_t)pkt[offset + 5] << 16) |
                   ((uint32_t)pkt[offset + 6] << 8) |
                   (uint32_t)pkt[offset + 7];
    uint16_t rdlength = _read_u16_be(pkt + offset + 8);
    offset += 10;
    if (offset + rdlength > pkt_len) return -1;
    (void)rclass;
    (void)ttl;

    /* Only consider records in the .offs._tcp.local zone. */
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(MDNS_SERVICE_SUFFIX);
    if (name_len > suffix_len &&
        strcmp(name + name_len - suffix_len, MDNS_SERVICE_SUFFIX) == 0) {
      /* Extract the node_id_base58 (everything before the suffix). */
      size_t b58_len = name_len - suffix_len;
      if (b58_len > 0 && b58_len < sizeof(found_name)) {
        memcpy(found_name, name, b58_len);
        found_name[b58_len] = '\0';

        if (rtype == DNS_TYPE_A && rdlength == 4) {
          found_ip = ((uint32_t)pkt[offset] << 24) |
                     ((uint32_t)pkt[offset + 1] << 16) |
                     ((uint32_t)pkt[offset + 2] << 8) |
                     (uint32_t)pkt[offset + 3];
          have_a = 1;
        } else if (rtype == DNS_TYPE_SRV) {
          if (rdlength >= 6) {
            found_port = _read_u16_be(pkt + offset + 4);  /* after priority+weight */
            have_srv = 1;
          }
        }
      }
    }
    offset += rdlength;
  }

  if (!have_a) return -1;  /* no A record — can't connect without an IP */

  /* Copy out the node_id_base58. If we have no SRV record, fall back to
     the default QUIC listener port (network->quic_listener->listen_port). */
  if (node_id_b58_len < strlen(found_name) + 1) return -1;
  strcpy(node_id_b58, found_name);
  *lan_ip = found_ip;
  *quic_port = have_srv ? found_port : 0;
  return 0;
}

struct mdns_t {
  struct network_t* network;
  scheduler_pool_t* pool;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  int sock_fd;
};

/* Broadcast thread: periodically send an announce packet and listen for
   other nodes' broadcasts. Loops until running is cleared. */
static void* _mdns_thread_fn(void* arg) {
  mdns_t* responder = (mdns_t*)arg;
  if (responder == NULL) return NULL;

  /* Set SO_RCVTIMEO so recvfrom returns periodically (every
     MDNS_RECV_TIMEOUT_S) even if no packets arrive, letting us check the
     running flag and re-broadcast on the interval. */
  struct timeval recv_timeout;
  recv_timeout.tv_sec = MDNS_RECV_TIMEOUT_S;
  recv_timeout.tv_usec = 0;
  setsockopt(responder->sock_fd, SOL_SOCKET, SO_RCVTIMEO,
            &recv_timeout, sizeof(recv_timeout));

  struct sockaddr_in multicast_addr;
  memset(&multicast_addr, 0, sizeof(multicast_addr));
  multicast_addr.sin_family = AF_INET;
  multicast_addr.sin_port = htons(MDNS_MULTICAST_PORT);
  if (inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &multicast_addr.sin_addr) != 1) {
    log_error("mdns: inet_pton failed for multicast group");
    return NULL;
  }

  uint8_t recv_buf[MDNS_MAX_PACKET];
  uint8_t send_buf[MDNS_MAX_PACKET];
  uint64_t last_broadcast_s = 0;

  while (atomic_load(&responder->running)) {
    /* Re-broadcast on the interval. The announce packet is rebuilt each
       tick so a changed node_id (rare, only after a hot restart) is picked
       up. */
    uint64_t now_s = (uint64_t)time(NULL);
    if (now_s - last_broadcast_s >= MDNS_BROADCAST_INTERVAL_S) {
      last_broadcast_s = now_s;
      if (responder->network != NULL &&
          responder->network->authority != NULL &&
          responder->network->quic_listener != NULL) {
        /* Format the node_id_base58 from the local node_id. */
        char node_id_b58[256];
        if (base58_encode(responder->network->authority->local_id.hash,
                          NODE_ID_HASH_SIZE, node_id_b58, sizeof(node_id_b58)) == 0) {
          uint32_t lan_ip = _find_lan_ipv4();
          if (lan_ip != 0) {
            uint16_t quic_port = responder->network->quic_listener->listen_port;
            int pkt_len = _mdns_build_announce(node_id_b58, lan_ip, quic_port,
                                                send_buf, sizeof(send_buf));
            if (pkt_len > 0) {
              ssize_t sent = sendto(responder->sock_fd, send_buf, (size_t)pkt_len,
                                     0, (struct sockaddr*)&multicast_addr,
                                     sizeof(multicast_addr));
              if (sent < 0) {
                log_error("mdns: sendto failed: %s", strerror(errno));
              }
            }
          }
        }
      }
    }

    /* Listen for incoming mDNS packets. */
    struct sockaddr_storage src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t recv_len = recvfrom(responder->sock_fd, recv_buf, sizeof(recv_buf),
                                 0, (struct sockaddr*)&src_addr, &src_len);
    if (recv_len < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        log_error("mdns: recvfrom failed: %s", strerror(errno));
      }
      continue;
    }
    if (recv_len < 12 || (size_t)recv_len > sizeof(recv_buf)) continue;

    char peer_b58[256];
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    if (_mdns_parse_response((const uint8_t*)recv_buf, (size_t)recv_len,
                              peer_b58, sizeof(peer_b58),
                              &peer_ip, &peer_port) != 0) {
      continue;  /* not a liboffs announce, or malformed */
    }

    /* Ignore our own broadcasts. */
    if (responder->network != NULL && responder->network->authority != NULL) {
      char self_b58[256];
      if (base58_encode(responder->network->authority->local_id.hash,
                        NODE_ID_HASH_SIZE, self_b58, sizeof(self_b58)) == 0) {
        if (strcmp(self_b58, peer_b58) == 0) continue;
      }
    }

    /* Decode the peer's node_id from base58, then admit via
       network_connect_peer_candidates with a HOST candidate. */
    node_id_t peer_id;
    memset(&peer_id, 0, sizeof(peer_id));
    if (node_id_from_string(peer_b58, &peer_id) != 0) continue;

    /* Format the peer's IP as a dotted-quad string. */
    char peer_host[16];
    uint8_t ip_bytes[4];
    ip_bytes[0] = (uint8_t)((peer_ip >> 24) & 0xFF);
    ip_bytes[1] = (uint8_t)((peer_ip >> 16) & 0xFF);
    ip_bytes[2] = (uint8_t)((peer_ip >> 8) & 0xFF);
    ip_bytes[3] = (uint8_t)(peer_ip & 0xFF);
    snprintf(peer_host, sizeof(peer_host), "%u.%u.%u.%u",
             ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);

    /* If the announce didn't carry an SRV port, fall back to our own QUIC
       listener port (same-LAN peers are likely on the same port). */
    uint16_t connect_port = peer_port;
    if (connect_port == 0 && responder->network->quic_listener != NULL) {
      connect_port = responder->network->quic_listener->listen_port;
    }
    if (connect_port == 0) continue;  /* nothing to connect to */

    /* Build a single-address peer_info and call
       network_connect_peer_candidates. The peer_address_t host string is
       heap-allocated (strdup) and freed via peer_address_destroy. */
    peer_info_t info;
    memset(&info, 0, sizeof(info));
    info.node_id = peer_id;
    info.addresses = get_clear_memory(sizeof(peer_address_t));
    if (info.addresses == NULL) continue;
    info.address_count = 1;
    info.addresses[0].type = PEER_ADDR_HOST;
    info.addresses[0].port = connect_port;
    info.addresses[0].host = strdup(peer_host);
    if (info.addresses[0].host == NULL) {
      free(info.addresses);
      continue;
    }

    log_info("mdns: discovered peer %s at %s:%u via mDNS",
             peer_b58, peer_host, connect_port);
    (void)network_connect_peer_candidates(responder->network, &peer_id,
                                          info.addresses, info.address_count,
                                          false);
    peer_info_destroy(&info);
  }
  return NULL;
}

mdns_t* mdns_create(struct network_t* network, scheduler_pool_t* pool) {
  (void)pool;
  mdns_t* responder = get_clear_memory(sizeof(mdns_t));
  if (responder == NULL) return NULL;
  responder->network = network;
  responder->sock_fd = -1;
  atomic_store(&responder->running, 0);
  return responder;
}

void mdns_destroy(mdns_t* responder) {
  if (responder == NULL) return;
  mdns_stop(responder);
  free(responder);
}

int mdns_start(mdns_t* responder) {
  if (responder == NULL) return -1;
  if (atomic_load(&responder->running)) return 0;  /* already started */

  /* Create the multicast UDP socket. */
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    log_error("mdns: socket() failed: %s", strerror(errno));
    return -1;
  }

  int reuse = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    log_error("mdns: SO_REUSEADDR failed: %s", strerror(errno));
    close(sock);
    return -1;
  }

  /* Bind to the mDNS port on INADDR_ANY. */
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(MDNS_MULTICAST_PORT);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
    log_error("mdns: bind() failed: %s", strerror(errno));
    close(sock);
    return -1;
  }

  /* Join the multicast group. */
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  if (inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr) != 1) {
    log_error("mdns: inet_pton failed for multicast group");
    close(sock);
    return -1;
  }
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    log_error("mdns: IP_ADD_MEMBERSHIP failed: %s", strerror(errno));
    close(sock);
    return -1;
  }

  /* Set TTL=255 (mDNS spec). */
  unsigned char ttl = 255;
  if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
    log_warn("mdns: IP_MULTICAST_TTL failed: %s", strerror(errno));
  }

  /* Enable loopback so a single-node test can verify the broadcast path
     end-to-end (we filter our own announces by node_id in the thread). */
  unsigned char loop = 1;
  if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
    log_warn("mdns: IP_MULTICAST_LOOP failed: %s", strerror(errno));
  }

  responder->sock_fd = sock;
  atomic_store(&responder->running, 1);
  responder->thread = platform_thread_create(_mdns_thread_fn, responder);
  if (responder->thread == NULL) {
    log_error("mdns: failed to create thread");
    atomic_store(&responder->running, 0);
    close(sock);
    responder->sock_fd = -1;
    return -1;
  }
  return 0;
}

void mdns_stop(mdns_t* responder) {
  if (responder == NULL) return;
  if (!atomic_load(&responder->running)) return;
  atomic_store(&responder->running, 0);
  if (responder->thread != NULL) {
    platform_thread_join(responder->thread);
    responder->thread = NULL;
  }
  if (responder->sock_fd >= 0) {
    close(responder->sock_fd);
    responder->sock_fd = -1;
  }
}

#else /* _WIN32 — stubbed, see mdns.h for the rationale. */

struct mdns_t {
  struct network_t* network;
  scheduler_pool_t* pool;
};

mdns_t* mdns_create(struct network_t* network, scheduler_pool_t* pool) {
  (void)network;
  (void)pool;
  log_warn("mdns: stubbed on Windows — implement WSAStartup + multicast API in a Windows-only task");
  return NULL;
}

void mdns_destroy(mdns_t* responder) {
  (void)responder;
}

int mdns_start(mdns_t* responder) {
  (void)responder;
  return -1;
}

void mdns_stop(mdns_t* responder) {
  (void)responder;
}

#endif /* _WIN32 */