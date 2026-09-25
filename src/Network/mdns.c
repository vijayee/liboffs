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
#include "../Platform/platform_socket.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>

#ifndef _WIN32

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>

/* mDNS multicast group + port (RFC 6762). */
#define MDNS_MULTICAST_ADDR  "224.0.0.251"
#define MDNS_MULTICAST_PORT   5353
#define MDNS_BROADCAST_INTERVAL_S 5
#define MDNS_RECV_TIMEOUT_S   1
#define MDNS_TTL_S            120

/* DNS record types we use. */
#define DNS_TYPE_A     1
#define DNS_TYPE_AAAA  28
#define DNS_TYPE_SRV   33
#define DNS_CLASS_IN   1
#define DNS_CACHE_FLUSH 0x8000  /* high bit of CLASS — mDNS cache-flush bit */

/* Service suffix appended to the node_id label. */
#define MDNS_SERVICE_SUFFIX ".offs._tcp.local"

/* Max DNS packet size we accept. RFC 6762 says mDNS packets must be ≤ 9000
   bytes; we cap at 1452 (typical Ethernet MTU minus IP+UDP headers). */
#define MDNS_MAX_PACKET 1452

/* ff02::fb — the mDNS multicast group for IPv6 (RFC 6762). */
static const uint8_t _mdns_multicast_v6[16] = {
  0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFB
};

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

/* Find the best v6 LAN address: global-unicast 2000::/3, then ULA fc00::/7,
   then link-local fe80::/10. Returns the 16 address bytes plus the owning
   interface's scope/index. Returns 0 on success, -1 if none found. */
static int _find_lan_v6(uint8_t out_addr[16], uint32_t* out_scope) {
  struct ifaddrs* ifaces = NULL;
  if (getifaddrs(&ifaces) != 0) return -1;
  int best_class = -1;  /* 0=global 1=ula 2=link-local */
  int result = -1;
  for (struct ifaddrs* ifa = ifaces; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6) continue;
    struct sockaddr_in6* sa = (struct sockaddr_in6*)ifa->ifa_addr;
    if (IN6_IS_ADDR_LOOPBACK(&sa->sin6_addr)) continue;
    if (IN6_IS_ADDR_V4MAPPED(&sa->sin6_addr)) continue;
    const uint8_t* b = sa->sin6_addr.s6_addr;
    int class;
    if ((b[0] & 0xE0) == 0x20) class = 0;
    else if ((b[0] & 0xFE) == 0xFC) class = 1;
    else if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) class = 2;
    else continue;  /* multicast / reserved */
    if (best_class == -1 || class < best_class) {
      best_class = class;
      memcpy(out_addr, b, 16);
      *out_scope = sa->sin6_scope_id;
      result = 0;
      if (best_class == 0) break;  /* global wins immediately */
    }
  }
  freeifaddrs(ifaces);
  return result;
}

/* Build a DNS response announcing our node. Layout:
   header(12) + <addr record>(name + type + class + ttl + rdlength + rdata)
                + SRV_record(name + type + class + ttl + rdlength + rdata)
   The name is <node_id_base58>.offs._tcp.local. is_v6 selects AAAA (16-byte
   rdata) vs A (4-byte rdata). Returns packet size, -1 on error. */
static int _mdns_build_announce_generic(const char* node_id_b58,
                                        int is_v6,
                                        const uint8_t* rdata,
                                        uint16_t quic_port,
                                        uint8_t* out_buf, size_t out_len) {
  if (out_buf == NULL || out_len < 64) return -1;
  memset(out_buf, 0, out_len);

  char name[256];
  int written = snprintf(name, sizeof(name), "%s%s",
                         node_id_b58, MDNS_SERVICE_SUFFIX);
  if (written <= 0 || (size_t)written >= sizeof(name)) return -1;

  uint16_t rdata_len = is_v6 ? 16 : 4;
  size_t offset = 0;

  /* Header (12 bytes). ID=0 (mDNS). Flags: QR=1 (response), AA=1.
     QDCOUNT=0, ANCOUNT=2, NSCOUNT=0, ARCOUNT=0. */
  if (out_len < 12) return -1;
  _write_u16_be(out_buf + 0, 0);          /* ID */
  _write_u16_be(out_buf + 2, 0x8400);     /* flags: QR=1, AA=1 */
  _write_u16_be(out_buf + 4, 0);          /* QDCOUNT */
  _write_u16_be(out_buf + 6, 2);          /* ANCOUNT — A/AAAA + SRV */
  _write_u16_be(out_buf + 8, 0);          /* NSCOUNT */
  _write_u16_be(out_buf + 10, 0);         /* ARCOUNT */
  offset = 12;

  /* Address record: name + TYPE + CLASS(IN|cache-flush) + TTL(120) +
     RDLENGTH + RDATA (4-byte IPv4 or 16-byte IPv6 in network byte order). */
  if (_dns_encode_name(name, out_buf, out_len, &offset) != 0) return -1;
  if (offset + 10 + rdata_len > out_len) return -1;
  _write_u16_be(out_buf + offset, is_v6 ? DNS_TYPE_AAAA : DNS_TYPE_A);
  offset += 2;
  _write_u16_be(out_buf + offset, DNS_CLASS_IN | DNS_CACHE_FLUSH);
  offset += 2;
  _write_u32_be(out_buf + offset, MDNS_TTL_S);
  offset += 4;
  _write_u16_be(out_buf + offset, rdata_len);
  offset += 2;
  memcpy(out_buf + offset, rdata, rdata_len);
  offset += rdata_len;

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
  size_t srv_rdata_len = offset - target_start + 6;  /* +6 for priority+weight+port */
  _write_u16_be(out_buf + rdlength_offset, (uint16_t)srv_rdata_len);

  return (int)offset;
}

static int _mdns_build_announce(const char* node_id_b58, uint32_t lan_ip,
                                 uint16_t quic_port, uint8_t* out_buf,
                                 size_t out_len) {
  uint8_t v4_rdata[4];
  v4_rdata[0] = (uint8_t)((lan_ip >> 24) & 0xFF);
  v4_rdata[1] = (uint8_t)((lan_ip >> 16) & 0xFF);
  v4_rdata[2] = (uint8_t)((lan_ip >> 8) & 0xFF);
  v4_rdata[3] = (uint8_t)(lan_ip & 0xFF);
  return _mdns_build_announce_generic(node_id_b58, 0, v4_rdata, quic_port,
                                      out_buf, out_len);
}

static int _mdns_build_announce_v6(const char* node_id_b58,
                                    const uint8_t addr6[16],
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len) {
  return _mdns_build_announce_generic(node_id_b58, 1, addr6, quic_port,
                                      out_buf, out_len);
}

/* Parse an incoming DNS response packet. Looks for A/AAAA and SRV records in
   the answer section matching <node_id>.offs._tcp.local. On success, fills
   out node_id_b58 (NODE_ID_STRING_SIZE bytes), lan_ip (host byte order, 0 if
   the announce carried only an AAAA record), quic_port, and — when the
   announce carried an AAAA record — addr6_out (16 bytes, network byte order)
   with *have_v6 set to 1. Returns 0 on success, -1 on no match / malformed
   input / no usable address record. */
static int _mdns_parse_response(const uint8_t* pkt, size_t pkt_len,
                                 char* node_id_b58, size_t node_id_b58_len,
                                 uint32_t* lan_ip, uint16_t* quic_port,
                                 uint8_t addr6_out[16], int* have_v6_out) {
  if (pkt == NULL || pkt_len < 12 || node_id_b58 == NULL ||
      lan_ip == NULL || quic_port == NULL || have_v6_out == NULL) {
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
  uint8_t found_addr6[16];
  int have_aaaa = 0;

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
        } else if (rtype == DNS_TYPE_AAAA && rdlength == 16) {
          memcpy(found_addr6, pkt + offset, 16);
          have_aaaa = 1;
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

  if (!have_a && !have_aaaa) return -1;  /* no usable address record */

  /* Copy out the node_id_base58. If we have no SRV record, fall back to
     the default QUIC listener port (network->quic_listener->listen_port). */
  if (node_id_b58_len < strlen(found_name) + 1) return -1;
  strcpy(node_id_b58, found_name);
  *lan_ip = found_ip;
  if (have_aaaa && addr6_out != NULL) memcpy(addr6_out, found_addr6, 16);
  *have_v6_out = have_aaaa;
  *quic_port = have_srv ? found_port : 0;
  return 0;
}

struct mdns_t {
  struct network_t* network;
  scheduler_pool_t* pool;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  int sock_fd;
  int sock6_fd;  /* AF_INET6 mDNS socket; -1 when unavailable */
};

/* Receive one packet on an mDNS socket. On the v6 socket, extracts the
   arriving interface index from IPV6_PKTINFO. Returns the byte count or -1
   (with *arriving_if set when available). The caller must set
   src->ss_family = AF_INET6 before the call to select the recvmsg path. */
static ssize_t _mdns_recv(int fd, uint8_t* buf, size_t buf_len,
                          struct sockaddr_storage* src, unsigned* arriving_if) {
  *arriving_if = 0;
  if (src->ss_family == AF_INET6) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = buf_len;
    char cmsg_buf[256];
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = src;
    mh.msg_namelen = sizeof(*src);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg_buf;
    mh.msg_controllen = sizeof(cmsg_buf);
    ssize_t received = recvmsg(fd, &mh, 0);
    if (received < 0) return -1;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&mh); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&mh, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
        struct in6_pktinfo* pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
        *arriving_if = pktinfo->ipi6_ifindex;
      }
    }
    return received;
  }
  socklen_t src_len = sizeof(*src);
  return recvfrom(fd, buf, buf_len, 0, (struct sockaddr*)src, &src_len);
}

/* Admission dedup check: a peer already in the connection manager AND
   currently connected does not need admitting — the 5s announce cadence
   would otherwise re-log and re-dial every tick. A known-but-disconnected
   peer still needs admission (the manager owns retries after a drop). */
static bool _mdns_peer_needs_admit(const connection_manager_t* conn_mgr,
                                   const node_id_t* peer_id) {
  peer_connection_t* peer = connection_manager_lookup(conn_mgr, peer_id);
  return peer == NULL || !peer->connected;
}

/* Shared admission tail for v4/v6 discovery: self-filter, decode the node
   id, build the single-candidate peer_info, connect, destroy. */
static void _mdns_admit_peer(mdns_t* responder, const char* peer_b58,
                             const char* peer_host, uint16_t peer_port) {
  if (responder->network == NULL) return;
  /* Ignore our own broadcasts. */
  if (responder->network->authority != NULL) {
    char self_b58[256];
    int self_len = base58_encode(responder->network->authority->local_id.hash,
                                 NODE_ID_HASH_SIZE, self_b58, sizeof(self_b58));
    /* base58_encode writes exactly self_len characters without a
       terminator — terminate before the strcmp. */
    if (self_len > 0) {
      self_b58[self_len] = '\0';
      if (strcmp(self_b58, peer_b58) == 0) {
        return;
      }
    }
  }

  node_id_t peer_id;
  memset(&peer_id, 0, sizeof(peer_id));
  if (node_id_from_string(peer_b58, &peer_id) != 0) return;

  /* Already admitted (and currently connected): skip — the 5s announce
     cadence would otherwise re-log and re-dial every tick. A known-but-
     disconnected peer still gets a re-attempt (the manager owns retries). */
  if (!_mdns_peer_needs_admit(&responder->network->conn_mgr, &peer_id)) {
    log_debug("mdns: peer %s already connected — skipping re-admission",
              peer_b58);
    return;
  }

  /* If the announce didn't carry an SRV port, fall back to our own QUIC
     listener port (same-LAN peers are likely on the same port). */
  uint16_t connect_port = peer_port;
  if (connect_port == 0 && responder->network->quic_listener != NULL) {
    connect_port = responder->network->quic_listener->listen_port;
  }
  if (connect_port == 0) return;  /* nothing to connect to */

  /* Build a single-address peer_info and call
     network_connect_peer_candidates. The peer_address_t host string is
     heap-allocated (strdup) and freed via peer_address_destroy. */
  peer_info_t info;
  memset(&info, 0, sizeof(info));
  info.node_id = peer_id;
  info.addresses = get_clear_memory(sizeof(peer_address_t));
  if (info.addresses == NULL) return;
  info.address_count = 1;
  info.addresses[0].type = PEER_ADDR_HOST;
  info.addresses[0].port = connect_port;
  info.addresses[0].host = strdup(peer_host);
  if (info.addresses[0].host == NULL) {
    free(info.addresses);
    return;
  }

  log_info("mdns: discovered peer %s at %s:%u via mDNS",
           peer_b58, peer_host, connect_port);
  (void)network_connect_peer_candidates(responder->network, &peer_id,
                                        info.addresses, info.address_count,
                                        false);
  peer_info_destroy(&info);
}

/* Admit a v6-discovered peer: link-local addresses get the arriving
   interface's scope (AAAA rdata carries none); global/ULA are taken as-is. */
static void _mdns_admit_v6_peer(mdns_t* responder, const char* peer_b58,
                                const uint8_t addr6[16], uint16_t peer_port,
                                unsigned arriving_if) {
  bool is_link_local = (addr6[0] == 0xFE) && ((addr6[1] & 0xC0) == 0x80);
  platform_address_t peer_addr;
  memset(&peer_addr, 0, sizeof(peer_addr));
  peer_addr.family = PLATFORM_AF_INET6;
  memcpy(peer_addr.inet6.addr, addr6, 16);
  if (is_link_local) {
    if (arriving_if == 0) {
      log_debug("mdns: dropping link-local AAAA without scope knowledge");
      return;
    }
    peer_addr.inet6.scope_id = arriving_if;
  }
  char peer_host[128];
  if (platform_address_to_string(&peer_addr, peer_host, sizeof(peer_host)) != 0) {
    return;
  }
  _mdns_admit_peer(responder, peer_b58, peer_host, peer_port);
}

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
  if (responder->sock6_fd >= 0) {
    setsockopt(responder->sock6_fd, SOL_SOCKET, SO_RCVTIMEO,
               &recv_timeout, sizeof(recv_timeout));
  }

  struct sockaddr_in multicast_addr;
  memset(&multicast_addr, 0, sizeof(multicast_addr));
  multicast_addr.sin_family = AF_INET;
  multicast_addr.sin_port = htons(MDNS_MULTICAST_PORT);
  if (inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &multicast_addr.sin_addr) != 1) {
    log_error("mdns: inet_pton failed for multicast group");
    return NULL;
  }

  struct sockaddr_in6 multicast_addr6;
  memset(&multicast_addr6, 0, sizeof(multicast_addr6));
  multicast_addr6.sin6_family = AF_INET6;
  multicast_addr6.sin6_port = htons(MDNS_MULTICAST_PORT);
  memcpy(&multicast_addr6.sin6_addr, _mdns_multicast_v6, 16);

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
        /* Format the node_id_base58 from the local node_id. base58_encode
           returns the character count without terminating — terminate so
           the announce name encoder reads a clean string. */
        char node_id_b58[256];
        int node_id_len = base58_encode(responder->network->authority->local_id.hash,
                                        NODE_ID_HASH_SIZE, node_id_b58,
                                        sizeof(node_id_b58));
        if (node_id_len > 0) {
          node_id_b58[node_id_len] = '\0';
          uint16_t quic_port = responder->network->quic_listener->listen_port;
          uint32_t lan_ip = _find_lan_ipv4();
          if (lan_ip != 0) {
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

          uint8_t lan_v6[16];
          uint32_t lan_v6_scope = 0;
          if (responder->sock6_fd >= 0 &&
              _find_lan_v6(lan_v6, &lan_v6_scope) == 0) {
            /* ff02::fb is link-scoped — pin the send to the interface owning
               the announced address rather than the kernel default. */
            multicast_addr6.sin6_scope_id = lan_v6_scope;
            int pkt_len = _mdns_build_announce_v6(node_id_b58, lan_v6,
                                                  quic_port, send_buf,
                                                  sizeof(send_buf));
            if (pkt_len > 0) {
              ssize_t sent = sendto(responder->sock6_fd, send_buf,
                                    (size_t)pkt_len, 0,
                                    (struct sockaddr*)&multicast_addr6,
                                    sizeof(multicast_addr6));
              if (sent < 0) {
                log_error("mdns: v6 sendto failed: %s", strerror(errno));
              }
            }
          }
        }
      }
    }

    /* Listen for incoming mDNS packets on the v4 socket. The do/while(0)
       turns each per-packet skip path into a fall-through so the v6 socket
       below is serviced every iteration; the v4 handling itself is
       unchanged. */
    do {
      struct sockaddr_storage src_addr;
      socklen_t src_len = sizeof(src_addr);
      ssize_t recv_len = recvfrom(responder->sock_fd, recv_buf, sizeof(recv_buf),
                                  0, (struct sockaddr*)&src_addr, &src_len);
      if (recv_len < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          log_error("mdns: recvfrom failed: %s", strerror(errno));
        }
        break;
      }
      if (recv_len < 12 || (size_t)recv_len > sizeof(recv_buf)) break;

      char peer_b58[256];
      uint32_t peer_ip = 0;
      uint16_t peer_port = 0;
      /* AAAA payload is parsed out here; wiring it into a v6 candidate is the
         next step (the v4 path below is unchanged). */
      uint8_t peer_addr6[16];
      int peer_have_v6 = 0;
      if (_mdns_parse_response((const uint8_t*)recv_buf, (size_t)recv_len,
                                peer_b58, sizeof(peer_b58),
                                &peer_ip, &peer_port,
                                peer_addr6, &peer_have_v6) != 0) {
        break;  /* not a liboffs announce, or malformed */
      }

      /* A v6-only announce carries no A record, so peer_ip is 0 — the v4
         connect path below has no address to format. Skip candidate
         construction; wiring the AAAA into a v6 candidate on the v6 socket is
         deferred (see the parse above). */
      if (peer_ip == 0) break;

      /* Format the peer's IP as a dotted-quad string. */
      char peer_host[16];
      uint8_t ip_bytes[4];
      ip_bytes[0] = (uint8_t)((peer_ip >> 24) & 0xFF);
      ip_bytes[1] = (uint8_t)((peer_ip >> 16) & 0xFF);
      ip_bytes[2] = (uint8_t)((peer_ip >> 8) & 0xFF);
      ip_bytes[3] = (uint8_t)(peer_ip & 0xFF);
      snprintf(peer_host, sizeof(peer_host), "%u.%u.%u.%u",
               ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);

      _mdns_admit_peer(responder, peer_b58, peer_host, peer_port);
    } while (0);

    /* v6 socket: parse AAAA announces and apply scope to link-locals. */
    if (responder->sock6_fd >= 0) {
      struct sockaddr_storage src6;
      memset(&src6, 0, sizeof(src6));
      src6.ss_family = AF_INET6;  /* selects the recvmsg path in _mdns_recv */
      unsigned arriving_if = 0;
      ssize_t recv6_len = _mdns_recv(responder->sock6_fd, recv_buf,
                                     sizeof(recv_buf), &src6, &arriving_if);
      if (recv6_len < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          log_error("mdns: v6 recvmsg failed: %s", strerror(errno));
        }
      } else if (recv6_len >= 12 && (size_t)recv6_len <= sizeof(recv_buf)) {
        char peer_b58_v6[256];
        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        uint8_t peer_v6[16];
        int peer_has_v6 = 0;
        if (_mdns_parse_response((const uint8_t*)recv_buf, (size_t)recv6_len,
                                  peer_b58_v6, sizeof(peer_b58_v6), &peer_ip,
                                  &peer_port, peer_v6,
                                  &peer_has_v6) == 0 &&
            peer_has_v6) {
          _mdns_admit_v6_peer(responder, peer_b58_v6, peer_v6, peer_port,
                              arriving_if);
        }
      }
    }
  }
  return NULL;
}

mdns_t* mdns_create(struct network_t* network, scheduler_pool_t* pool) {
  (void)pool;
  mdns_t* responder = get_clear_memory(sizeof(mdns_t));
  if (responder == NULL) return NULL;
  responder->network = network;
  responder->sock_fd = -1;
  responder->sock6_fd = -1;
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

  /* v6 socket — non-fatal on failure: log once and stay v4-only. */
  responder->sock6_fd = -1;
  {
    int sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock6 < 0) {
      log_warn("mdns: IPv6 socket unavailable (%s) — staying v4-only",
               strerror(errno));
    } else {
      int v6only = 1;
      (void)setsockopt(sock6, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
      int reuse6 = 1;
      (void)setsockopt(sock6, SOL_SOCKET, SO_REUSEADDR, &reuse6, sizeof(reuse6));
      struct sockaddr_in6 bind6;
      memset(&bind6, 0, sizeof(bind6));
      bind6.sin6_family = AF_INET6;
      bind6.sin6_port = htons(MDNS_MULTICAST_PORT);
      if (bind(sock6, (struct sockaddr*)&bind6, sizeof(bind6)) < 0) {
        log_warn("mdns: IPv6 bind failed: %s", strerror(errno));
        close(sock6);
      } else {
        /* Join ff02::fb on every IPv6-capable interface. The kernel fills
           sin6_scope_id with the owning interface's index on getifaddrs
           results, so no if_nametoindex call is needed. */
        struct ifaddrs* join_ifaces = NULL;
        unsigned joined[64];
        size_t joined_count = 0;
        int join_failures = 0;
        int successful_joins = 0;
        if (getifaddrs(&join_ifaces) == 0) {
          for (struct ifaddrs* jifa = join_ifaces; jifa != NULL;
               jifa = jifa->ifa_next) {
            if (jifa->ifa_addr == NULL ||
                jifa->ifa_addr->sa_family != AF_INET6) continue;
            unsigned ifindex =
                ((struct sockaddr_in6*)jifa->ifa_addr)->sin6_scope_id;
            bool seen = false;
            for (size_t j = 0; j < joined_count; j++) {
              if (joined[j] == ifindex) { seen = true; break; }
            }
            if (seen) continue;
            struct ipv6_mreq mreq6;
            memset(&mreq6, 0, sizeof(mreq6));
            memcpy(&mreq6.ipv6mr_multiaddr, _mdns_multicast_v6, 16);
            mreq6.ipv6mr_interface = ifindex;
            if (setsockopt(sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                           &mreq6, sizeof(mreq6)) < 0) {
              join_failures++;
              log_warn("mdns: IPV6_JOIN_GROUP on %s failed: %s",
                       jifa->ifa_name, strerror(errno));
            } else {
              successful_joins++;
              if (joined_count < 64) {
                joined[joined_count++] = ifindex;
              }
            }
          }
          freeifaddrs(join_ifaces);
        }
        if (successful_joins == 0 && join_failures == 0) {
          log_warn("mdns: no IPv6 interfaces to join — v6 announce disabled");
          close(sock6);
        } else {
          /* All joins failed keeps the socket in announce-only mode (our
             announces still send; other peers' announces won't arrive). */
          /* Receiver scope resolution: need the arriving interface. */
          int pktinfo = 1;
          (void)setsockopt(sock6, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                           &pktinfo, sizeof(pktinfo));
          /* Multicast loop for the single-node round trip, as on v4. */
          int loop6 = 1;
          (void)setsockopt(sock6, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                           &loop6, sizeof(loop6));
          responder->sock6_fd = sock6;
        }
      }
    }
  }

  responder->sock_fd = sock;
  atomic_store(&responder->running, 1);
  responder->thread = platform_thread_create(_mdns_thread_fn, responder);
  if (responder->thread == NULL) {
    log_error("mdns: failed to create thread");
    atomic_store(&responder->running, 0);
    close(sock);
    responder->sock_fd = -1;
    if (responder->sock6_fd >= 0) {
      close(responder->sock6_fd);
      responder->sock6_fd = -1;
    }
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
  if (responder->sock6_fd >= 0) {
    close(responder->sock6_fd);
    responder->sock6_fd = -1;
  }
}

/* Test-only wrappers around static packet helpers (see test_mdns.cpp). */
int mdns_build_announce_v4_for_test(const char* node_id_b58, uint32_t lan_ip,
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len) {
  return _mdns_build_announce(node_id_b58, lan_ip, quic_port, out_buf, out_len);
}

int mdns_build_announce_v6_for_test(const char* node_id_b58,
                                    const uint8_t addr6[16],
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len) {
  return _mdns_build_announce_v6(node_id_b58, addr6, quic_port, out_buf, out_len);
}

int mdns_parse_response_for_test(const uint8_t* pkt, size_t pkt_len,
                                 char* node_id_b58, size_t node_id_b58_len,
                                 uint32_t* lan_ip, uint16_t* quic_port,
                                 uint8_t addr6_out[16], int* have_v6) {
  return _mdns_parse_response(pkt, pkt_len, node_id_b58, node_id_b58_len,
                              lan_ip, quic_port, addr6_out, have_v6);
}

const uint8_t* mdns_multicast_group_v6_for_test(void) {
  return _mdns_multicast_v6;
}

bool mdns_peer_needs_admit_for_test(const connection_manager_t* conn_mgr,
                                    const node_id_t* peer_id) {
  return _mdns_peer_needs_admit(conn_mgr, peer_id);
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

/* The packet-layer test wrappers are declared in mdns.h unconditionally (the
   test binary builds on every platform), so the Windows stub section must
   provide definitions. The POSIX packet helpers don't exist here, so the
   stubs just fail. */
int mdns_build_announce_v4_for_test(const char* node_id_b58, uint32_t lan_ip,
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len) {
  (void)node_id_b58;
  (void)lan_ip;
  (void)quic_port;
  (void)out_buf;
  (void)out_len;
  return -1;
}

int mdns_build_announce_v6_for_test(const char* node_id_b58,
                                    const uint8_t addr6[16],
                                    uint16_t quic_port, uint8_t* out_buf,
                                    size_t out_len) {
  (void)node_id_b58;
  (void)addr6;
  (void)quic_port;
  (void)out_buf;
  (void)out_len;
  return -1;
}

int mdns_parse_response_for_test(const uint8_t* pkt, size_t pkt_len,
                                 char* node_id_b58, size_t node_id_b58_len,
                                 uint32_t* lan_ip, uint16_t* quic_port,
                                 uint8_t addr6_out[16], int* have_v6) {
  (void)pkt;
  (void)pkt_len;
  (void)node_id_b58;
  (void)node_id_b58_len;
  (void)lan_ip;
  (void)quic_port;
  (void)addr6_out;
  (void)have_v6;
  return -1;
}

const uint8_t* mdns_multicast_group_v6_for_test(void) {
  return NULL;
}

/* No connection manager on the Windows stub — always admit (preserves the
   pre-dedup behavior on stubbed platforms). */
bool mdns_peer_needs_admit_for_test(const connection_manager_t* conn_mgr,
                                    const node_id_t* peer_id) {
  (void)conn_mgr;
  (void)peer_id;
  return true;
}

#endif /* _WIN32 */