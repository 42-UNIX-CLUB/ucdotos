// SPDX-License-Identifier: GPL-2.0-or-later
/* wproto: test program for the worker-process prototype (docs/WORKER-PROCESSES.md).
 * The same source is built several ways by tools/worker-proto/proto.mk:
 *   /bin/wproto     ordinary build, runs on the main thread (the orchestrator)
 *   /bin/wproto-b   ordinary build plus the `ucos_worker` export: approach B
 *   /bin/wproto-a   shared-memory build: approach A
 *   /bin/wproto-y   yield points inserted at every loop: approach C
 * Modes:
 *   basic                 the system calls the brief asks for, with checks
 *   bench                 ns per call: getppid, write 64 B, read 64 B, pipe, clock
 *   spin SECONDS          a pure CPU loop: no system call until it ends
 *   forever               a pure CPU loop that never ends
 *   true                  exit at once (for start-up timing)
 *   tick N MS [fb]        sleep MS milliseconds N times; report how late the wake-ups were;
 *                         with fb, also paint a stripe on /dev/fb0 each time
 *   startup PROG N        spawn PROG true and wait, N times; report the time per start
 *   killtest PROG MS      start PROG forever, wait MS, kill it, report how long until it is reaped */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static int fails;
#define CHECK(what, cond) do { int ok_ = !!(cond); if (!ok_) fails++; printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what); } while (0)

/* ---- basic ---- */
static int basic(char *self) {
  printf("wproto basic: pid %ld, parent %ld\n", (long)getpid(), (long)getppid());
  CHECK("getppid > 0", getppid() > 0);
  const char msg[] = "  write to stdout works\n";
  CHECK("write", write(1, msg, sizeof msg - 1) == (long)sizeof msg - 1);
  struct stat st;
  CHECK("stat /bin/sh", stat("/bin/sh", &st) == 0 && st.st_size > 1000);
  int fd = open("/etc/motd", O_RDONLY);
  char buf[256];
  long n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
  CHECK("open + read /etc/motd", n > 0);
  if (fd >= 0) close(fd);
  int p[2];
  CHECK("pipe", pipe2(p, O_CLOEXEC) == 0);
  char *argv[] = {"echo", "hello", "through", "a", "pipe", 0};
  int fdmap[3] = {-1, p[1], -1};
  int pid = spawn("/bin/echo", argv, 0, fdmap);
  CHECK("spawn /bin/echo from this process", pid > 0);
  close(p[1]);
  n = 0;
  for (long r; (r = read(p[0], buf + n, sizeof buf - 1 - n)) > 0;) n += r;
  buf[n > 0 ? n : 0] = 0;
  CHECK("read the child's output from the pipe", strcmp(buf, "hello through a pipe\n") == 0);
  close(p[0]);
  int status = -1;
  CHECK("waitpid", waitpid(pid, &status, 0) == pid && status == 0);
  double t0 = now_ms();
  struct timespec ts = {0, 50 * 1000 * 1000};
  nanosleep(&ts, 0);
  double slept = now_ms() - t0;
  printf("  nanosleep 50 ms took %.1f ms\n", slept);
  CHECK("nanosleep 50 ms", slept >= 49 && slept < 500);
  /* memory that grew after start: the kernel must reach it (A: the shared memory grew under
   * the main thread's shim; B: the proxy memory must grow to match) */
  char *big = malloc(32 << 20);
  int zf = open("/dev/zero", O_RDONLY), nf = open("/dev/null", O_WRONLY);
  if (big) memset(big + (32 << 20) - 64, 'x', 64);
  CHECK("read into memory grown by 32 MiB", big && read(zf, big + (32 << 20) - 64, 64) == 64 && big[(32 << 20) - 1] == 0);
  memset(big + (32 << 20) - 64, 'y', 64);
  CHECK("write from memory grown by 32 MiB", big && write(nf, big + (32 << 20) - 64, 64) == 64);
  close(zf); close(nf);
  free(big);
  if (self) { /* basic -: also read one chunk from stdin */
    char line[128] = {0};
    n = read(0, line, sizeof line - 1);
    printf("  read from stdin: %ld bytes: %s", n, line);
    CHECK("read from stdin", n > 0);
  }
  printf("wproto basic: %d failed\n", fails);
  return fails ? 1 : 7; /* 7: the caller checks the exit code travels back */
}

/* ---- bench ---- */
static int zfd, nfd, pfd[2];
static char buf64[64];
__attribute__((noinline)) static long b_getppid(int n) { long s = 0; for (int i = 0; i < n; i++) s += __syscall0(SYS_getppid); return s; }
__attribute__((noinline)) static long b_write(int n) { long s = 0; for (int i = 0; i < n; i++) s += __syscall3(SYS_write, nfd, buf64, 64); return s; }
__attribute__((noinline)) static long b_read(int n) { long s = 0; for (int i = 0; i < n; i++) s += __syscall3(SYS_read, zfd, buf64, 64); return s; }
__attribute__((noinline)) static long b_pipe(int n) { long s = 0; for (int i = 0; i < n; i++) { s += __syscall3(SYS_write, pfd[1], buf64, 64); s += __syscall3(SYS_read, pfd[0], buf64, 64); } return s; }
static void bench1(const char *name, long (*f)(int)) {
  int n = 16;
  double dt;
  for (;; n *= 2) { double t0 = now_ms(); f(n); dt = now_ms() - t0; if (dt > 20 || n > (1 << 24)) break; }
  double first = dt * 1e6 / n, best = first;
  for (int r = 0; r < 15; r++) { double t0 = now_ms(); f(n); double d = (now_ms() - t0) * 1e6 / n; if (d < best) best = d; }
  printf("  %-24s first %10.1f ns   best %10.1f ns   (batch %d)\n", name, first, best, n);
}
static int bench(void) {
  zfd = open("/dev/zero", O_RDONLY);
  nfd = open("/dev/null", O_WRONLY);
  pipe2(pfd, 0);
  printf("wproto bench (pid %ld): best of 16 batches of at least 20 ms\n", (long)getpid());
  bench1("getppid", b_getppid);
  bench1("write 64 B /dev/null", b_write);
  bench1("read 64 B /dev/zero", b_read);
  bench1("pipe write+read 64 B", b_pipe);
  return 0;
}

/* ---- pure CPU ---- */
volatile unsigned spin_progress; /* approach A: the main thread can watch this in the shared memory */
__attribute__((noinline)) static unsigned crunch(unsigned x, long iters) {
  for (long i = 0; i < iters; i++) x = x * 1664525u + 1013904223u + (x >> 13);
  return x;
}
static int spin(double seconds) {
  double t0 = now_ms();
  long per = 1 << 20, calls = 0;
  unsigned x = 1;
  while (now_ms() - t0 < 100) { x = crunch(x, per); calls++; } /* calibrate with the clock */
  double rate = calls * (double)per / (now_ms() - t0);           /* iterations per ms */
  long chunks = (long)(seconds * 1000 * rate / per);
  printf("wproto spin: %.1f s of pure CPU, no system calls (%.0f M iterations/s)\n", seconds, rate / 1000);
  t0 = now_ms();
  for (long c = 0; c < chunks; c++) { x = crunch(x, per); spin_progress = (unsigned)c; } /* no system call in here */
  printf("wproto spin: done in %.0f ms (%u)\n", now_ms() - t0, x & 1);
  return 0;
}
static int forever(void) {
  unsigned x = 1;
  printf("wproto forever: pid %ld spinning\n", (long)getpid());
  for (unsigned c = 0;; c++) { x = crunch(x, 1 << 16); spin_progress = c + (x & 1); }
}

/* ---- main-thread companions ---- */
static int tick(int n, int ms, int fb) {
  int fd = fb ? open("/dev/fb0", O_RDWR) : -1;
  static unsigned row[1024 * 16];
  double worst = 0, sum = 0;
  for (int i = 0; i < n; i++) {
    double t0 = now_ms();
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, 0);
    double late = now_ms() - t0 - ms;
    if (late > worst) worst = late;
    sum += late;
    if (fd >= 0) {
      for (int k = 0; k < 1024 * 16; k++) row[k] = 0xff000000u | (unsigned)(i * 0x0a0503);
      __syscall4(SYS_pwrite64, fd, row, sizeof row, 0);
    }
  }
  printf("wproto tick: %d sleeps of %d ms, late by %.2f ms on average, %.2f ms at worst\n", n, ms, sum / n, worst);
  return 0;
}
static int startup(char *prog, int n) {
  char *argv[] = {prog, "true", 0};
  double best = 1e18, first = 0, total = 0;
  for (int i = 0; i < n; i++) {
    double t0 = now_ms();
    int st, pid = spawn(prog, argv, 0, 0);
    if (pid < 0 || waitpid(pid, &st, 0) != pid) { printf("wproto startup: spawn failed\n"); return 1; }
    double dt = now_ms() - t0;
    if (i == 0) first = dt;
    if (dt < best) best = dt;
    total += dt;
  }
  printf("wproto startup %s: spawn + wait, first %.2f ms, best %.2f ms, mean %.2f ms over %d\n", prog, first, best, total / n, n);
  return 0;
}
static int killtest(char *prog, int ms) {
  char *argv[] = {prog, "forever", 0};
  int pid = spawn(prog, argv, 0, 0);
  if (pid < 0) { printf("wproto killtest: spawn failed\n"); return 1; }
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
  double t0 = now_ms();
  int r = kill(pid, SIGKILL);
  int st = 0, w = waitpid(pid, &st, 0);
  double dt = now_ms() - t0;
  printf("wproto killtest %s: kill -> %d, reaped %d with status %d after %.3f ms\n", prog, r, w, st, dt);
  return !(r == 0 && w == pid && st == SIGKILL);
}

/* time N INFILE PROG ARGS...: run PROG N times, stdin from INFILE ("-": none), stdout to /dev/null */
static int timeit(int n, char *in, char **argv) {
  double best = 1e18, sum = 0;
  int nul = open("/dev/null", O_WRONLY);
  for (int i = 0; i < n; i++) {
    int fd = strcmp(in, "-") ? open(in, O_RDONLY) : -1;
    int map[3] = {fd, nul, -1};
    double t0 = now_ms();
    int st, pid = spawn(argv[0], argv, 0, map);
    if (fd >= 0) close(fd);
    if (pid < 0 || waitpid(pid, &st, 0) != pid || st != 0) { printf("wproto time: %s failed (status %d)\n", argv[0], st); return 1; }
    double dt = now_ms() - t0;
    if (dt < best) best = dt;
    sum += dt;
  }
  printf("wproto time %-12s best %8.1f ms  mean %8.1f ms  (%d runs)\n", argv[0], best, sum / n, n);
  return 0;
}
/* rate: iterations per second of the tight loop, best of 10 windows of 100 ms */
static int rate(void) {
  double best = 0;
  unsigned x = 1;
  for (int w = 0; w < 10; w++) {
    double t0 = now_ms();
    long calls = 0;
    while (now_ms() - t0 < 100) { x = crunch(x, 1 << 18); calls++; }
    double r = calls * (double)(1 << 18) / (now_ms() - t0) / 1e3;
    if (r > best) best = r;
  }
  printf("wproto rate: %.1f M iterations/s (best of 10 windows) %u\n", best, x & 1);
  return 0;
}

int main(int argc, char **argv) {
  const char *m = argc > 1 ? argv[1] : "basic";
  if (!strcmp(m, "basic")) return basic(argc > 2 && !strcmp(argv[2], "-") ? argv[0] : 0);
  if (!strcmp(m, "bench")) return bench();
  if (!strcmp(m, "spin")) return spin(argc > 2 ? atof(argv[2]) : 3);
  if (!strcmp(m, "forever")) return forever();
  if (!strcmp(m, "true")) return 0;
  if (!strcmp(m, "rate")) return rate();
  if (!strcmp(m, "time") && argc > 4) return timeit(atoi(argv[2]), argv[3], argv + 4);
  if (!strcmp(m, "tick")) return tick(argc > 2 ? atoi(argv[2]) : 100, argc > 3 ? atoi(argv[3]) : 10, argc > 4);
  if (!strcmp(m, "startup") && argc > 2) return startup(argv[2], argc > 3 ? atoi(argv[3]) : 20);
  if (!strcmp(m, "killtest") && argc > 2) return killtest(argv[2], argc > 3 ? atoi(argv[3]) : 200);
  fprintf(stderr, "usage: wproto basic|bench|spin S|forever|true|tick N MS [fb]|startup PROG N|killtest PROG MS\n");
  return 2;
}
