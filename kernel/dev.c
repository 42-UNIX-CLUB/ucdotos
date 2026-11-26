// SPDX-License-Identifier: GPL-2.0-or-later
/* Character devices: null, zero, urandom, and the boot console. */
#include "kernel.h"

static long null_read(struct file *f, u32 b, u32 n) { (void)f; (void)b; (void)n; return 0; }
static long null_write(struct file *f, u32 b, u32 n) { (void)f; (void)b; return n; }
static int ready(struct file *f, int add) { (void)f; (void)add; return POLLIN | POLLOUT; }
static const struct fops null_fops = {null_read, null_write, ready, NULL, NULL};

static long fill_read(struct file *f, u32 ubuf, u32 len) {
  u8 tmp[256];
  int rnd = f->ino->dev == DEV_RANDOM;
  for (u32 done = 0; done < len;) {
    u32 n = len - done < sizeof tmp ? len - done : sizeof tmp;
    if (rnd) for (u32 i = 0; i < n; i += 4) { u32 r = host_random(); memcpy(tmp + i, &r, n - i < 4 ? n - i : 4); }
    else memset(tmp, 0, n);
    if (copy_to_user(ubuf + done, tmp, n)) return -EFAULT;
    done += n;
  }
  return len;
}
static const struct fops fill_fops = {fill_read, null_write, ready, NULL, NULL};

/* console: output goes to the host's log, input is pushed in by the host */
static u8 cin[1024];
static u32 cin_head, cin_len;
static int cin_eof;
static waitq cin_wq;
EXPORT(console_input) void console_input(int c) {
  if (c < 0) cin_eof = 1;
  else if (cin_len < sizeof cin) { cin[(cin_head + cin_len) % sizeof cin] = c; cin_len++; }
  wq_wake(&cin_wq);
}
static long con_read(struct file *f, u32 ubuf, u32 len) {
  if (!cin_len) {
    if (cin_eof || !len) return 0;
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&cin_wq);
    return block(-1);
  }
  u32 n = 0;
  u8 tmp[256];
  while (n < len && n < sizeof tmp && cin_len) { tmp[n++] = cin[cin_head]; cin_head = (cin_head + 1) % sizeof cin; cin_len--; }
  if (copy_to_user(ubuf, tmp, n)) return -EFAULT;
  return n;
}
static long con_write(struct file *f, u32 ubuf, u32 len) {
  (void)f;
  char tmp[512];
  for (u32 done = 0; done < len;) {
    u32 n = len - done < sizeof tmp ? len - done : sizeof tmp;
    if (copy_from_user(tmp, ubuf + done, n)) return -EFAULT;
    host_log(tmp, n);
    done += n;
  }
  return len;
}
static int con_poll(struct file *f, int add) {
  (void)f;
  int m = POLLOUT | ((cin_len || cin_eof) ? POLLIN : 0);
  if (!(m & POLLIN) && add) wq_add(&cin_wq);
  return m;
}
static const struct fops con_fops = {con_read, con_write, con_poll, NULL, NULL};

const struct fops *dev_fops(int dev) {
  switch (dev) {
  case DEV_NULL: return &null_fops;
  case DEV_ZERO: case DEV_RANDOM: return &fill_fops;
  case DEV_CONSOLE: return &con_fops;
  }
  return NULL;
}
