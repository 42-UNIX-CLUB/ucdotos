// SPDX-License-Identifier: GPL-2.0-or-later
/* /dev/hostnet: the router's line to the world outside the page. Each open descriptor
 * is one request the host carries out with what it has: in a browser, fetch() and DNS
 * over HTTPS; under Node, also real TCP sockets. What a host can do is reported by
 * HNIOC_CAPS. The data path:
 *   write()  -> host.hn_send(id, bytes)            (copied by the host at once)
 *   host     -> hn_alloc + hn_push(id, bytes)      (queued here until read)
 *   host     -> hn_end(id, err)                    (end of the reply, or why it failed)
 * When the queue holds HN_ROOM bytes, hn_push tells the host to pause; the kernel calls
 * host.hn_more(id) once the reader has drained it to half. */
#include "net.h"

HOST(hn_caps) u32 host_hn_caps(void);
HOST(hn_open) void host_hn_open(int id, int op, int port, const char *host, u32 len);
HOST(hn_send) void host_hn_send(int id, const u8 *p, u32 len);
HOST(hn_done) void host_hn_done(int id);
HOST(hn_close) void host_hn_close(int id);
HOST(hn_more) void host_hn_more(int id);

#define HN_MAX 32
#define HN_ROOM (256 * 1024)
struct seg { struct seg *next; u32 len, off; u8 data[]; };
struct chan {
  int used, opened, ended, err, paused;
  struct seg *head, *tail;
  u32 bytes;
  waitq rq;
};
static struct chan chans[HN_MAX];

static struct chan *chan_of(struct file *f) { return &chans[(int)(usize)f->priv - 1]; }
static int id_of(struct file *f) { return (int)(usize)f->priv - 1; }

/* ---- host side ---- */
EXPORT(hn_alloc) u8 *hn_alloc(u32 len) {
  struct seg *s = kmalloc(sizeof *s + len);
  if (!s) return NULL;
  s->next = NULL; s->len = len; s->off = 0;
  return s->data;
}
/* Takes the buffer from hn_alloc. Returns the room left; 0 or less means "pause until hn_more". */
EXPORT(hn_push) int hn_push(int id, u8 *data, u32 len) {
  struct seg *s = (struct seg *)(data - __builtin_offsetof(struct seg, data));
  if (id < 0 || id >= HN_MAX || !chans[id].used || chans[id].ended) { kfree(s); return -1; }
  struct chan *c = &chans[id];
  s->len = len;
  if (c->tail) c->tail->next = s; else c->head = s;
  c->tail = s;
  c->bytes += len;
  wq_wake(&c->rq);
  int room = HN_ROOM - (int)c->bytes;
  if (room <= 0) c->paused = 1;
  return room;
}
EXPORT(hn_end) void hn_end(int id, int err) {
  if (id < 0 || id >= HN_MAX || !chans[id].used) return;
  chans[id].ended = 1;
  chans[id].err = err;
  wq_wake(&chans[id].rq);
}

/* ---- file operations ---- */
static long hn_read(struct file *f, u32 ubuf, u32 len) {
  struct chan *c = chan_of(f);
  if (!c->opened) return -EINVAL;
  if (!c->head) {
    if (c->ended) { int e = c->err; c->err = 0; return e ? -e : 0; }
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&c->rq);
    return block(-1);
  }
  u32 done = 0;
  while (done < len && c->head) {
    struct seg *s = c->head;
    u32 n = s->len - s->off;
    if (n > len - done) n = len - done;
    if (copy_to_user(ubuf + done, s->data + s->off, n)) return done ? (long)done : -EFAULT;
    s->off += n;
    done += n;
    c->bytes -= n;
    if (s->off == s->len) { c->head = s->next; if (!c->head) c->tail = NULL; kfree(s); }
  }
  if (c->paused && c->bytes < HN_ROOM / 2) { c->paused = 0; host_hn_more(id_of(f)); }
  return done;
}
static long hn_write(struct file *f, u32 ubuf, u32 len) {
  struct chan *c = chan_of(f);
  if (!c->opened) return -EINVAL;
  if (c->ended && c->err) return -c->err;
  u8 tmp[4096];
  for (u32 done = 0; done < len;) {
    u32 n = len - done < sizeof tmp ? len - done : sizeof tmp;
    if (copy_from_user(tmp, ubuf + done, n)) return -EFAULT;
    host_hn_send(id_of(f), tmp, n);
    done += n;
  }
  return len;
}
static int hn_poll(struct file *f, int add) {
  struct chan *c = chan_of(f);
  int m = POLLOUT | ((c->head || c->ended) ? POLLIN : 0);
  if (!(m & POLLIN) && add) wq_add(&c->rq);
  return m;
}
static long hn_ioctl(struct file *f, u32 cmd, u32 arg) {
  struct chan *c = chan_of(f);
  switch (cmd) {
  case HNIOC_CAPS: { u32 caps = host_hn_caps(); return copy_to_user(arg, &caps, 4); }
  case HNIOC_OPEN: {
    struct hn_open o;
    if (c->opened) return -EISCONN;
    if (copy_from_user(&o, arg, sizeof o)) return -EFAULT;
    o.host[sizeof o.host - 1] = 0;
    if (o.op < HN_RESOLVE || o.op > HN_TCP || !(host_hn_caps() & (1u << o.op))) return -EOPNOTSUPP;
    c->opened = 1;
    host_hn_open(id_of(f), o.op, o.port, o.host, (u32)strlen(o.host));
    return 0;
  }
  case HNIOC_DONE: if (!c->opened) return -EINVAL; host_hn_done(id_of(f)); return 0;
  }
  return -ENOTTY;
}
static void hn_close(struct file *f) {
  struct chan *c = chan_of(f);
  if (c->opened) host_hn_close(id_of(f));
  while (c->head) { struct seg *s = c->head; c->head = s->next; kfree(s); }
  memset(c, 0, sizeof *c);
}
static const struct fops hn_fops = {hn_read, hn_write, hn_poll, hn_close, hn_ioctl};

struct file *hostnet_open(struct inode *ino, int flags) {
  for (int i = 0; i < HN_MAX; i++) {
    if (chans[i].used) continue;
    struct file *f = file_new(&hn_fops, ino, flags, (void *)(usize)(i + 1));
    if (!f) return NULL;
    memset(&chans[i], 0, sizeof chans[i]);
    chans[i].used = 1;
    return f;
  }
  return NULL;
}
