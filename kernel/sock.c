// SPDX-License-Identifier: GPL-2.0-or-later
/* AF_UNIX stream sockets: how programs talk to the window manager.
 * A connection is two byte queues, one per direction. */
#include "net.h"

#define SOCK_BUF (512 * 1024)
#define BACKLOG 16
enum { S_NEW, S_BOUND, S_LISTEN, S_CONN };
struct sock {
  int state, hup;
  struct inode *ino;       /* the bound path, for a listener */
  struct sock *peer;
  u8 *rx;                  /* bytes waiting for me */
  u32 rx_head, rx_len;
  waitq rq, wq;            /* my readers; writers waiting for room in my rx */
  struct sock *queue[BACKLOG];
  int nq;
  waitq accept_wq;
};
struct sockaddr_un { u16 family; char path[108]; };

/* MSG_PEEK leaves the bytes queued; MSG_DONTWAIT is O_NONBLOCK for this call only */
static long sock_recv(struct file *f, u32 ubuf, u32 len, int flags) {
  struct sock *s = f->priv;
  if (s->state != S_CONN) return -EINVAL;
  if (!s->rx_len) {
    if (s->hup || !len) return 0;
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
    wq_add(&s->rq);
    return block(-1);
  }
  if (len > s->rx_len) len = s->rx_len;
  u32 first = SOCK_BUF - s->rx_head < len ? SOCK_BUF - s->rx_head : len;
  if (copy_to_user(ubuf, s->rx + s->rx_head, first) || copy_to_user(ubuf + first, s->rx, len - first)) return -EFAULT;
  if (flags & MSG_PEEK) return len;
  s->rx_head = (s->rx_head + len) % SOCK_BUF;
  s->rx_len -= len;
  if (!s->rx_len) s->rx_head = 0; /* an empty queue starts again at the front: it does not creep through the buffer */
  wq_wake(&s->wq);
  return len;
}
/* A write to a connection whose peer is gone raises SIGPIPE in the writer, as on Linux,
 * unless the caller passed MSG_NOSIGNAL; EPIPE either way (the default action of SIGPIPE
 * ends the writer first). */
static long sock_send(struct file *f, u32 ubuf, u32 len, int flags) {
  struct sock *s = f->priv;
  if (s->state != S_CONN) return -EINVAL;
  struct sock *p = s->peer;
  if (!p) return (flags & MSG_NOSIGNAL) ? -EPIPE : sig_pipe();
  u32 room = SOCK_BUF - p->rx_len;
  if (!room) {
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
    wq_add(&p->wq);
    return block(-1);
  }
  if (len > room) len = room; /* short write; the libc loops */
  u32 tail = (p->rx_head + p->rx_len) % SOCK_BUF;
  u32 first = SOCK_BUF - tail < len ? SOCK_BUF - tail : len;
  if (copy_from_user(p->rx + tail, ubuf, first) || copy_from_user(p->rx, ubuf + first, len - first)) return -EFAULT;
  p->rx_len += len;
  wq_wake(&p->rq);
  return len;
}
static long sock_read(struct file *f, u32 ubuf, u32 len) { return sock_recv(f, ubuf, len, 0); }
static long sock_write(struct file *f, u32 ubuf, u32 len) { return sock_send(f, ubuf, len, 0); }
static int sock_poll(struct file *f, int add) {
  struct sock *s = f->priv;
  int m = 0;
  if (s->state == S_LISTEN) {
    m = s->nq ? POLLIN : 0;
    if (!m && add) wq_add(&s->accept_wq);
    return m;
  }
  if (s->state != S_CONN) return POLLOUT;
  if (s->rx_len) m |= POLLIN;
  if (s->hup) m |= POLLHUP | POLLIN;
  if (s->peer && s->peer->rx_len < SOCK_BUF) m |= POLLOUT;
  if (add) {
    if (!(m & POLLIN)) wq_add(&s->rq);
    if (!(m & POLLOUT) && s->peer) wq_add(&s->peer->wq);
  }
  return m;
}
static void sock_destroy(struct sock *s) {
  if (s->peer) { s->peer->peer = NULL; s->peer->hup = 1; wq_wake(&s->peer->rq); }
  wq_wake(&s->wq); /* writers blocked on my queue get EPIPE on retry */
  wq_wake(&s->rq);
  for (int i = 0; i < s->nq; i++) sock_destroy(s->queue[i]);
  if (s->ino) { s->ino->priv = NULL; inode_put(s->ino); }
  if (s->rx) kfree(s->rx);
  kfree(s);
}
static void sock_close(struct file *f) { sock_destroy(f->priv); }
static const struct fops sock_fops = {sock_read, sock_write, sock_poll, sock_close, NULL};
int unix_sock(struct file *f) { return f->ops == &sock_fops; }

static struct sock *fd_sock(int fd, struct file **fp) {
  struct file *f = fd_get(fd);
  if (!f || f->ops != &sock_fops) return NULL;
  if (fp) *fp = f;
  return f->priv;
}
static long get_path(u32 uaddr, u32 len, char *path) {
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  if (len < 3 || len > sizeof a) return -EINVAL;
  if (copy_from_user(&a, uaddr, len)) return -EFAULT;
  if (a.family != 1) return -EINVAL;
  a.path[sizeof a.path - 1] = 0;
  strlcpy(path, a.path, 108);
  return 0;
}
long sys_socket(int domain, int type, int proto) {
  (void)proto;
  if (domain != AF_UNIX) return inet_socket(domain, type, proto);
  if ((type & 0xf) != SOCK_STREAM) return -EINVAL;
  struct sock *s = kzalloc(sizeof *s);
  if (!s) return -ENOMEM;
  struct file *f = file_new(&sock_fops, NULL, O_RDWR | ((type & O_NONBLOCK) ? O_NONBLOCK : 0), s);
  int fd = f ? fd_alloc(cur, f, 0) : -ENOMEM;
  if (fd < 0) { if (f) file_put(f); else kfree(s); return fd; }
  cur->cloexec[fd] = !!(type & O_CLOEXEC);
  return fd;
}
long sys_bind(int fd, u32 uaddr, u32 len) {
  struct sock *s = fd_sock(fd, NULL);
  if (!s) return inet_bind(fd, uaddr, len);
  if (s->state != S_NEW) return -EINVAL;
  char path[108];
  long e = get_path(uaddr, len, path);
  if (e) return e;
  struct inode *i = vfs_lookup(cur->cwd, path);
  if (i) {
    if ((i->mode & S_IFMT) != S_IFSOCK || i->priv) return -EADDRINUSE;
  } else {
    int err = 0;
    i = vfs_create(cur->cwd, path, S_IFSOCK | 0777, &err);
    if (!i) return err;
  }
  i->priv = s;
  s->ino = i;
  inode_get(i); /* the socket keeps its name's inode alive, even if the name is unlinked */
  s->state = S_BOUND;
  return 0;
}
long sys_listen(int fd, int backlog) {
  (void)backlog;
  struct sock *s = fd_sock(fd, NULL);
  if (!s) return inet_listen(fd, backlog);
  if (s->state != S_BOUND) return -EINVAL;
  s->state = S_LISTEN;
  return 0;
}
long sys_connect(int fd, u32 uaddr, u32 len) {
  struct sock *c = fd_sock(fd, NULL);
  if (!c) return inet_connect(fd, uaddr, len);
  if (c->state != S_NEW) return -EINVAL;
  char path[108];
  long e = get_path(uaddr, len, path);
  if (e) return e;
  struct inode *i = vfs_lookup(cur->cwd, path);
  if (!i) return -ENOENT;
  struct sock *l = (i->mode & S_IFMT) == S_IFSOCK ? i->priv : NULL;
  if (!l || l->state != S_LISTEN || l->nq >= BACKLOG) return -ECONNREFUSED;
  struct sock *srv = kzalloc(sizeof *srv);
  if (!srv) return -ENOMEM;
  c->rx = kmalloc(SOCK_BUF);
  srv->rx = kmalloc(SOCK_BUF);
  if (!c->rx || !srv->rx) { kfree(c->rx); c->rx = NULL; kfree(srv->rx); kfree(srv); return -ENOMEM; }
  srv->state = c->state = S_CONN;
  srv->peer = c; c->peer = srv;
  l->queue[l->nq++] = srv;
  wq_wake(&l->accept_wq);
  return 0;
}
long sys_accept(int fd, u32 uaddr, u32 ulen) {
  struct file *lf;
  struct sock *l = fd_sock(fd, &lf);
  if (!l) return inet_accept(fd, uaddr, ulen);
  if (l->state != S_LISTEN) return -EINVAL;
  if (!l->nq) {
    if (lf->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&l->accept_wq);
    return block(-1);
  }
  struct sock *s = l->queue[0];
  struct file *f = file_new(&sock_fops, NULL, O_RDWR, s);
  int nfd = f ? fd_alloc(cur, f, 0) : -ENOMEM;
  if (nfd < 0) { if (f) kfree(f); return nfd; } /* leave it queued */
  l->nq--;
  memmove(l->queue, l->queue + 1, l->nq * sizeof l->queue[0]);
  if (uaddr && ulen) { /* an unnamed peer: just the family */
    u32 room, size = 2;
    u16 fam = AF_UNIX;
    if (copy_from_user(&room, ulen, 4) == 0 && room >= 2) copy_to_user(uaddr, &fam, 2);
    copy_to_user(ulen, &size, 4);
  }
  return nfd;
}

/* send, sendto, sendmsg, recv, recvfrom, recvmsg: connected AF_UNIX streams only, so an
 * address is refused on send (EISCONN) and never filled in on receive. */
#define SEND_FLAGS (MSG_DONTWAIT | MSG_NOSIGNAL)
#define RECV_FLAGS (MSG_DONTWAIT | MSG_PEEK | MSG_WAITALL)
static struct sock *conn_sock(int fd, struct file **fp, long *err) {
  struct file *f = fd_get(fd);
  if (!f) { *err = -EBADF; return NULL; }
  if (f->ops != &sock_fops) { *err = -ENOTSOCK; return NULL; }
  struct sock *s = f->priv;
  if (s->state != S_CONN) { *err = -ENOTCONN; return NULL; }
  *fp = f;
  return s;
}
long unix_sendto(int fd, u32 ubuf, u32 len, int flags, u32 uaddr) {
  struct file *f; long e;
  if (!conn_sock(fd, &f, &e)) return e;
  if (flags & ~SEND_FLAGS) return -EOPNOTSUPP;
  if (uaddr) return -EISCONN;
  return sock_send(f, ubuf, len, flags);
}
long unix_recvfrom(int fd, u32 ubuf, u32 len, int flags, u32 uaddr, u32 ulen) {
  (void)uaddr; (void)ulen;
  struct file *f; long e;
  if (!conn_sock(fd, &f, &e)) return e;
  if (flags & ~RECV_FLAGS) return -EOPNOTSUPP;
  return sock_recv(f, ubuf, len, flags & ~MSG_WAITALL); /* WAITALL: a short read is allowed here; the libc's callers loop */
}
#define MSG_IOV_MAX 64
static long msg_io(int send, int fd, u32 umsg, int flags) {
  struct file *f; long e;
  if (!conn_sock(fd, &f, &e)) return e;
  if (flags & ~(send ? SEND_FLAGS : RECV_FLAGS)) return -EOPNOTSUPP;
  struct k_msghdr m;
  if (copy_from_user(&m, umsg, sizeof m)) return -EFAULT;
  if (send && m.name) return -EISCONN;
  if (m.iovlen > MSG_IOV_MAX) return -EMSGSIZE;
  struct iovec v[MSG_IOV_MAX];
  if (m.iovlen && copy_from_user(v, m.iov, m.iovlen * sizeof v[0])) return -EFAULT;
  long done = 0;
  for (u32 k = 0; k < m.iovlen; k++) {
    if (!v[k].iov_len) continue;
    /* only the first piece may wait or raise SIGPIPE; after that, a short count is the answer */
    int fl = done ? (flags | MSG_DONTWAIT | MSG_NOSIGNAL) & ~MSG_PEEK : flags & ~MSG_WAITALL;
    if (!send && (flags & MSG_PEEK) && done) break; /* a peek of several pieces sees the first only */
    long r = send ? sock_send(f, (u32)v[k].iov_base, v[k].iov_len, fl) : sock_recv(f, (u32)v[k].iov_base, v[k].iov_len, fl);
    if (r < 0) { if (!done) return r; break; }
    done += r;
    if ((u32)r < v[k].iov_len || (!send && !r)) break;
  }
  if (!send) {
    u32 zero = 0;
    if (copy_to_user(umsg + 4, &zero, 4) || copy_to_user(umsg + 20, &zero, 4) || copy_to_user(umsg + 24, &zero, 4)) return -EFAULT; /* namelen, controllen, flags */
  }
  return done;
}
long sys_sendmsg(int fd, u32 umsg, int flags) { return msg_io(1, fd, umsg, flags); }
long sys_recvmsg(int fd, u32 umsg, int flags) { return msg_io(0, fd, umsg, flags); }
