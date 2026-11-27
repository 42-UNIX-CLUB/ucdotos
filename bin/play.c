// SPDX-License-Identifier: GPL-2.0-or-later
/* play: play sound through the sound server (audiod).
 *   play [-v VOLUME] FILE.wav...        PCM WAV: 8 or 16 bit, mono or stereo, any rate
 *   play [-v VOLUME] -t HZ [-s SECONDS] a sine tone (default 1 s)
 * VOLUME is 0 to 100 (default 100). play ends when audiod has taken all of the sound;
 * the last ~0.3 s is still playing then. The browser stays silent until the first click
 * or key press on the page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

struct audiod_hello { char magic[4]; unsigned rate; unsigned short channels, bits, volume, pad; unsigned total; };
static int volume = 100;

/* total: the bytes of samples that will follow, so audiod can say when it has taken them all. */
static int server(unsigned rate, int channels, int bits, unsigned total) {
  int fd = (int)__syscall3(SYS_socket, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0);
  struct { unsigned short family; char path[108]; } a = {1, ""};
  { const char *x = getenv("XDG_RUNTIME_DIR"); snprintf(a.path, sizeof a.path, "%s/audiod.sock", x && *x ? x : "/run"); } /* audiod's socket, in the session (bin/audiod.c) */
  long e = __syscall3(SYS_connect, fd, &a, sizeof a);
  if (e < 0) { fprintf(stderr, "play: cannot reach the sound server (audiod): %s\n", strerror((int)-e)); exit(1); }
  struct audiod_hello h = {{'U', 'C', 'A', 'U'}, rate, (unsigned short)channels, (unsigned short)bits, (unsigned short)volume, 0, total};
  if (write(fd, &h, sizeof h) != sizeof h) { perror("play"); exit(1); }
  return fd;
}
/* Wait for audiod's answer: it has mixed everything we sent. */
static void finish(int fd) {
  char b[16];
  while (read(fd, b, sizeof b) > 0) {}
  close(fd);
}
static int send_all(int fd, const unsigned char *p, long n) {
  while (n > 0) { long w = write(fd, p, (size_t)n); if (w <= 0) return -1; p += w; n -= w; }
  return 0;
}
static unsigned le32(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static int play_wav(const char *path) {
  int f = open(path, O_RDONLY);
  if (f < 0) { fprintf(stderr, "play: %s: %s\n", path, strerror(errno)); return 1; }
  unsigned char h[12], ch[8];
  if (read(f, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fprintf(stderr, "play: %s: not a WAV file\n", path); close(f); return 1; }
  unsigned rate = 0, channels = 0, bits = 0, format = 0, data_size = 0;
  for (;;) {                                   /* walk the chunks to "fmt " and "data" */
    if (read(f, ch, 8) != 8) { fprintf(stderr, "play: %s: no sound data\n", path); close(f); return 1; }
    unsigned size = le32(ch + 4);
    if (!memcmp(ch, "fmt ", 4)) {
      unsigned char fm[40];
      if (size < 16 || size > sizeof fm || read(f, fm, size) != (long)size) { fprintf(stderr, "play: %s: bad format chunk\n", path); close(f); return 1; }
      format = le16(fm); channels = le16(fm + 2); rate = le32(fm + 4); bits = le16(fm + 14);
      if (size & 1) lseek(f, 1, SEEK_CUR);
    } else if (!memcmp(ch, "data", 4)) { data_size = size; break; }
    else lseek(f, (long)(size + (size & 1)), SEEK_CUR);
  }
  if (format != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) {
    fprintf(stderr, "play: %s: only plain PCM, 8 or 16 bit, mono or stereo\n", path);
    close(f);
    return 1;
  }
  unsigned frame = channels * bits / 8;
  data_size -= data_size % frame;
  int s = server(rate, (int)channels, (int)bits, data_size);
  static unsigned char buf[32768];
  long n;
  unsigned left = data_size;
  while (left && (n = read(f, buf, left < sizeof buf ? left : sizeof buf)) > 0) { if (send_all(s, buf, n)) break; left -= (unsigned)n; }
  close(f);
  finish(s);
  return 0;
}

static int play_tone(double hz, double seconds) {
  const unsigned rate = 48000;
  long total = (long)(seconds * rate), done = 0;
  int s = server(rate, 1, 16, (unsigned)total * 2);
  static short buf[4800];
  double phase = 0, step = 2 * M_PI * hz / rate;
  while (done < total) {
    long n = total - done < 4800 ? total - done : 4800;
    for (long i = 0; i < n; i++) {
      double fade = done + i < 480 ? (done + i) / 480.0 : total - done - i < 480 ? (total - done - i) / 480.0 : 1; /* no clicks */
      buf[i] = (short)(sin(phase) * 12000 * fade);
      phase += step;
    }
    if (send_all(s, (unsigned char *)buf, n * 2)) break;
    done += n;
  }
  finish(s);
  return 0;
}

int main(int argc, char **argv) {
  double tone = 0, secs = 1;
  int c;
  while ((c = getopt(argc, argv, "v:t:s:")) != -1) {
    if (c == 'v') volume = atoi(optarg) < 0 ? 0 : atoi(optarg) > 100 ? 100 : atoi(optarg);
    else if (c == 't') tone = strtod(optarg, NULL);
    else if (c == 's') secs = strtod(optarg, NULL);
    else { fprintf(stderr, "usage: play [-v VOLUME] FILE.wav... | play [-v VOLUME] -t HZ [-s SECONDS]\n"); return 2; }
  }
  if (tone > 0) return play_tone(tone, secs);
  if (optind >= argc) { fprintf(stderr, "usage: play [-v VOLUME] FILE.wav... | play [-v VOLUME] -t HZ [-s SECONDS]\n"); return 2; }
  int rc = 0;
  for (int i = optind; i < argc; i++) rc |= play_wav(argv[i]);
  return rc;
}
