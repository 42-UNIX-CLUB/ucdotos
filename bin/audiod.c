// SPDX-License-Identifier: GPL-2.0-or-later
/* audiod: the sound server, for everything that is not a game.
 *   audiod [-v]
 * Programs connect to $XDG_RUNTIME_DIR/audiod.sock (/run/audiod.sock without it), send a 20-byte header (struct below), then
 * samples in their own format: 8-bit unsigned or 16-bit signed, mono or stereo, any
 * rate from 4000 to 192000 Hz. audiod converts each to the device's format (linear
 * interpolation, integer arithmetic), applies its volume, mixes them, and writes
 * 20 ms chunks to its own /dev/dsp stream with a large buffer (about 340 ms), so
 * music never skips when the system is busy. Games write to /dev/dsp themselves, with
 * a small buffer; the browser's audio thread mixes the two.
 * Cost: /dev/dsp is held only while there are clients; idle, audiod sleeps in poll().
 * Playing, it wakes about once per 21 ms fragment. A client that gives the number of
 * bytes it will send (total, in the header) gets one byte back, and the connection
 * closed, once all of them are mixed, so it can wait for its sound to be taken. With
 * total 0 it just closes the connection when it is done. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* The sound server runs in the desktop user's session (as on Linux, where PipeWire and
 * PulseAudio do), so its socket is in that session's runtime directory. A program started
 * with no environment (a test run as pid 1) uses /run, as before users existed. */
static char sock_path[108];
static const char *audiod_socket(void) {
  const char *x = getenv("XDG_RUNTIME_DIR");
  snprintf(sock_path, sizeof sock_path, "%s/audiod.sock", x && *x ? x : "/run");
  return sock_path;
}
#define SOCK_PATH audiod_socket()
#define MAX_CLIENTS 16
#define IN_CAP 65536              /* raw bytes buffered per client */
#define FRAG_SHIFT 12             /* /dev/dsp: 16 fragments of 4 KiB, about 340 ms at 48 kHz */
#define FRAG_COUNT 16

struct audiod_hello {             /* what a client sends first; little-endian */
  char magic[4];                  /* "UCAU" */
  unsigned rate;
  unsigned short channels, bits, volume, pad; /* volume 0 to 100 */
  unsigned total;                 /* bytes of samples that follow; 0 = until the connection closes */
};
struct client {
  int fd, used, eof, channels, bits, volume, have_hello;
  unsigned rate, step, pos;       /* pos: 16.16 fixed point, in input frames from head */
  unsigned char in[IN_CAP];
  int head, len;
  unsigned got;                   /* sample bytes received */
  struct audiod_hello hello;
  int hello_len;
};
static struct client clients[MAX_CLIENTS];
static int dsp = -1, verbose;
static unsigned rate = 48000;
static int acc[2 * 4096];
static short out[2 * 4096];

static int nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
static int frame_bytes(const struct client *c) { return c->channels * c->bits / 8; }
static inline void sample(const struct client *c, const unsigned char *p, int *l, int *r) {
  if (c->bits == 8) { *l = (p[0] - 128) << 8; *r = c->channels == 2 ? (p[1] - 128) << 8 : *l; }
  else { *l = (short)(p[0] | p[1] << 8); *r = c->channels == 2 ? (short)(p[2] | p[3] << 8) : *l; }
}
static int dsp_open(void) {
  dsp = open("/dev/dsp", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (dsp < 0) return -1;
  int frag = FRAG_COUNT << 16 | FRAG_SHIFT, r = 48000;
  ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
  ioctl(dsp, SNDCTL_DSP_SPEED, &r);
  rate = (unsigned)r;
  return 0;
}
static void drop(struct client *c) {
  if (verbose) fprintf(stderr, "audiod: client %d done\n", c->fd);
  if (c->hello.total && c->got >= c->hello.total) write(c->fd, "d", 1); /* "all of it is mixed" */
  close(c->fd);
  c->used = 0;
}
static int hello_ok(struct client *c) {
  struct audiod_hello *h = &c->hello;
  if (memcmp(h->magic, "UCAU", 4) || (h->channels != 1 && h->channels != 2) || (h->bits != 8 && h->bits != 16) ||
      h->rate < 4000 || h->rate > 192000) return 0;
  c->channels = h->channels; c->bits = h->bits; c->rate = h->rate;
  c->volume = h->volume > 100 ? 100 : h->volume;
  c->step = (unsigned)(((unsigned long long)c->rate << 16) / rate);
  c->have_hello = 1;
  if (verbose) fprintf(stderr, "audiod: client %d: %u Hz, %d-bit, %d channel(s), volume %d\n", c->fd, c->rate, c->bits, c->channels, c->volume);
  return 1;
}
/* Read what fits; never more than the client's buffer holds, so a fast client waits in write(). */
static void client_read(struct client *c) {
  if (!c->have_hello) {
    long n = read(c->fd, (char *)&c->hello + c->hello_len, sizeof c->hello - (size_t)c->hello_len);
    if (n <= 0) { if (n == 0 || errno != EAGAIN) drop(c); return; }
    c->hello_len += (int)n;
    if (c->hello_len == (int)sizeof c->hello && !hello_ok(c)) drop(c);
    return;
  }
  if (c->head && c->head + c->len > IN_CAP / 2) { memmove(c->in, c->in + c->head, (size_t)c->len); c->head = 0; }
  int room = IN_CAP - c->head - c->len;
  if (room <= 0) return;
  if (c->hello.total && (unsigned)room > c->hello.total - c->got) room = (int)(c->hello.total - c->got);
  long n = room ? read(c->fd, c->in + c->head + c->len, (size_t)room) : 0;
  if (n > 0) { c->len += (int)n; c->got += (unsigned)n; if (c->hello.total && c->got >= c->hello.total) c->eof = 1; }
  else if (n == 0) c->eof = 1;
  else if (errno != EAGAIN) drop(c);
}
/* Convert and add up to n frames of this client into acc. Each input frame is decoded
 * once; the volume is a multiply and a shift (no division per sample); a client already
 * at the device's rate skips interpolation. */
static void client_mix(struct client *c, int n) {
  int fb = frame_bytes(c), frames = c->len / fb, produced = 0;
  int vol = c->volume * 65536 / 100;              /* 16.16 */
  const unsigned char *base = c->in + c->head;
  int *a = acc;
  unsigned pos = c->pos;
  if (c->step == 65536) {
    int k = (int)(pos >> 16);
    for (; produced < n && k < frames; produced++, k++, a += 2) {
      int l, r;
      sample(c, base + k * fb, &l, &r);
      a[0] += (l * vol) >> 16;
      a[1] += (r * vol) >> 16;
    }
    pos = (unsigned)k << 16;
  } else {
    int cur = -2, l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    while (produced < n) {
      int k = (int)(pos >> 16);
      if (k + 1 >= frames) break;                 /* need the next frame to interpolate */
      if (k != cur) {
        if (k == cur + 1) { l0 = l1; r0 = r1; } else sample(c, base + k * fb, &l0, &r0);
        sample(c, base + (k + 1) * fb, &l1, &r1);
        cur = k;
      }
      int f = (int)(pos & 0xffff) >> 1;           /* 15 bits, so the product fits in 32 */
      int l = l0 + (((l1 - l0) * f) >> 15), r = r0 + (((r1 - r0) * f) >> 15);
      a[0] += (l * vol) >> 16;
      a[1] += (r * vol) >> 16;
      a += 2;
      pos += c->step;
      produced++;
    }
  }
  int used = (int)(pos >> 16);
  if (used > frames) used = frames;
  c->pos = pos & 0xffff;
  c->head += used * fb;
  c->len -= used * fb;
  if (c->eof && c->len < 2 * fb) drop(c);       /* everything it sent is mixed */
}
static int has_data(void) {
  for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].used && clients[i].have_hello && clients[i].len >= 2 * frame_bytes(&clients[i])) return 1;
  return 0;
}
static void mix_and_write(void) {
  struct audio_buf_info sp;
  if (ioctl(dsp, SNDCTL_DSP_GETOSPACE, &sp) < 0) return;
  int n = sp.bytes / 4, chunk = (int)rate / 50;   /* 20 ms */
  if (n > chunk) n = chunk;
  if (n > 4096) n = 4096;
  if (n <= 0) return;
  memset(acc, 0, sizeof acc[0] * 2 * (size_t)n);
  for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].used && clients[i].have_hello) client_mix(&clients[i], n);
  for (int i = 0; i < 2 * n; i++) out[i] = (short)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
  write(dsp, out, (size_t)n * 4);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
  else if (argc > 1) { fprintf(stderr, "usage: audiod [-v]\n"); return 2; }
  if (!verbose) setsid();                       /* a daemon: closing a terminal does not stop it */
  mkdir("/run", 0755);
  unlink(SOCK_PATH);
  int lfd = (int)__syscall3(SYS_socket, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */ | O_CLOEXEC, 0);
  struct { unsigned short family; char path[108]; } a = {1, ""};
  snprintf(a.path, sizeof a.path, "%s", SOCK_PATH);
  if (lfd < 0 || __syscall3(SYS_bind, lfd, &a, sizeof a) < 0 || __syscall2(SYS_listen, lfd, 16) < 0) { fprintf(stderr, "audiod: cannot listen on %s\n", SOCK_PATH); return 1; }
  nonblock(lfd);
  if (verbose) fprintf(stderr, "audiod: listening on %s\n", SOCK_PATH);
  for (;;) {
    struct pollfd p[2 + MAX_CLIENTS];
    int who[2 + MAX_CLIENTS], n = 0;
    p[n] = (struct pollfd){lfd, POLLIN, 0}; who[n++] = -1;
    int any = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      struct client *c = &clients[i];
      if (!c->used) continue;
      if (c->eof && (!c->have_hello || c->len < 2 * frame_bytes(c))) { drop(c); continue; } /* nothing left to mix */
      any = 1;
      if (!c->eof && c->len < IN_CAP / 2) { p[n] = (struct pollfd){c->fd, POLLIN, 0}; who[n++] = i; }
    }
    if (any && dsp < 0 && dsp_open() < 0) { perror("audiod: /dev/dsp"); return 1; }
    if (!any && dsp >= 0) { close(dsp); dsp = -1; }  /* idle: give the stream back; what is queued still plays */
    if (dsp >= 0 && has_data()) { p[n] = (struct pollfd){dsp, POLLOUT, 0}; who[n++] = -2; }
    if (poll(p, (unsigned long)n, -1) < 0) continue;
    for (int k = 0; k < n; k++) {
      if (!p[k].revents) continue;
      if (who[k] == -1) {
        int fd = (int)__syscall3(SYS_accept, lfd, 0, 0);
        if (fd < 0) continue;
        struct client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!clients[i].used) { c = &clients[i]; break; }
        if (!c) { close(fd); continue; }
        memset(c, 0, sizeof *c);
        c->fd = fd; c->used = 1;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        nonblock(fd);
      } else if (who[k] == -2) mix_and_write();
      else if (clients[who[k]].used) client_read(&clients[who[k]]);
    }
  }
}
