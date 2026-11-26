// SPDX-License-Identifier: GPL-2.0-or-later
/* IPv4 (RFC 791) and ICMP (RFC 792). No fragmentation: datagrams leave with "don't
 * fragment" set, and fragments that arrive are dropped. No options. */
#include "net.h"

static u16 ip_id;

long ip_send(struct pkt *p, u8 proto, u32 src, u32 dst, u8 ttl) {
  struct netif *n = route(dst);
  if (!n) { pkt_free(p); return -ENETUNREACH; }
  if (p->len > n->mtu) { pkt_free(p); return -EMSGSIZE; }
  if (!src) src = n->addr;
  u8 *h = p->data;
  h[0] = 0x45;               /* version 4, 5 words of header */
  h[1] = 0;
  wr16(h + 2, (u16)p->len);
  wr16(h + 4, ip_id++);
  wr16(h + 6, 0x4000);       /* don't fragment */
  h[8] = ttl ? ttl : 64;
  h[9] = proto;
  wr16(h + 10, 0);
  wr32(h + 12, src);
  wr32(h + 16, dst);
  wr16(h + 10, csum_fold(csum_add(0, h, IP_HDR)));
  n->xmit(n, p);
  return 0;
}

static void icmp_input(struct pkt *p) {
  u8 *ip = p->data, *m = p->data + p->off;
  u32 len = p->len - p->off;
  if (len < 8 || csum_fold(csum_add(0, m, len)) != 0) { pkt_free(p); return; }
  raw_input(p, IPPROTO_ICMP);
  if (m[0] == 8 && m[1] == 0) { /* echo request: answer with the same id, sequence and data */
    struct pkt *r = pkt_alloc(IP_HDR + len);
    if (r) {
      memcpy(r->data + IP_HDR, m, len);
      u8 *e = r->data + IP_HDR;
      e[0] = 0;              /* echo reply */
      wr16(e + 2, 0);
      wr16(e + 2, csum_fold(csum_add(0, e, len)));
      ip_send(r, IPPROTO_ICMP, rd32(ip + 16), rd32(ip + 12), 64);
    }
  } else if (m[0] == 3 && len >= 8 + IP_HDR + 8) { /* destination unreachable: tell the sender's socket */
    const u8 *orig = m + 8;
    int err = m[1] == 3 ? ECONNREFUSED : m[1] == 1 ? EHOSTUNREACH : m[1] == 4 ? EMSGSIZE : ENETUNREACH;
    if (orig[9] == IPPROTO_UDP) udp_err(orig, len - 8, err);
  }
  pkt_free(p);
}

/* Destination unreachable, carrying the offending header and 8 bytes of its payload.
 * Never sent about an ICMP error, so two machines cannot bounce errors forever. */
void icmp_unreach(const struct pkt *orig, u8 code) {
  const u8 *oh = orig->data;
  if (oh[9] == IPPROTO_ICMP) {
    u32 ihl = (u32)(oh[0] & 15) * 4;
    if (orig->len > ihl && oh[ihl] != 0 && oh[ihl] != 8) return;
  }
  u32 quote = orig->len < IP_HDR + 8 ? orig->len : IP_HDR + 8;
  u32 len = 8 + quote;
  struct pkt *r = pkt_alloc(IP_HDR + len);
  if (!r) return;
  u8 *m = r->data + IP_HDR;
  memset(m, 0, 8);
  m[0] = 3; m[1] = code;
  memcpy(m + 8, oh, quote);
  wr16(m + 2, csum_fold(csum_add(0, m, len)));
  ip_send(r, IPPROTO_ICMP, rd32(oh + 16), rd32(oh + 12), 64);
}

void ip_input(struct netif *n, struct pkt *p) {
  u8 *h = p->data;
  if (p->len < IP_HDR || (h[0] >> 4) != 4) goto drop;
  u32 ihl = (u32)(h[0] & 15) * 4;
  u32 total = rd16(h + 2);
  if (ihl < IP_HDR || total < ihl || total > p->len) goto drop;
  if (csum_fold(csum_add(0, h, ihl)) != 0) goto drop;
  if (rd16(h + 6) & 0x3fff) goto drop;   /* a fragment: not supported */
  p->len = total;                         /* drop any padding after the datagram */
  p->saddr = rd32(h + 12);
  p->daddr = rd32(h + 16);
  p->off = (u16)ihl;
  if (!is_local(p->daddr)) {
    /* Bound for outside the machine. Only datagrams our own default route looped back
     * are taken: TCP goes to the router's transparent listener; anything else cannot
     * leave, so the sender hears "host unreachable". The kernel never forwards. */
    if (!(n->flags & IFF_DEFAULT) || (n->flags & IFF_LOOPBACK)) goto drop;
    if (h[9] == IPPROTO_TCP) { tcp_input(p); return; }
    icmp_unreach(p, 1);
    goto drop;
  }
  switch (h[9]) {
  case IPPROTO_ICMP: icmp_input(p); return;
  case IPPROTO_TCP: tcp_input(p); return;
  case IPPROTO_UDP: udp_input(p); return;
  }
  raw_input(p, h[9]);
  icmp_unreach(p, 2);                     /* protocol unreachable */
drop:
  n->dropped++;
  pkt_free(p);
}
