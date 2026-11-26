// SPDX-License-Identifier: GPL-2.0-or-later
/* /dev/fb0 (the screen) and /dev/input/event0 (keyboard and pointer).
 * These are the only two things the window manager needs from the kernel. */
#include "kernel.h"

/* ---- framebuffer: 32-bit pixels, bytes in memory are R, G, B, A ---- */
static u8 *fb;
static u32 fb_w, fb_h;
/* Damage: the bounding box of everything written since the host last presented, as
 * x0, y0, x1, y1 (inclusive), empty when x0 > x1. The host takes it with fb_damage().
 * The host is told once per frame (the first write after it took the box), not per write. */
static u32 dmg[4] = {1, 1, 0, 0}, dmg_out[4];
static int dmg_told;
static void damage(u32 off, u32 len) {
  u32 stride = fb_w * 4, last = off + len - 1;
  u32 y0 = off / stride, y1 = last / stride, x0 = 0, x1 = fb_w - 1;
  if (y0 == y1) { x0 = (off % stride) / 4; x1 = (last % stride) / 4; }
  if (dmg[0] > dmg[2]) { dmg[0] = x0; dmg[1] = y0; dmg[2] = x1; dmg[3] = y1; }
  else {
    if (x0 < dmg[0]) dmg[0] = x0;
    if (y0 < dmg[1]) dmg[1] = y0;
    if (x1 > dmg[2]) dmg[2] = x1;
    if (y1 > dmg[3]) dmg[3] = y1;
  }
  if (!dmg_told) { dmg_told = 1; host_fb_dirty(); }
}

/* Linux's FBIOGET_VSCREENINFO layout */
struct fb_bitfield { u32 offset, length, msb_right; };
struct fb_var_screeninfo {
  u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
  struct fb_bitfield red, green, blue, transp;
  u32 nonstd, activate, height, width, accel_flags;
  u32 pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace;
  u32 reserved[4];
};
#define FBIOGET_VSCREENINFO 0x4600

static long fb_read(struct file *f, u32 ubuf, u32 len) {
  u32 size = fb_w * fb_h * 4;
  if (f->off >= size) return 0;
  if (len > size - f->off) len = size - f->off;
  if (copy_to_user(ubuf, fb + f->off, len)) return -EFAULT;
  f->off += len;
  return len;
}
static long fb_write(struct file *f, u32 ubuf, u32 len) {
  u32 size = fb_w * fb_h * 4;
  if (f->off >= size) return -ENOSPC;
  if (len > size - f->off) len = size - f->off;
  if (copy_from_user(fb + f->off, ubuf, len)) return -EFAULT;
  if (len) damage(f->off, len);
  f->off += len;
  return len;
}
static long fb_ioctl(struct file *f, u32 cmd, u32 arg) {
  (void)f;
  if (cmd != FBIOGET_VSCREENINFO) return -ENOTTY;
  struct fb_var_screeninfo v;
  memset(&v, 0, sizeof v);
  v.xres = v.xres_virtual = fb_w; v.yres = v.yres_virtual = fb_h; v.bits_per_pixel = 32;
  v.red.offset = 0; v.green.offset = 8; v.blue.offset = 16; v.transp.offset = 24;
  v.red.length = v.green.length = v.blue.length = v.transp.length = 8;
  return copy_to_user(arg, &v, sizeof v);
}
static int ready(struct file *f, int add) { (void)f; (void)add; return POLLIN | POLLOUT; }
static const struct fops fb_fops = {fb_read, fb_write, ready, NULL, fb_ioctl};

/* ---- input: Linux evdev records ---- */
struct input_event { s64 sec, usec; u16 type, code; s32 value; };
#define NEV 512
static struct input_event evq[NEV];
static u32 ev_head, ev_len;
static waitq ev_wq;

static void ev_push(u16 type, u16 code, s32 value) {
  if (ev_len == NEV) { ev_head = (ev_head + 1) % NEV; ev_len--; } /* drop the oldest */
  struct input_event *e = &evq[(ev_head + ev_len) % NEV];
  double ms = host_now();
  e->sec = (s64)(ms / 1000.0);
  e->usec = (s64)((ms - (double)e->sec * 1000.0) * 1000.0);
  e->type = type; e->code = code; e->value = value;
  ev_len++;
}
/* The host calls this for every key, pointer and resize event. */
EXPORT(input_event) void input_event(int type, int code, int value) {
  ev_push(type, code, value);
  if (type == 0 /* EV_SYN */) wq_wake(&ev_wq);
}
/* The host tells the kernel how big the screen is, at boot and when the window changes. */
EXPORT(fb_resize) u8 *fb_resize(u32 w, u32 h) {
  if (w != fb_w || h != fb_h) {
    u8 *n = kzalloc(w * h * 4);
    if (!n) return fb;
    if (fb) kfree(fb);
    fb = n; fb_w = w; fb_h = h;
    dmg[0] = 0; dmg[1] = 0; dmg[2] = w - 1; dmg[3] = h - 1; /* a new buffer: the next frame shows all of it */
    ev_push(4 /* EV_MSC */, 0x11 /* screen resized */, (s32)((w << 16) | h));
    ev_push(0, 0, 0);
    wq_wake(&ev_wq);
  }
  return fb;
}
EXPORT(fb_addr) u8 *fb_addr(void) { return fb; }
/* The host calls this when it presents: returns the address of x0, y0, x1, y1 (inclusive;
 * empty when x0 > x1) covering every write since the previous call, and starts a new box. */
EXPORT(fb_damage) u32 *fb_damage(void) {
  for (int i = 0; i < 4; i++) dmg_out[i] = dmg[i];
  dmg[0] = 1; dmg[1] = 1; dmg[2] = 0; dmg[3] = 0;
  dmg_told = 0;
  return dmg_out;
}
EXPORT(fb_width) u32 fb_width(void) { return fb_w; }
EXPORT(fb_height) u32 fb_height(void) { return fb_h; }

static long ev_read(struct file *f, u32 ubuf, u32 len) {
  if (len < sizeof(struct input_event)) return -EINVAL;
  if (!ev_len) {
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&ev_wq);
    return block(-1);
  }
  u32 n = 0;
  while (ev_len && n + sizeof(struct input_event) <= len) {
    if (copy_to_user(ubuf + n, &evq[ev_head], sizeof(struct input_event))) return -EFAULT;
    ev_head = (ev_head + 1) % NEV; ev_len--;
    n += sizeof(struct input_event);
  }
  return n;
}
static int ev_poll(struct file *f, int add) {
  (void)f;
  if (!ev_len && add) wq_add(&ev_wq);
  return ev_len ? POLLIN : 0;
}
static const struct fops ev_fops = {ev_read, NULL, ev_poll, NULL, NULL};

struct file *fb_open(struct inode *ino, int flags) { return fb ? file_new(&fb_fops, ino, flags, NULL) : NULL; }
struct file *ev_open(struct inode *ino, int flags) { return file_new(&ev_fops, ino, flags, NULL); }
void fb_init(void) {
  int err;
  vfs_create(vfs_root, "/dev/input", S_IFDIR | 0755, &err);
  vfs_mkdev("/dev/fb0", DEV_FB);
  vfs_mkdev("/dev/input/event0", DEV_INPUT);
}
