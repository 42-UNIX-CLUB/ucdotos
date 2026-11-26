// SPDX-License-Identifier: GPL-2.0-or-later
/* Packets, checksums and network interfaces. There is one interface today, the
 * loopback "lo": a datagram sent on it is queued and delivered back to ip_input on a
 * later turn of the event loop (as Linux delivers loopback traffic from a softirq,
 * never inside the sender's call). Loss and delay can be switched on to see TCP
 * recover; they are off by default. */
#include "net.h"

/* ---- packets ---- */
struct pkt *pkt_alloc(u32 len) {
  struct pkt *p = kmalloc(sizeof *p + len);
  if (!p) return NULL;
  memset(p, 0, sizeof *p);
  p->len = len;
  return p;
}
struct pkt *pkt_copy(const struct pkt *src) {
  struct pkt *p = kmalloc(sizeof *p + src->len);
  if (p) { memcpy(p, src, sizeof *p + src->len); p->next = NULL; }
  return p;
}
void pkt_free(struct pkt *p) { kfree(p); }

/* ---- checksums (RFC 1071) ---- */
u32 csum_add(u32 sum, const u8 *p, u32 len) {
  u64 s = sum;
  for (; len > 1; p += 2, len -= 2) s += (u32)(p[0] << 8 | p[1]);
  if (len) s += (u32)p[0] << 8;
  while (s >> 32) s = (s & 0xffffffffu) + (s >> 32);
  return (u32)s;
}
u16 csum_fold(u32 sum) {
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (u16)~sum;
}
u32 csum_pseudo(u32 src, u32 dst, u8 proto, u32 len) {
  u8 ph[12];
  wr32(ph, src); wr32(ph + 4, dst);
  ph[8] = 0; ph[9] = proto; wr16(ph + 10, (u16)len);
  return csum_add(0, ph, 12);
}

/* ---- interfaces ---- */
struct netif *netifs[4];
int nnetif;

struct netif *route(u32 dst) {
  struct netif *def = NULL;
  for (int i = 0; i < nnetif; i++) {
    struct netif *n = netifs[i];
    if (!(n->flags & IFF_UP) || !n->addr) continue;
    if ((dst & n->mask) == (n->addr & n->mask)) return n;
    if (n->flags & IFF_DEFAULT) def = n;
  }
  return def;                         /* everything else: the default route, if there is one */
}
int is_local(u32 addr) {
  for (int i = 0; i < nnetif; i++) {
    struct netif *n = netifs[i];
    if (!(n->flags & IFF_UP)) continue;
    if (addr == n->addr) return 1;
    if ((n->flags & IFF_LOOPBACK) && (addr & n->mask) == (n->addr & n->mask)) return 1; /* all of 127/8 */
  }
  return 0;
}
/* Packet sockets see every datagram an interface sends. A copy is queued with the
 * interface index in saddr and the packet type in sport. */
void netif_tap(struct netif *n, const struct pkt *p, int outgoing) {
  for (struct inet *s = inets; s; s = s->next) {
    if (s->family != AF_PACKET) continue;
    struct pkt *c = pkt_copy(p);
    if (!c) return;
    c->saddr = (u32)n->index;
    c->sport = outgoing ? PACKET_OUTGOING : PACKET_HOST;
    c->off = 0;
    sk_queue(s, c);
  }
}

/* ---- looped interfaces: lo, and gw0 (the router's side of the default route) ----
 * Both hand a sent datagram back to ip_input on a later turn of the event loop. On gw0
 * the destination is somewhere outside the machine; ip_input gives such TCP segments
 * to a transparent listener (the router). */
static struct netif lo = {"lo", 1, IFF_UP | IFF_LOOPBACK, 65535, LOOPBACK_ADDR, 0xff000000u, 0, 0, 0, 0, 0, 0, 0, 0};
static struct netif gw0 = {"gw0", 2, 0, 1500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static struct pkt *q_head, *q_tail;
static struct ktimer q_timer;

static void loop_deliver(void *arg) {
  (void)arg;
  double now = host_now();
  /* Only what was queued before this turn: replies sent while delivering wait for the
   * next turn, so two sockets talking to each other cannot hold the event loop. */
  struct pkt *last = q_tail;
  while (q_head && q_head->due <= now) {
    struct pkt *p = q_head;
    q_head = p->next;
    if (!q_head) q_tail = NULL;
    p->next = NULL;
    struct netif *n = p->dev;
    n->rx_packets++;
    n->rx_bytes += p->len;
    ip_input(n, p);
    if (p == last) break;
  }
  if (q_head) timer_set(&q_timer, q_head->due - host_now(), loop_deliver, NULL);
}
static void loop_xmit(struct netif *n, struct pkt *p) {
  n->tx_packets++;
  n->tx_bytes += p->len;
  netif_tap(n, p, 1);
  if (n->loss_permille && host_random() % 1000 < n->loss_permille) { n->dropped++; pkt_free(p); return; }
  p->due = host_now() + n->delay_ms;
  p->dev = n;
  p->next = NULL;
  if (q_tail) q_tail->next = p; else q_head = p;
  q_tail = p;
  if (!q_timer.armed) timer_set(&q_timer, q_head->due - host_now(), loop_deliver, NULL);
}

void net_init(void) {
  lo.xmit = loop_xmit;
  gw0.xmit = loop_xmit;
  netifs[nnetif++] = &lo;
  netifs[nnetif++] = &gw0;
  vfs_mkdev("/dev/hostnet", DEV_HOSTNET);
}

/* ---- system calls ---- */
long sys_ifinfo(int index, u32 uinfo) {
  if (index < 0 || index >= nnetif) return -ESRCH;
  struct netif *n = netifs[index];
  struct ifinfo i;
  memset(&i, 0, sizeof i);
  strlcpy(i.name, n->name, sizeof i.name);
  i.flags = n->flags; i.mtu = n->mtu; i.addr = n->addr; i.mask = n->mask;
  i.rx_packets = n->rx_packets; i.rx_bytes = n->rx_bytes;
  i.tx_packets = n->tx_packets; i.tx_bytes = n->tx_bytes; i.dropped = n->dropped;
  i.loss_permille = n->loss_permille; i.delay_ms = n->delay_ms;
  return copy_to_user(uinfo, &i, sizeof i);
}
long sys_ifctl(int index, int op, long value) {
  if (index < 0 || index >= nnetif) return -ESRCH;
  struct netif *n = netifs[index];
  switch (op) {
  case IFCTL_UP: if (value) n->flags |= IFF_UP; else n->flags &= ~IFF_UP; return 0;
  case IFCTL_MTU: if (value < 68 || value > 65535) return -EINVAL; n->mtu = (u32)value; return 0;
  case IFCTL_LOSS: if (value < 0 || value > 1000) return -EINVAL; n->loss_permille = (u32)value; return 0;
  case IFCTL_DELAY: if (value < 0 || value > 10000) return -EINVAL; n->delay_ms = (u32)value; return 0;
  case IFCTL_ADDR: if (n->flags & IFF_LOOPBACK) return -EPERM; n->addr = (u32)value; return 0;
  case IFCTL_MASK: if (n->flags & IFF_LOOPBACK) return -EPERM; n->mask = (u32)value; return 0;
  case IFCTL_DEFAULT:
    if (n->flags & IFF_LOOPBACK) return -EPERM;
    for (int k = 0; k < nnetif; k++) netifs[k]->flags &= ~IFF_DEFAULT; /* one default route */
    if (value) n->flags |= IFF_DEFAULT;
    return 0;
  }
  return -EINVAL;
}
