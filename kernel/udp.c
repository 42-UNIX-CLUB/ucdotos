// SPDX-License-Identifier: GPL-2.0-or-later
/* UDP (RFC 768) and raw IP sockets. A raw socket receives a copy of every datagram
 * of its protocol, IP header included, and sends a payload the kernel puts an IP
 * header on (as Linux does without IP_HDRINCL). ping uses one for ICMP. */
#include "net.h"

void udp_input(struct pkt *p) {
  u8 *u = p->data + p->off;
  u32 len = p->len - p->off;
  if (len < 8) goto drop;
  u32 ulen = rd16(u + 4);
  if (ulen < 8 || ulen > len) goto drop;
  if (rd16(u + 6) && csum_fold(csum_add(csum_pseudo(p->saddr, p->daddr, IPPROTO_UDP, ulen), u, ulen)) != 0) goto drop;
  u16 sport = rd16(u), dport = rd16(u + 2);
  /* The best match: a connected socket for this peer, else one bound to the address, else the wildcard. */
  struct inet *best = NULL;
  int score = -1;
  for (struct inet *s = inets; s; s = s->next) {
    if (s->family != AF_INET || s->type != SOCK_DGRAM || s->lport != dport) continue;
    if (s->laddr && s->laddr != p->daddr) continue;
    if (s->connected && (s->raddr != p->saddr || s->rport != sport)) continue;
    int sc = (s->connected ? 2 : 0) + (s->laddr ? 1 : 0);
    if (sc > score) { score = sc; best = s; }
  }
  if (!best) { icmp_unreach(p, 3); goto drop; }     /* port unreachable */
  p->sport = sport;
  p->len = p->off + ulen;
  p->off += 8;
  sk_queue(best, p);
  return;
drop:
  pkt_free(p);
}

/* An ICMP error about a datagram we sent. Like Linux, only a connected socket hears it. */
void udp_err(const u8 *orig, u32 len, int err) {
  u32 ihl = (u32)(orig[0] & 15) * 4;
  if (len < ihl + 8) return;
  u32 src = rd32(orig + 12), dst = rd32(orig + 16);
  u16 sport = rd16(orig + ihl), dport = rd16(orig + ihl + 2);
  for (struct inet *s = inets; s; s = s->next) {
    if (s->family != AF_INET || s->type != SOCK_DGRAM || !s->connected) continue;
    if (s->lport != sport || s->raddr != dst || s->rport != dport) continue;
    if (s->laddr && s->laddr != src) continue;
    s->err = err;
    sk_wake(s);
  }
}

void raw_input(const struct pkt *p, u8 proto) {
  for (struct inet *s = inets; s; s = s->next) {
    if (s->family != AF_INET || s->type != SOCK_RAW || s->proto != proto) continue;
    if (s->laddr && s->laddr != p->daddr) continue;
    if (s->connected && s->raddr != p->saddr) continue;
    struct pkt *c = pkt_copy(p);
    if (!c) return;
    c->off = 0;
    c->sport = 0;
    sk_queue(s, c);
  }
}

long udp_send(struct inet *s, u32 ubuf, u32 len, u32 dst, u16 dport) {
  struct netif *n = route(dst);
  if (!n) return -ENETUNREACH;
  if (len > n->mtu - IP_HDR - 8) return -EMSGSIZE;
  if (!dport) return -EINVAL;
  if (!s->lport) {
    s->lport = port_ephemeral(IPPROTO_UDP);
    if (!s->lport) return -EAGAIN;
  }
  struct pkt *p = pkt_alloc(IP_HDR + 8 + len);
  if (!p) return -ENOBUFS;
  u8 *u = p->data + IP_HDR;
  if (copy_from_user(u + 8, ubuf, len)) { pkt_free(p); return -EFAULT; }
  u32 src = s->laddr ? s->laddr : n->addr;
  wr16(u, s->lport); wr16(u + 2, dport);
  wr16(u + 4, (u16)(8 + len)); wr16(u + 6, 0);
  u16 c = csum_fold(csum_add(csum_pseudo(src, dst, IPPROTO_UDP, 8 + len), u, 8 + len));
  wr16(u + 6, c ? c : 0xffff);                       /* 0 means "no checksum" in UDP */
  long e = ip_send(p, IPPROTO_UDP, src, dst, s->ttl);
  return e < 0 ? e : (long)len;
}

long raw_send(struct inet *s, u32 ubuf, u32 len, u32 dst) {
  struct netif *n = route(dst);
  if (!n) return -ENETUNREACH;
  if (len > n->mtu - IP_HDR) return -EMSGSIZE;
  struct pkt *p = pkt_alloc(IP_HDR + len);
  if (!p) return -ENOBUFS;
  if (copy_from_user(p->data + IP_HDR, ubuf, len)) { pkt_free(p); return -EFAULT; }
  long e = ip_send(p, (u8)s->proto, s->laddr, dst, s->ttl);
  return e < 0 ? e : (long)len;
}
