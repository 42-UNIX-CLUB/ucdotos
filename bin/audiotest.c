// SPDX-License-Identifier: GPL-2.0-or-later
/* Sound self-test: /dev/dsp directly (the game path) and through audiod (the
 * multimedia path). Run by tools/tests/audio.mjs, which boots UC.OS under Node with the
 * host saving what it "plays" to a file, and then checks that file sample by sample:
 *   - 1 s of a known pattern written to /dev/dsp appears in it unchanged;
 *   - two audiod clients mixed: 16-bit mono at 24 kHz holding 1000, plus 8-bit mono at
 *     48 kHz holding 2048 at volume 50, appear as a steady 2024 on both channels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int ioc(int fd, unsigned long cmd, int v) { ioctl(fd, cmd, &v); return v; }

static short pattern(long i, int ch) { int l = (int)((i * 97) % 30000) - 15000; return (short)(ch ? -l : l); }

struct audiod_hello { char magic[4]; unsigned rate; unsigned short channels, bits, volume, pad; unsigned total; };
static int unix_socket(void) { return (int)__syscall3(SYS_socket, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0); }
static long unix_connect(int fd) {
  struct { unsigned short family; char path[108]; } a = {1, "/run/audiod.sock"};
  return __syscall3(SYS_connect, fd, &a, sizeof a);
}
static int client(unsigned rate, int bits, int volume, unsigned total) {
  int fd = unix_socket();
  for (int i = 0; i < 50 && unix_connect(fd) < 0; i++) usleep(20000);
  struct audiod_hello h = {{'U', 'C', 'A', 'U'}, rate, 1, (unsigned short)bits, (unsigned short)volume, 0, total};
  write(fd, &h, sizeof h);
  return fd;
}

int main(void) {
  printf("audiotest: /dev/dsp and audiod\n");
  int d = open("/dev/dsp", O_WRONLY);
  CHECK("open /dev/dsp", d >= 0);
  CHECK("SPEED answers with the host's rate (48000 under Node)", ioc(d, SNDCTL_DSP_SPEED, 44100) == 48000);
  CHECK("SETFMT: 8-bit asked, 16-bit given", ioc(d, SNDCTL_DSP_SETFMT, AFMT_U8) == AFMT_S16_LE);
  CHECK("CHANNELS: mono asked, stereo given", ioc(d, SNDCTL_DSP_CHANNELS, 1) == 2);
  CHECK("SETFRAGMENT 4 x 4 KiB", ioc(d, SNDCTL_DSP_SETFRAGMENT, 4 << 16 | 12) == (4 << 16 | 12));
  struct audio_buf_info sp;
  CHECK("GETOSPACE: 16 KiB free", ioctl(d, SNDCTL_DSP_GETOSPACE, &sp) == 0 && sp.bytes == 16384 && sp.fragsize == 4096 && sp.fragstotal == 4);

  /* Non-blocking: a game must never wait. */
  fcntl(d, F_SETFL, O_NONBLOCK);
  static short buf[2 * 48000];
  for (int i = 0; i < 2 * 48000; i++) buf[i] = 0;
  long took = 0, w;
  while ((w = write(d, buf, 4096)) > 0) took += w;
  CHECK("non-blocking: takes exactly the buffer, then EAGAIN", took == 16384 && w < 0 && errno == EAGAIN);
  CHECK("GETODELAY: 16 KiB not yet played", ioc(d, SNDCTL_DSP_GETODELAY, 0) == 16384);
  struct pollfd p = {d, POLLOUT, 0};
  CHECK("poll: not writable while full", poll(&p, 1, 0) == 0);
  CHECK("poll: writable once a fragment has played", poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT));
  CHECK("SETFRAGMENT after writing is EBUSY", ioctl(d, SNDCTL_DSP_SETFRAGMENT, &(int){8 << 16 | 12}) < 0 && errno == EBUSY);
  fcntl(d, F_SETFL, 0);
  CHECK("SYNC waits until everything has played", ioctl(d, SNDCTL_DSP_SYNC, 0) == 0 && ioc(d, SNDCTL_DSP_GETODELAY, 0) == 0);
  usleep(600000);                                 /* longer than a pause the device counts as a gap */

  /* Blocking: 1 s of the pattern takes about 1 s, paced by the device. */
  for (long i = 0; i < 48000; i++) { buf[2 * i] = pattern(i, 0); buf[2 * i + 1] = pattern(i, 1); }
  double t0 = now_ms();
  long wr = write(d, buf, sizeof buf);
  double ms = now_ms() - t0;
  CHECK("blocking write of 1 s is paced by playback (0.8 to 1.3 s)", wr == (long)sizeof buf && ms > 800 && ms < 1300);
  printf("        (%.0f ms)\n", ms);
  ioctl(d, SNDCTL_DSP_SYNC, 0);
  struct audio_errinfo e;
  CHECK("no underrun while fed steadily", ioctl(d, SNDCTL_DSP_GETERROR, &e) == 0 && e.play_underruns == 0);

  usleep(600000);
  /* Starve it on purpose: 50 ms, a 200 ms pause, more. */
  write(d, buf, 48 * 50 * 4);
  usleep(200000);
  write(d, buf, 48 * 50 * 4);
  CHECK("a gap while fed is counted as an underrun", ioctl(d, SNDCTL_DSP_GETERROR, &e) == 0 && e.play_underruns == 1);
  CHECK("reading the count clears it", ioctl(d, SNDCTL_DSP_GETERROR, &e) == 0 && e.play_underruns == 0);
  CHECK("SETPLAYVOL keeps 0 to 100", ioc(d, SNDCTL_DSP_SETPLAYVOL, 150 | 30 << 8) == (100 | 30 << 8));
  ioctl(d, SNDCTL_DSP_RESET, 0);
  CHECK("RESET drops what is queued", ioc(d, SNDCTL_DSP_GETODELAY, 0) == 0);

  /* Garbage: random ioctls with random arguments (pointers among them, valid or not),
   * writes of odd sizes from odd addresses. The device must keep working afterwards. */
  unsigned seed = 12345;
  #define RND() (seed = seed * 1103515245u + 12345u, seed >> 8)
  static const unsigned cmds[] = {SNDCTL_DSP_RESET, SNDCTL_DSP_SYNC, SNDCTL_DSP_SPEED, SNDCTL_DSP_STEREO, SNDCTL_DSP_GETBLKSIZE,
    SNDCTL_DSP_SETFMT, SNDCTL_DSP_CHANNELS, SNDCTL_DSP_SETFRAGMENT, SNDCTL_DSP_GETFMTS, SNDCTL_DSP_GETOSPACE, SNDCTL_DSP_GETODELAY,
    SNDCTL_DSP_SETPLAYVOL, SNDCTL_DSP_GETERROR, 0x5401, 0xdeadbeef};
  fcntl(d, F_SETFL, O_NONBLOCK);
  static int scratch[64];
  for (int i = 0; i < 3000; i++) {
    unsigned r = RND();
    long arg = r % 4 == 0 ? 0 : r % 4 == 1 ? (long)scratch : r % 4 == 2 ? (long)(RND() * 4093u) : -1L;
    if (r & 0x100) { scratch[0] = (int)RND(); ioctl(d, cmds[RND() % (sizeof cmds / sizeof cmds[0])], arg); }
    else write(d, (char *)buf + RND() % 1000, RND() % 70000);
  }
  fcntl(d, F_SETFL, 0);
  ioctl(d, SNDCTL_DSP_RESET, 0);
  CHECK("after 3000 random calls, the device still answers and plays", ioc(d, SNDCTL_DSP_SPEED, 0) == 48000 &&
        write(d, buf, 4096) == 4096 && ioctl(d, SNDCTL_DSP_SYNC, 0) == 0 && ioc(d, SNDCTL_DSP_GETODELAY, 0) == 0);

  int more[8], opened = 0;
  for (int i = 0; i < 8; i++) { more[i] = open("/dev/dsp", O_WRONLY); if (more[i] >= 0) opened++; }
  CHECK("8 streams at once, the 9th is refused", opened == 7);
  for (int i = 0; i < 8; i++) if (more[i] >= 0) close(more[i]);
  close(d);

  /* The multimedia path. */
  char *argv[] = {"audiod", NULL};
  pid_t server = spawn("/bin/audiod", argv, NULL, NULL);
  CHECK("audiod starts", server > 0);
  int a = client(24000, 16, 100, 24000 * 2), b = client(48000, 8, 50, 48000);
  static short a_buf[24000 / 2];
  static unsigned char b_buf[48000 / 2];
  for (int i = 0; i < 12000; i++) a_buf[i] = 1000;
  memset(b_buf, 128 + 8, sizeof b_buf);           /* (136 - 128) << 8 = 2048 */
  t0 = now_ms();
  int ok = 1;
  for (int k = 0; k < 2 && ok; k++) {             /* 1 s from each, in two halves */
    ok = write(a, a_buf, sizeof a_buf) == (long)sizeof a_buf && write(b, b_buf, sizeof b_buf) == (long)sizeof b_buf;
  }
  CHECK("two clients send 1 s each", ok);
  char x;
  CHECK("audiod answers each client once all its sound is mixed", read(a, &x, 1) == 1 && read(b, &x, 1) == 1 && read(a, &x, 1) == 0);
  ms = now_ms() - t0;
  CHECK("...in about real time (0.5 to 1.5 s)", ms > 500 && ms < 1500);
  printf("        (%.0f ms)\n", ms);
  close(a); close(b);
  usleep(500000);                                 /* audiod's buffer plays out */
  struct procinfo pi;
  unsigned long long cpu = 0;
  for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == server) cpu = pi.cpu_us;
  CHECK("audiod used little CPU for 1 s of two streams (under 100 ms)", cpu < 100000);
  printf("        (audiod: %.1f ms of CPU)\n", cpu / 1000.0);
  int sfd = unix_socket();
  unix_connect(sfd);
  write(sfd, "nonsense, not a header", 20);
  CHECK("a bad header is refused (connection closed)", read(sfd, &x, 1) == 0);
  close(sfd);

  /* The music synthesizer (oplplay): one instrument that is a pure sine (the modulator
   * never sounds), an A (MIDI note 69, 440 Hz) for 0.5 s, from a MIDI file and from a
   * MUS file. Pitch, centring, release, speed, and the control pipe. */
  int st = -1;
  static unsigned char gm[8 + 175 * 36];
  memcpy(gm, "#OPL_II#", 8);
  unsigned char *in0 = gm + 8;
  in0[2] = 128;                                   /* fine tuning: none */
  unsigned char *v0 = in0 + 4;                    /* voice 0: modulator (6), feedback, carrier (6), unused, base note (2) */
  v0[0] = 0x01; v0[1] = 0x00; v0[2] = 0x00; v0[5] = 63;            /* modulator: attack rate 0, so it stays silent */
  v0[6] = 0x00;                                   /* FM, no feedback */
  v0[7] = 0x21; v0[8] = 0xf0; v0[9] = 0x0f; v0[12] = 0;            /* carrier: sustained, instant attack, fast release, loudest */
  int gfd = open("/tmp/gm.op2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  write(gfd, gm, sizeof gm); close(gfd);
  static const unsigned char mid[] = {'M','T','h','d',0,0,0,6, 0,0, 0,1, 0,96, 'M','T','r','k',0,0,0,13,
    0, 0x90, 69, 127,  0x60, 0x80, 69, 0,  0x60, 0xff, 0x2f, 0};     /* on, 0.5 s, off, 0.5 s, end */
  static const unsigned char mus[] = {'M','U','S',0x1a, 7,0, 16,0, 1,0, 0,0, 0,0, 0,0,
    0x90, 0x80 | 69, 127, 70,  0x80, 69, 70,  0x60};               /* play (vol 127), 70 ticks, release, 70 ticks, end */
  int mfd = open("/tmp/a.mid", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(mfd, mid, sizeof mid); close(mfd);
  mfd = open("/tmp/a.mus", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(mfd, mus, sizeof mus); close(mfd);
  static short pcm[2 * 48000 * 12 / 10];
  for (int k = 0; k < 2; k++) {
    char *op[] = {"oplplay", "-g", "/tmp/gm.op2", "-o", "/tmp/a.raw", "-s", "1.2", k ? "/tmp/a.mus" : "/tmp/a.mid", NULL};
    pid_t o = spawn("/bin/oplplay", op, NULL, NULL);
    waitpid(o, &st, 0);
    int rf = open("/tmp/a.raw", O_RDONLY);
    long got = rf >= 0 ? read(rf, pcm, sizeof pcm) : -1;
    if (rf >= 0) close(rf);
    int frames = (int)(got / 4), crossings = 0, centred = 1;
    for (int i = 4800; i < 21600 && i < frames; i++) {           /* 0.1 to 0.45 s */
      if (pcm[2 * (i - 1)] < 0 && pcm[2 * i] >= 0) crossings++;
      if (pcm[2 * i] != pcm[2 * i + 1]) centred = 0;
    }
    double hz = crossings / 0.35;
    long long tail = 0, body = 0;
    for (int i = 48000; i < frames; i++) tail += (long long)pcm[2 * i] * pcm[2 * i];  /* 1.0 to 1.2 s */
    for (int i = 4800; i < 21600 && i < frames; i++) body += (long long)pcm[2 * i] * pcm[2 * i];
    char name[96];
    snprintf(name, sizeof name, "oplplay (%s): renders 1.2 s", k ? "MUS" : "MIDI");
    CHECK(name, WIFEXITED(st) && WEXITSTATUS(st) == 0 && frames == 57600);
    snprintf(name, sizeof name, "oplplay (%s): note 69 sounds at 440 Hz (+-1%%)", k ? "MUS" : "MIDI");
    CHECK(name, hz > 435.6 && hz < 444.4);
    printf("        (%.1f Hz)\n", hz);
    snprintf(name, sizeof name, "oplplay (%s): centred, and silent after the release", k ? "MUS" : "MIDI");
    CHECK(name, centred && body > 0 && tail * 1000 < body);
  }
  /* Speed: 10 s of a 9-voice chord, the synthesizer's busiest case. */
  static unsigned char chord[14 + 8 + 9 * 4 + 9 * 4 + 4];
  memcpy(chord, "MThd\0\0\0\6\0\0\0\1\0\140MTrk", 18);
  int cl = 22;
  for (int i = 0; i < 9; i++) { chord[cl++] = 0; chord[cl++] = 0x90; chord[cl++] = (unsigned char)(48 + i * 4); chord[cl++] = 100; }
  chord[cl++] = 0x8f; chord[cl++] = 0x00;                       /* 1920 ticks later: 10 s at 96 per quarter note, 120 bpm */
  chord[cl++] = 0x80; chord[cl++] = 48; chord[cl++] = 0;
  chord[cl++] = 0; chord[cl++] = 0xff; chord[cl++] = 0x2f; chord[cl++] = 0;
  int tl = cl - 22;
  chord[18] = 0; chord[19] = 0; chord[20] = (unsigned char)(tl >> 8); chord[21] = (unsigned char)tl;
  mfd = open("/tmp/chord.mid", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(mfd, chord, (size_t)cl); close(mfd);
  char *ch9[] = {"oplplay", "-g", "/tmp/gm.op2", "-o", "/tmp/chord.raw", "-s", "10", "/tmp/chord.mid", NULL};
  t0 = now_ms();
  pid_t o9 = spawn("/bin/oplplay", ch9, NULL, NULL);
  waitpid(o9, &st, 0);
  ms = now_ms() - t0;
  CHECK("oplplay renders 10 s of 9 voices in under 0.5 s (5% of real time)", WIFEXITED(st) && WEXITSTATUS(st) == 0 && ms < 500);
  printf("        (%.0f ms for 10 s of 9 voices)\n", ms);
  /* Control: started with -c on a pipe, "stop" ends it at once. */
  int cp[2];
  pipe(cp);
  char *ctl[] = {"oplplay", "-c", "-g", "/tmp/gm.op2", "-l", "/tmp/a.mid", NULL};
  int fm[3] = {cp[0], -1, -1};
  pid_t oc = spawn("/bin/oplplay", ctl, NULL, fm);
  close(cp[0]);
  usleep(300000);
  write(cp[1], "volume 50\npause\nresume\nstop\n", 28);
  t0 = now_ms();
  waitpid(oc, &st, 0);
  close(cp[1]);
  CHECK("oplplay -c: pause, resume, volume, then stop ends it within 0.5 s", WIFEXITED(st) && WEXITSTATUS(st) == 0 && now_ms() - t0 < 500);

  char *pl[] = {"play", "-t", "440", "-s", "0.2", NULL};
  pid_t pp = spawn("/bin/play", pl, NULL, NULL);
  st = -1;
  waitpid(pp, &st, 0);
  CHECK("play -t 440 -s 0.2 runs and ends", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  usleep(400000);
  kill(server, SIGTERM);
  waitpid(server, &st, 0);
  printf("audiotest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
