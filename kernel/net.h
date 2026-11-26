// SPDX-License-Identifier: GPL-2.0-or-later
/* The network stack's shared declarations: timers, packets, interfaces, IPv4.
 * Addresses and ports are kept in host byte order inside the kernel and converted
 * at the edges (packet headers and struct sockaddr_in). */
#ifndef NET_H
#define NET_H
#include "kernel.h"

/* ---- timers (timer.c). The host runs one timeout for the earliest one. ---- */
struct ktimer {
  double due;
  void (*fn)(void *);
  void *arg;
  struct ktimer *next;
  int armed;
};
void timer_set(struct ktimer *t, double ms, void (*fn)(void *), void *arg);
void timer_cancel(struct ktimer *t);

/* ---- byte order and checksums ---- */
static inline u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void wr16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = (u8)v; }
static inline void wr32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static inline u16 bswap16(u16 v) { return (u16)(v << 8 | v >> 8); }
u32 csum_add(u32 sum, const u8 *p, u32 len);   /* one's complement sum, not folded */
u16 csum_fold(u32 sum);                        /* fold and invert: the value to put in a header */
u32 csum_pseudo(u32 src, u32 dst, u8 proto, u32 len); /* TCP and UDP pseudo-header */

/* ---- packets: one IPv4 datagram, header first ---- */
struct netif;
struct pkt {
  struct pkt *next;
  struct netif *dev;   /* looped interfaces: where it is delivered */
  double due;          /* loopback: when it is delivered */
  u32 len;             /* bytes in data[] */
  u32 saddr, daddr;    /* filled by ip_input, for the transport layer */
  u16 sport, off;      /* UDP: source port; offset of the payload in data[] */
  u8 data[];
};
struct pkt *pkt_alloc(u32 len);
struct pkt *pkt_copy(const struct pkt *p);
void pkt_free(struct pkt *p);

/* ---- interfaces (netif.c) ---- */
struct netif {
  char name[16];
  int index;
  u32 flags, mtu, addr, mask;
  u64 rx_packets, rx_bytes, tx_packets, tx_bytes, dropped;
  u32 loss_permille, delay_ms;
  void (*xmit)(struct netif *, struct pkt *);
};
extern struct netif *netifs[];
extern int nnetif;
struct netif *route(u32 dst);       /* the interface a datagram for dst leaves through */
int is_local(u32 addr);             /* an address this machine answers to */
void netif_tap(struct netif *n, const struct pkt *p, int outgoing); /* copies for packet sockets */
void net_init(void);

/* ---- IPv4 and ICMP (ip.c) ---- */
#define IP_HDR 20
#define LOOPBACK_ADDR 0x7f000001u
long ip_send(struct pkt *p, u8 proto, u32 src, u32 dst, u8 ttl); /* p->data[0..20) is filled in here */
void ip_input(struct netif *n, struct pkt *p);
void icmp_unreach(const struct pkt *orig, u8 code); /* answer a datagram nobody wanted */

/* ---- sockets (inet.c) ---- */
struct tcb;
struct inet {
  int type, proto, family;
  u32 laddr, raddr;
  u16 lport, rport;
  u8 reuse, ttl, connected, shut_rd, connecting, transparent, linger0;
  u16 tport;             /* transparent listener: the one destination port it takes, 0 = all */
  int err;               /* pending error, reported once (SO_ERROR) */
  int owner;
  waitq rq, wq;
  struct pkt *rxq, *rxq_tail; /* datagram, raw and packet sockets */
  u32 rxq_bytes, rcvbuf;
  struct tcb *tcb;       /* a TCP connection, or the listening state */
  struct inet *next;
};
extern struct inet *inets;
int unix_sock(struct file *f);   /* sock.c */
void sk_wake(struct inet *s);
int port_in_use(int proto, u32 addr, u16 port, int reuse);
u16 port_ephemeral(int proto);
void sk_queue(struct inet *s, struct pkt *p); /* consumes p; dropped if the queue is full */

/* ---- UDP and raw (udp.c) ---- */
void udp_input(struct pkt *p);
void udp_err(const u8 *orig_ip, u32 len, int err);
void raw_input(const struct pkt *p, u8 proto);
long udp_send(struct inet *s, u32 ubuf, u32 len, u32 dst, u16 dport);
long raw_send(struct inet *s, u32 ubuf, u32 len, u32 dst);

/* ---- TCP (tcp.c) ---- */
void tcp_input(struct pkt *p);
long tcp_connect(struct inet *s, int nonblock);
long tcp_listen(struct inet *s, int backlog);
long tcp_accept(struct inet *l, struct inet **out, int nonblock);
long tcp_read(struct inet *s, u32 ubuf, u32 len, int nonblock, int peek);
long tcp_write(struct inet *s, u32 ubuf, u32 len, int nonblock);
long tcp_shutdown(struct inet *s, int how);
void tcp_close(struct inet *s);
int tcp_poll(struct inet *s);
int tcp_info(int index, struct sockinfo *out); /* 1 if index named a connection */
int tcp_count(void);
int tcp_state(const struct tcb *t);
int tcp_port_used(u16 port, u32 addr, int reuse);

#endif
