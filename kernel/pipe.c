// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel.h"
#define PIPE_SZ 65536
struct pipe { u8 *buf; u32 head, len; int readers, writers; waitq rq, wq; };

static long pipe_read(struct file *f, u32 ubuf, u32 len) {
  struct pipe *p = f->priv;
  if (!p->len) {
    if (!p->writers || !len) return 0;
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&p->rq);
    return block(-1);
  }
  if (len > p->len) len = p->len;
  u32 first = PIPE_SZ - p->head < len ? PIPE_SZ - p->head : len;
  if (copy_to_user(ubuf, p->buf + p->head, first) || copy_to_user(ubuf + first, p->buf, len - first)) return -EFAULT;
  p->head = (p->head + len) % PIPE_SZ;
  p->len -= len;
  wq_wake(&p->wq);
  return len;
}
static long pipe_write(struct file *f, u32 ubuf, u32 len) {
  struct pipe *p = f->priv;
  if (!p->readers) return -EPIPE;
  u32 room = PIPE_SZ - p->len;
  if (!room) {
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&p->wq);
    return block(-1);
  }
  if (len > room) len = room; /* short write; the libc loops */
  u32 tail = (p->head + p->len) % PIPE_SZ;
  u32 first = PIPE_SZ - tail < len ? PIPE_SZ - tail : len;
  if (copy_from_user(p->buf + tail, ubuf, first) || copy_from_user(p->buf, ubuf + first, len - first)) return -EFAULT;
  p->len += len;
  wq_wake(&p->rq);
  return len;
}
static int pipe_poll_r(struct file *f, int add) {
  struct pipe *p = f->priv;
  int m = (p->len ? POLLIN : 0) | (!p->writers ? POLLHUP : 0);
  if (!m && add) wq_add(&p->rq);
  return m;
}
static int pipe_poll_w(struct file *f, int add) {
  struct pipe *p = f->priv;
  int m = (p->len < PIPE_SZ ? POLLOUT : 0) | (!p->readers ? POLLERR : 0);
  if (!m && add) wq_add(&p->wq);
  return m;
}
static void pipe_free(struct pipe *p) { if (!p->readers && !p->writers) { kfree(p->buf); kfree(p); } }
static void pipe_close_r(struct file *f) { struct pipe *p = f->priv; p->readers--; wq_wake(&p->wq); pipe_free(p); }
static void pipe_close_w(struct file *f) { struct pipe *p = f->priv; p->writers--; wq_wake(&p->rq); pipe_free(p); }
static const struct fops pipe_r_fops = {pipe_read, NULL, pipe_poll_r, pipe_close_r, NULL};
static const struct fops pipe_w_fops = {NULL, pipe_write, pipe_poll_w, pipe_close_w, NULL};

int pipe_create(struct file **r, struct file **w) {
  struct pipe *p = kzalloc(sizeof *p);
  if (p) p->buf = kmalloc(PIPE_SZ);
  *r = p && p->buf ? file_new(&pipe_r_fops, NULL, O_RDONLY, p) : NULL;
  *w = *r ? file_new(&pipe_w_fops, NULL, O_WRONLY, p) : NULL;
  if (!*w) {
    kfree(*r);
    if (p) kfree(p->buf);
    kfree(p);
    return -ENOMEM;
  }
  p->readers = p->writers = 1;
  return 0;
}
