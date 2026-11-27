// SPDX-License-Identifier: GPL-2.0-or-later
/* Internet and packet sockets: the system calls, on top of tcp.c and udp.c.
 * AF_UNIX stays in sock.c; its calls hand anything else to the functions here. */
#include "net.h"

#define RCVBUF_DEFAULT (256 * 1024)

struct inet *inets;
static const struct fops inet_fops;

void sk_wake(struct inet *s) { wq_wake(&s->rq); wq_wake(&s->wq); }

void sk_queue(struct inet *s, struct pkt *p) {
  u32 n = p->len - p->off;
  if (s->rxq_bytes + n > s->rcvbuf) { pkt_free(p); return; } /* full: dropped, as a datagram may be */
  p->next = NULL;
  if (s->rxq_tail) s->rxq_tail->next = p; else s->rxq = p;
  s->rxq_tail = p;
  s->rxq_bytes += n;
  wq_wake(&s->rq);
}

/* ---- ports ---- */
static int kind(int proto) { return proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM; }
int port_in_use(int proto, u32 addr, u16 port, int reuse) {
  for (struct inet *s = inets; s; s = s->next) {
    if (s->family != AF_INET || s->type != kind(proto) || s->lport != port) continue;
    if (addr && s->laddr && s->laddr != addr) continue;
    if (proto == IPPROTO_TCP) {
      /* As Linux: two sockets may share a port if both asked for SO_REUSEADDR and neither listens. */
      int listening = s->tcb && tcp_state(s->tcb) == TCP_LISTEN;
      if (!(reuse && s->reuse && !listening)) return 1;
    } else if (!(reuse && s->reuse)) return 1;
  }
  return proto == IPPROTO_TCP && tcp_port_used(port, addr, reuse);
}
u16 port_ephemeral(int proto) {
  static u16 next = 49152;
  for (int i = 0; i < 16384; i++) {
    u16 p = next;
    next = next == 65535 ? 49152 : next + 1;
    if (!port_in_use(proto, 0, p, 0)) return p;
  }
  return 0;
}

/* ---- addresses ---- */
static long get_sin(u32 uaddr, u32 len, u32 *addr, u16 *port) {
  struct sockaddr_in a;
  if (!uaddr || len < sizeof a) return -EINVAL;
  if (copy_from_user(&a, uaddr, sizeof a)) return -EFAULT;
  if (a.sin_family != AF_INET) return -EAFNOSUPPORT;
  *addr = __builtin_bswap32(a.sin_addr.s_addr);
  *port = bswap16(a.sin_port);
  return 0;
}
/* Write an address to the caller, as accept(), recvfrom() and getsockname() do:
 * *ulen says how much room there is and comes back as the full size. */
static long put_addr(u32 uaddr, u32 ulen, const void *a, u32 size) {
  if (!uaddr || !ulen) return 0;
  u32 room;
  if (copy_from_user(&room, ulen, 4)) return -EFAULT;
  if (copy_to_user(uaddr, a, room < size ? room : size) || copy_to_user(ulen, &size, 4)) return -EFAULT;
  return 0;
}
static long put_sin(u32 uaddr, u32 ulen, u32 addr, u16 port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = bswap16(port);
  a.sin_addr.s_addr = __builtin_bswap32(addr);
  return put_addr(uaddr, ulen, &a, sizeof a);
}

static struct inet *fd_inet(int fd, struct file **fp) {
  struct file *f = fd_get(fd);
  if (!f || f->ops != &inet_fops) return NULL;
  if (fp) *fp = f;
  return f->priv;
}

/* ---- datagram receive (UDP, raw, packet) ---- */
static long dgram_recv(struct inet *s, int nonblock, u32 ubuf, u32 len, int peek, u32 uaddr, u32 ulen) {
  if (!s->rxq) {
    if (s->err) { int e = s->err; s->err = 0; return -e; }
    if (s->shut_rd) return 0;
    if (nonblock) return -EAGAIN;
    wq_add(&s->rq);
    return block(-1);
  }
  struct pkt *p = s->rxq;
  u32 n = p->len - p->off;
  if (n > len) n = len;
  if (copy_to_user(ubuf, p->data + p->off, n)) return -EFAULT;
  long e;
  if (s->family == AF_PACKET) {
    struct sockaddr_ll a;
    memset(&a, 0, sizeof a);
    a.sll_family = AF_PACKET;
    a.sll_protocol = bswap16(ETH_P_IP);
    a.sll_ifindex = (int)p->saddr;
    a.sll_hatype = 772;                               /* ARPHRD_LOOPBACK */
    a.sll_pkttype = (u8)p->sport;
    e = put_addr(uaddr, ulen, &a, sizeof a);
  } else e = put_sin(uaddr, ulen, p->saddr, s->type == SOCK_RAW ? 0 : p->sport);
  if (e) return e;
  if (!peek) {
    s->rxq = p->next;
    if (!s->rxq) s->rxq_tail = NULL;
    s->rxq_bytes -= p->len - p->off;
    pkt_free(p);
  }
  return n;
}

/* ---- file operations ---- */
static long do_recv(struct file *f, u32 ubuf, u32 len, int flags, u32 uaddr, u32 ulen) {
  struct inet *s = f->priv;
  int nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
  if (s->type == SOCK_STREAM) {
    long r = tcp_read(s, ubuf, len, nonblock, flags & MSG_PEEK);
    if (r >= 0 && uaddr) { long e = put_sin(uaddr, ulen, s->raddr, s->rport); if (e) return e; }
    return r;
  }
  return dgram_recv(s, nonblock, ubuf, len, flags & MSG_PEEK, uaddr, ulen);
}
static long do_send(struct file *f, u32 ubuf, u32 len, int flags, u32 uaddr, u32 alen) {
  struct inet *s = f->priv;
  int nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
  if (s->family == AF_PACKET) return -EOPNOTSUPP;
  if (s->type == SOCK_STREAM) return tcp_write(s, ubuf, len, nonblock);
  if (s->err) { int e = s->err; s->err = 0; return -e; }
  u32 dst = s->raddr;
  u16 port = s->rport;
  if (uaddr) { long e = get_sin(uaddr, alen, &dst, &port); if (e) return e; }
  else if (!s->connected) return -EDESTADDRREQ;
  if (s->type == SOCK_RAW) return raw_send(s, ubuf, len, dst);
  return udp_send(s, ubuf, len, dst, port);
}
static long inet_read(struct file *f, u32 ubuf, u32 len) { return do_recv(f, ubuf, len, 0, 0, 0); }
static long inet_write(struct file *f, u32 ubuf, u32 len) { return do_send(f, ubuf, len, 0, 0, 0); }
static int inet_poll(struct file *f, int add) {
  struct inet *s = f->priv;
  int m;
  if (s->type == SOCK_STREAM) m = tcp_poll(s);
  else {
    m = POLLOUT;
    if (s->rxq || s->shut_rd) m |= POLLIN;
    if (s->err) m |= POLLERR;
  }
  if (add) {
    if (!(m & POLLIN)) wq_add(&s->rq);
    if (!(m & POLLOUT)) wq_add(&s->wq);
  }
  return m;
}
static void inet_close(struct file *f) {
  struct inet *s = f->priv;
  for (struct inet **pp = &inets; *pp; pp = &(*pp)->next)
    if (*pp == s) { *pp = s->next; break; }
  if (s->type == SOCK_STREAM) tcp_close(s);
  while (s->rxq) { struct pkt *p = s->rxq; s->rxq = p->next; pkt_free(p); }
  sk_wake(s);
  kfree(s);
}
static const struct fops inet_fops = {inet_read, inet_write, inet_poll, inet_close, NULL};

/* ---- system calls ---- */
long inet_socket(int domain, int type, int proto) {
  int t = type & 0xf;
  if (domain == AF_INET) {
    if (t == SOCK_STREAM) { if (proto && proto != IPPROTO_TCP) return -EPROTONOSUPPORT; proto = IPPROTO_TCP; }
    else if (t == SOCK_DGRAM) { if (proto && proto != IPPROTO_UDP) return -EPROTONOSUPPORT; proto = IPPROTO_UDP; }
    else if (t == SOCK_RAW) { if (proto <= 0 || proto > 255 || proto == IPPROTO_TCP || proto == IPPROTO_UDP) return -EPROTONOSUPPORT; }
    else return -EPROTOTYPE;
  } else if (domain == AF_PACKET) {
    if (t != SOCK_DGRAM && t != SOCK_RAW) return -EPROTOTYPE;
    if (bswap16((u16)proto) != ETH_P_ALL && bswap16((u16)proto) != ETH_P_IP) return -EPROTONOSUPPORT;
  } else return -EAFNOSUPPORT;
  struct inet *s = kzalloc(sizeof *s);
  if (!s) return -ENOMEM;
  s->family = domain; s->type = t; s->proto = proto;
  s->rcvbuf = RCVBUF_DEFAULT;
  s->owner = cur->pid;
  struct file *f = file_new(&inet_fops, NULL, O_RDWR | (type & O_NONBLOCK), s);
  int fd = f ? fd_alloc(cur, f, 0) : -ENOMEM;
  if (fd < 0) { if (f) kfree(f); kfree(s); return fd; }
  cur->cloexec[fd] = !!(type & O_CLOEXEC);
  s->next = inets;
  inets = s;
  return fd;
}
long inet_bind(int fd, u32 uaddr, u32 len) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return -ENOTSOCK;
  if (s->family == AF_PACKET) return 0;                /* one interface: binding chooses nothing */
  if (s->lport || s->tcb) return -EINVAL;
  u32 addr; u16 port;
  long e = get_sin(uaddr, len, &addr, &port);
  if (e) return e;
  if (addr && !is_local(addr)) return -EADDRNOTAVAIL;
  if (s->type == SOCK_RAW) { s->laddr = addr; return 0; }
  if (!port) port = port_ephemeral(s->proto);
  else if (port_in_use(s->proto, addr, port, s->reuse)) return -EADDRINUSE;
  if (!port) return -EADDRINUSE;
  s->laddr = addr;
  s->lport = port;
  return 0;
}
long inet_listen(int fd, int backlog) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return -ENOTSOCK;
  if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
  return tcp_listen(s, backlog);
}
long inet_connect(int fd, u32 uaddr, u32 len) {
  struct file *f;
  struct inet *s = fd_inet(fd, &f);
  if (!s) return -ENOTSOCK;
  if (s->family == AF_PACKET) return -EOPNOTSUPP;
  u32 addr; u16 port;
  if (s->type == SOCK_STREAM && s->tcb) return tcp_connect(s, f->flags & O_NONBLOCK); /* a retry, or a second call */
  long e = get_sin(uaddr, len, &addr, &port);
  if (e) return e;
  if (s->type == SOCK_STREAM) {
    if (!port) return -ECONNREFUSED;
    s->raddr = addr; s->rport = port;
    return tcp_connect(s, f->flags & O_NONBLOCK);
  }
  struct netif *n = route(addr);
  if (!n) return -ENETUNREACH;
  if (s->type == SOCK_DGRAM && !s->lport) {
    s->lport = port_ephemeral(IPPROTO_UDP);
    if (!s->lport) return -EAGAIN;
  }
  if (!s->laddr && s->type == SOCK_DGRAM) s->laddr = n->addr;
  s->raddr = addr; s->rport = port;
  s->connected = 1;
  return 0;
}
long inet_accept(int fd, u32 uaddr, u32 ulen) {
  struct file *lf;
  struct inet *l = fd_inet(fd, &lf);
  if (!l) return -ENOTSOCK;
  if (l->type != SOCK_STREAM) return -EOPNOTSUPP;
  struct inet *s = NULL;
  long r = tcp_accept(l, &s, lf->flags & O_NONBLOCK);
  if (r) return r;
  struct file *f = file_new(&inet_fops, NULL, O_RDWR, s);
  int nfd = f ? fd_alloc(cur, f, 0) : -ENOMEM;
  if (nfd < 0) { if (f) file_put(f); else inet_close(&(struct file){.priv = s}); return nfd; }
  put_sin(uaddr, ulen, s->raddr, s->rport);
  return nfd;
}
long sys_getsockname(int fd, u32 uaddr, u32 ulen) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return fd_get(fd) ? -EOPNOTSUPP : -EBADF;
  return put_sin(uaddr, ulen, s->laddr, s->lport);
}
long sys_getpeername(int fd, u32 uaddr, u32 ulen) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return fd_get(fd) ? -EOPNOTSUPP : -EBADF;
  int conn = s->type == SOCK_STREAM ? (s->tcb && tcp_state(s->tcb) != TCP_LISTEN && tcp_state(s->tcb) != TCP_SYN_SENT && tcp_state(s->tcb) != TCP_CLOSE) : s->connected;
  if (!conn) return -ENOTCONN;
  return put_sin(uaddr, ulen, s->raddr, s->rport);
}
/* send/recv on any socket; on a unix socket (no addresses) they are write and read. */
long sys_sendto(int fd, u32 ubuf, u32 len, int flags, u32 uaddr, u32 alen) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  if (f->ops == &inet_fops) return do_send(f, ubuf, len, flags, uaddr, alen);
  if (unix_sock(f)) return unix_sendto(fd, ubuf, len, flags, uaddr); /* flags, SIGPIPE and MSG_NOSIGNAL: sock.c */
  return -ENOTSOCK;
}
long sys_recvfrom(int fd, u32 ubuf, u32 len, int flags, u32 uaddr, u32 ulen) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  if (f->ops == &inet_fops) return do_recv(f, ubuf, len, flags, uaddr, ulen);
  if (unix_sock(f)) return unix_recvfrom(fd, ubuf, len, flags, uaddr, ulen);
  return -ENOTSOCK;
}
long sys_setsockopt(int fd, int level, int opt, u32 uval, u32 len) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return fd_get(fd) ? -ENOPROTOOPT : -EBADF;
  int v;
  if (len < 4) return -EINVAL;
  if (copy_from_user(&v, uval, 4)) return -EFAULT;
  if (level == SOL_SOCKET) {
    switch (opt) {
    case SO_REUSEADDR: s->reuse = !!v; return 0;
    case SO_RCVBUF: if (s->type != SOCK_STREAM) s->rcvbuf = v < 2048 ? 2048 : v > (1 << 20) ? (1 << 20) : (u32)v; return 0;
    case SO_SNDBUF: case SO_KEEPALIVE: return 0;        /* accepted; TCP buffers are fixed, no keepalive */
    case SO_LINGER: {
      struct linger lg;
      if (len < sizeof lg || copy_from_user(&lg, uval, sizeof lg)) return -EFAULT;
      s->linger0 = lg.l_onoff && lg.l_linger == 0;   /* other timeouts: close() never waits anyway */
      return 0;
    }
    }
  } else if (level == IPPROTO_IP && opt == IP_TRANSPARENT) {
    if (s->type != SOCK_STREAM) return -ENOPROTOOPT;
    s->transparent = !!v;
    return 0;
  } else if (level == IPPROTO_IP && opt == IP_TRANSPARENT_PORT) {
    if (s->type != SOCK_STREAM || v < 0 || v > 65535) return -EINVAL;
    s->tport = (u16)v;
    return 0;
  } else if (level == IPPROTO_IP && opt == IP_TTL) {
    if (v < 1 || v > 255) return -EINVAL;
    s->ttl = (u8)v;
    return 0;
  } else if (level == IPPROTO_TCP && opt == TCP_NODELAY) return 0; /* segments are never held back (no Nagle) */
  return -ENOPROTOOPT;
}
long sys_getsockopt(int fd, int level, int opt, u32 uval, u32 ulen) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return fd_get(fd) ? -ENOPROTOOPT : -EBADF;
  int v;
  if (level == SOL_SOCKET) {
    switch (opt) {
    case SO_ERROR: v = s->err; s->err = 0; break;
    case SO_TYPE: v = s->type; break;
    case SO_REUSEADDR: v = s->reuse; break;
    case SO_RCVBUF: v = s->type == SOCK_STREAM ? 65535 : (int)s->rcvbuf; break;
    case SO_SNDBUF: v = 65536; break;
    case SO_KEEPALIVE: v = 0; break;
    case SO_LINGER: { struct linger lg = {s->linger0, 0}; return put_addr(uval, ulen, &lg, sizeof lg); }
    default: return -ENOPROTOOPT;
    }
  } else if (level == IPPROTO_IP && opt == IP_TRANSPARENT) v = s->transparent;
  else if (level == IPPROTO_IP && opt == IP_TRANSPARENT_PORT) v = s->tport;
  else if (level == IPPROTO_IP && opt == IP_TTL) v = s->ttl ? s->ttl : 64;
  else if (level == IPPROTO_TCP && opt == TCP_NODELAY) v = 1;
  else return -ENOPROTOOPT;
  return put_addr(uval, ulen, &v, 4);
}
long sys_shutdown(int fd, int how) {
  struct inet *s = fd_inet(fd, NULL);
  if (!s) return fd_get(fd) ? -EOPNOTSUPP : -EBADF;
  if (how < SHUT_RD || how > SHUT_RDWR) return -EINVAL;
  if (s->type == SOCK_STREAM) return tcp_shutdown(s, how);
  if (!s->connected) return -ENOTCONN;
  if (how != SHUT_WR) { s->shut_rd = 1; sk_wake(s); }
  return 0;
}
/* Connections first (tcp.c keeps them, including ones whose socket is closed), then
 * the other sockets. TCP sockets that are neither connected nor listening are left out. */
long sys_netstat(int index, u32 uinfo) {
  struct sockinfo i;
  memset(&i, 0, sizeof i);
  if (index < 0) return -ESRCH;
  int nt = tcp_count();
  if (index < nt) { tcp_info(index, &i); return copy_to_user(uinfo, &i, sizeof i); }
  index -= nt;
  for (struct inet *s = inets; s; s = s->next) {
    if (s->type == SOCK_STREAM) continue;
    if (index-- > 0) continue;
    i.proto = s->family == AF_PACKET ? 255 : (u8)s->proto;
    i.state = s->connected ? TCP_ESTABLISHED : TCP_CLOSE;
    i.laddr = s->laddr; i.lport = s->lport; i.raddr = s->raddr; i.rport = s->rport;
    i.owner = (u16)s->owner;
    i.rx_queue = s->rxq_bytes;
    return copy_to_user(uinfo, &i, sizeof i);
  }
  return -ESRCH;
}
