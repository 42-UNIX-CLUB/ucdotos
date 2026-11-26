// SPDX-License-Identifier: GPL-2.0-or-later
/* /dev/dsp: sound output, in the shape of OSS (Linux's original sound interface).
 * Each open descriptor is one stream. The kernel does not mix: it hands each write to
 * the host, which plays every stream on the browser's audio thread (an AudioWorklet
 * sums them there), so mixing costs the system's one thread nothing. What the kernel
 * keeps per stream is a count of bytes written and not yet played: the host reports
 * what it has played (audio_played), which frees room and wakes writers. That count is
 * the stream's buffer, so its size (SNDCTL_DSP_SETFRAGMENT) is the stream's delay:
 * a game asks for a small one, the sound server for a large one.
 * One format: 16-bit little-endian stereo, at the host's rate (SNDCTL_DSP_SPEED). */
#include "kernel.h"

HOST(audio_rate) int host_audio_rate(void);                   /* frames per second the host plays at */
HOST(audio_open) void host_audio_open(int id);
HOST(audio_write) void host_audio_write(int id, const u8 *p, u32 len); /* copied by the host at once */
HOST(audio_close) void host_audio_close(int id);              /* what is queued still plays */
HOST(audio_volume) void host_audio_volume(int id, int left, int right);
HOST(audio_reset) void host_audio_reset(int id);              /* drop what is queued */

#define NSTREAM 8
#define FRAME 4                       /* bytes per frame: two 16-bit samples */
#define BUF_MAX (256 * 1024)          /* about 1.4 s at 48 kHz */
struct stream {
  int used;
  u32 fragsize, fragcount;            /* the buffer: fragcount fragments of fragsize bytes */
  u32 queued;                         /* bytes written and not yet played */
  int underruns, vol;
  waitq wq;
};
static struct stream streams[NSTREAM];
static u8 xfer[16384];                /* one copy out of the process at a time; not on the stack */

static struct stream *of(struct file *f) { return &streams[(int)(usize)f->priv - 1]; }
static int id_of(struct file *f) { return (int)(usize)f->priv - 1; }
static u32 bufsize(struct stream *s) { return s->fragsize * s->fragcount; }
static u32 space(struct stream *s) { u32 b = bufsize(s); return b > s->queued ? (b - s->queued) & ~(u32)(FRAME - 1) : 0; }

/* ---- host side ---- */
EXPORT(audio_played) void audio_played(int id, u32 frames) {
  if (id < 0 || id >= NSTREAM || !streams[id].used) return;
  struct stream *s = &streams[id];
  u32 b = frames * FRAME;
  s->queued = b > s->queued ? 0 : s->queued - b;
  if (space(s) >= s->fragsize || !s->queued) wq_wake(&s->wq);
}
/* The stream ran dry while it was being fed: a gap the listener hears. */
EXPORT(audio_underrun) void audio_underrun(int id) {
  if (id >= 0 && id < NSTREAM && streams[id].used) streams[id].underruns++;
}

/* ---- file operations ---- */
static long dsp_write(struct file *f, u32 ubuf, u32 len) {
  struct stream *s = of(f);
  len &= ~(u32)(FRAME - 1);
  if (!len) return 0;
  u32 room = space(s);
  if (!room) {
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&s->wq);
    return block(-1);
  }
  u32 n = len < room ? len : room;
  for (u32 done = 0; done < n;) {
    u32 c = n - done < sizeof xfer ? n - done : sizeof xfer;
    if (copy_from_user(xfer, ubuf + done, c)) return done ? (long)done : -EFAULT;
    host_audio_write(id_of(f), xfer, c);
    s->queued += c;
    done += c;
  }
  return n;
}
static long dsp_read(struct file *f, u32 ubuf, u32 len) { (void)f; (void)ubuf; (void)len; return -EINVAL; } /* no recording */
static int dsp_poll(struct file *f, int add) {
  struct stream *s = of(f);
  int m = space(s) >= s->fragsize ? POLLOUT : 0;
  if (!m && add) wq_add(&s->wq);
  return m;
}
static long dsp_ioctl(struct file *f, u32 cmd, u32 arg) {
  struct stream *s = of(f);
  int v = 0;
  if ((cmd & 0x40000000) && copy_from_user(&v, arg, 4)) return -EFAULT; /* the call carries a value in */
  switch (cmd) {
  case SNDCTL_DSP_SPEED: v = host_audio_rate(); break;             /* OSS: the driver answers with the rate it uses */
  case SNDCTL_DSP_SETFMT: case SNDCTL_DSP_GETFMTS: v = AFMT_S16_LE; break;
  case SNDCTL_DSP_CHANNELS: v = 2; break;
  case SNDCTL_DSP_STEREO: v = 1; break;
  case SNDCTL_DSP_GETBLKSIZE: v = (int)s->fragsize; break;
  case SNDCTL_DSP_SETFRAGMENT: {
    if (s->queued) return -EBUSY;                                    /* OSS: before the first write */
    u32 count = (u32)v >> 16, shift = (u32)v & 0xffff;
    if (shift < 8) shift = 8;
    if (shift > 16) shift = 16;
    if (count < 2) count = 2;
    while (count > 2 && (count << shift) > BUF_MAX) count--;
    s->fragsize = 1u << shift;
    s->fragcount = count;
    v = (int)(count << 16 | shift);
    break;
  }
  case SNDCTL_DSP_GETOSPACE: {
    struct audio_buf_info i = {(int)(space(s) / s->fragsize), (int)s->fragcount, (int)s->fragsize, (int)space(s)};
    return copy_to_user(arg, &i, sizeof i);
  }
  case SNDCTL_DSP_GETODELAY: v = (int)s->queued; break;
  case SNDCTL_DSP_SETPLAYVOL: {
    int l = v & 0xff, r = (v >> 8) & 0xff;
    if (l > 100) l = 100;
    if (r > 100) r = 100;
    s->vol = l | r << 8;
    host_audio_volume(id_of(f), l, r);
    v = s->vol;
    break;
  }
  case SNDCTL_DSP_GETERROR: {
    struct audio_errinfo e;
    memset(&e, 0, sizeof e);
    e.play_underruns = s->underruns;
    s->underruns = 0;                                                /* OSS: reading clears the counters */
    return copy_to_user(arg, &e, sizeof e);
  }
  case SNDCTL_DSP_RESET: host_audio_reset(id_of(f)); s->queued = 0; wq_wake(&s->wq); return 0;
  case SNDCTL_DSP_SYNC:                                              /* wait until everything has played */
    if (!s->queued) return 0;
    if (f->flags & O_NONBLOCK) return -EAGAIN;
    wq_add(&s->wq);
    return block(-1);
  default: return -ENOTTY;
  }
  return copy_to_user(arg, &v, 4);
}
static void dsp_close(struct file *f) {
  struct stream *s = of(f);
  host_audio_close(id_of(f));
  wq_wake(&s->wq);
  memset(s, 0, sizeof *s);
}
static const struct fops dsp_fops = {dsp_read, dsp_write, dsp_poll, dsp_close, dsp_ioctl};

struct file *dsp_open(struct inode *ino, int flags) {
  for (int i = 0; i < NSTREAM; i++) {
    if (streams[i].used) continue;
    struct file *f = file_new(&dsp_fops, ino, flags, (void *)(usize)(i + 1));
    if (!f) return NULL;
    struct stream *s = &streams[i];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->fragsize = 4096;                                              /* default: 16 KiB, about 85 ms */
    s->fragcount = 4;
    s->vol = 100 | 100 << 8;
    host_audio_open(i);
    return f;
  }
  return NULL;
}
void audio_init(void) { vfs_mkdev("/dev/dsp", DEV_DSP); }
