// SPDX-License-Identifier: GPL-2.0-or-later
/* sysbench: time system calls and print nanoseconds per call.
 * Each row runs about 150 ms (iteration count chosen adaptively); the best of 5 runs is shown.
 * Usage: sysbench [--json] [--color|--no-color]
 * Builds for the OS and natively on Linux (cc -O2 bin/sysbench.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __wasm__
#include <sys/syscall.h>
static int is_tty(int fd) { struct termios t; return __syscall3(SYS_ioctl, fd, TCGETS, (long)&t) == 0; }
static void os_name(char *buf, int cap) {
  struct utsname u;
  if (__syscall1(SYS_uname, (long)&u) == 0) snprintf(buf, cap, "%s %s %s", u.sysname, u.release, u.machine);
  else snprintf(buf, cap, "unknown");
}
static int run_true(void) {
  static char *argv[] = {"true", 0};
  int st, pid = spawn("/bin/true", argv, 0, 0);
  return pid > 0 && waitpid(pid, &st, 0) == pid ? 0 : -1;
}
#else
#include <termios.h>
#include <spawn.h>
#include <sys/utsname.h>
extern char **environ;
static int is_tty(int fd) { struct termios t; return tcgetattr(fd, &t) == 0; }
static void os_name(char *buf, int cap) {
  struct utsname u;
  if (uname(&u) == 0) snprintf(buf, cap, "%s %s %s", u.sysname, u.release, u.machine);
  else snprintf(buf, cap, "unknown");
}
static int run_true(void) {
  static char *argv[] = {"true", 0};
  int st; pid_t pid;
  if (posix_spawn(&pid, "/bin/true", 0, 0, argv, environ) != 0) return -1;
  return waitpid(pid, &st, 0) == pid ? 0 : -1;
}
#endif

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* --- the benchmarks: each runs a batch of n iterations and returns 0, or -1 on failure.
 * They are called many times, never inlined: a wasm engine swaps in optimised code between
 * calls, not in the middle of a running loop, so the first batch shows the cold cost. --- */
#define BATCH __attribute__((noinline))
static int fd_zero, fd_null, pfd[2];
static char buf[64];
static volatile long sink;
static const char *tmpfile_path = "/tmp/sysbench.tmp";

BATCH static int b_getppid(long n) { for (long i = 0; i < n; i++) sink += getppid(); return 0; }
BATCH static int b_read(long n) { for (long i = 0; i < n; i++) if (read(fd_zero, buf, 64) != 64) return -1; return 0; }
BATCH static int b_write(long n) { for (long i = 0; i < n; i++) if (write(fd_null, buf, 64) != 64) return -1; return 0; }
BATCH static int b_pipe(long n) {
  for (long i = 0; i < n; i++) if (write(pfd[1], buf, 64) != 64 || read(pfd[0], buf, 64) != 64) return -1;
  return 0;
}
BATCH static int b_open(long n) {
  for (long i = 0; i < n; i++) { int fd = open(tmpfile_path, O_RDONLY); if (fd < 0) return -1; close(fd); }
  return 0;
}
BATCH static int b_stat(long n) { struct stat st; for (long i = 0; i < n; i++) if (stat(tmpfile_path, &st)) return -1; return 0; }
BATCH static int b_clock(long n) { struct timespec ts; for (long i = 0; i < n; i++) clock_gettime(CLOCK_MONOTONIC, &ts); sink += ts.tv_nsec; return 0; }
BATCH static int b_spawn(long n) { for (long i = 0; i < n; i++) if (run_true()) return -1; return 0; }

struct bench {
  const char *name, *label, *unit; /* unit "ns" or "us" */
  int (*fn)(long);
  double ref_linux, ref_qemu, ref_unk; /* published figures, same unit; 0 = none */
  const char *ref_what;
  double result, first, avg; long batch; int failed; /* best batch, first (cold) batch, best run average */
};
static struct bench benches[] = {
  {"getppid", "getppid", "ns", b_getppid, 110, 1662, 172, "getppid", 0, 0, 0, 0, 0},
  {"read", "read 64 B /dev/zero", "ns", b_read, 161, 2247, 172, "read", 0, 0, 0, 0, 0},
  {"write", "write 64 B /dev/null", "ns", b_write, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"pipe", "pipe write+read 64 B", "ns", b_pipe, 417, 7256, 482, "pipe", 0, 0, 0, 0, 0},
  {"open_close", "open + close", "ns", b_open, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"stat", "stat", "ns", b_stat, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"clock_gettime", "clock_gettime", "ns", b_clock, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"spawn_wait", "spawn + wait /bin/true", "us", b_spawn, 73, 1178, 57, "fork+exit+wait", 0, 0, 0, 0, 0},
};
#define NB (int)(sizeof benches / sizeof benches[0])

static void run_bench(struct bench *b) {
  double scale = !strcmp(b->unit, "us") ? 1000.0 : 1.0;
  long n = !strcmp(b->unit, "us") ? 1 : 200;
  double t0 = now_ns();
  if (b->fn(n)) { b->failed = 1; return; }
  double t = now_ns() - t0;
  b->first = t / n / scale;
  while (t < 1e6 && n < (1L << 26)) { /* batch size: about 1 ms per batch */
    n *= 2;
    t0 = now_ns();
    if (b->fn(n)) { b->failed = 1; return; }
    t = now_ns() - t0;
  }
  double best = 1e300, best_avg = 1e300;
  for (int r = 0; r < 5; r++) { /* 5 runs of ~150 ms, each made of batches */
    double total = 0;
    long calls = 0;
    while (total < 150e6) {
      t0 = now_ns();
      if (b->fn(n)) { b->failed = 1; return; }
      t = now_ns() - t0;
      total += t; calls += n;
      if (t / n < best) best = t / n;
    }
    if (total / calls < best_avg) best_avg = total / calls;
  }
  b->batch = n;
  b->result = best / scale;
  b->avg = best_avg / scale;
}

/* --- colours: THEME=deck (gruvbox, default) or THEME=brand --- */
struct theme { const char *fg, *accent, *good, *warn, *dim, *head, *key; };
static const struct theme deck = {"235;219;178", "254;128;25", "184;187;38", "251;73;52", "146;131;116", "250;189;47", "142;192;124"};
static const struct theme brand = {"237;230;197", "254;119;0", "173;199;135", "252;65;58", "122;150;132", "255;199;46", "173;199;135"};
static int color;
static const struct theme *th = &deck;
static void fgc(const char *rgb) { if (color) printf("\033[38;2;%sm", rgb); }
static void bold(void) { if (color) printf("\033[1m"); }
static void reset(void) { if (color) printf("\033[0m"); }

static const char *num(double v, char *out) {
  if (v < 10) sprintf(out, "%.2f", v);
  else if (v < 100) sprintf(out, "%.1f", v);
  else sprintf(out, "%.0f", v);
  return out;
}
static void cell(double v, const char *unit, int width) { /* right-aligned "123 ns" */
  char a[32], s[48];
  if (v <= 0) snprintf(s, sizeof s, "-");
  else if (v == (double)(long)v) snprintf(s, sizeof s, "%ld %s", (long)v, unit);
  else snprintf(s, sizeof s, "%s %s", num(v, a), unit);
  printf("%*s", width, s);
}
static void rule(int n) {
  fgc(th->dim);
  for (int i = 0; i < n; i++) fputs(color ? "\xe2\x94\x80" : "-", stdout);
  reset();
  putchar('\n');
}

static void print_table(const char *os) {
  bold(); fgc(th->accent); printf("  sysbench"); reset();
  fgc(th->dim); printf("  system calls on %s: best ~1 ms batch over 5 runs of ~150 ms; first = cold first batch\n", os); reset();
  rule(98);
  bold(); fgc(th->head);
  printf("  %-24s%12s%12s %8s  ", "call", "best batch", "first", "vs linux");
  fgc(th->dim);
  printf("%s %10s %10s %10s\n", color ? "\xe2\x94\x82" : "|", "linux", "qemu", "unkernel");
  reset();
  for (int i = 0; i < NB; i++) {
    struct bench *b = &benches[i];
    printf("  "); fgc(th->fg); printf("%-24s", b->label); reset();
    if (b->failed) { fgc(th->warn); printf("%12s%12s", "failed", ""); reset(); }
    else { bold(); fgc(th->accent); cell(b->result, b->unit, 12); reset(); fgc(th->dim); cell(b->first, b->unit, 12); reset(); }
    if (b->ref_linux > 0 && !b->failed && b->result > 0) {
      double x = b->ref_linux / b->result;
      char a[32], s[48];
      snprintf(s, sizeof s, "%sx", num(x, a));
      fgc(x >= 1 ? th->good : th->warn); printf(" %8s  ", s); reset();
    } else printf(" %8s  ", "");
    fgc(th->dim);
    printf("%s", color ? "\xe2\x94\x82" : "|");
    cell(b->ref_linux, b->unit, 10); printf(" "); cell(b->ref_qemu, b->unit, 10); printf(" "); cell(b->ref_unk, b->unit, 10);
    reset();
    putchar('\n');
  }
  rule(98);
  fgc(th->dim);
  printf("  right of the bar: figures PUBLISHED by other projects, not measured here\n");
  printf("  (native Linux, Linux in qemu-system, unkernel). Their spawn row is fork+exit+wait.\n");
  reset();
}

static void print_json(const char *os) {
  printf("{\n  \"system\": \"%s\",\n  \"method\": \"value: best ~1 ms batch over 5 runs of ~150 ms; first_batch: the cold first batch; best_run_average: best of the 5 run averages; CLOCK_MONOTONIC\",\n  \"results\": [\n", os);
  for (int i = 0; i < NB; i++) {
    struct bench *b = &benches[i];
    printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"value\": ", b->name, b->unit);
    if (b->failed) printf("null"); else printf("%.3f, \"first_batch\": %.3f, \"best_run_average\": %.3f", b->result, b->first, b->avg);
    printf(", \"batch\": %ld}%s\n", b->batch, i + 1 < NB ? "," : "");
  }
  printf("  ],\n  \"reference_published_not_measured\": {\n");
  printf("    \"linux\": {\"getppid_ns\": 110, \"read_ns\": 161, \"pipe_ns\": 417, \"fork_exit_wait_us\": 73},\n");
  printf("    \"qemu_system\": {\"getppid_ns\": 1662, \"read_ns\": 2247, \"pipe_ns\": 7256, \"fork_exit_wait_us\": 1178},\n");
  printf("    \"unkernel\": {\"getppid_ns\": 172, \"read_ns\": 172, \"pipe_ns\": 482, \"fork_exit_wait_us\": 57}\n  }\n}\n");
}

int main(int argc, char **argv) {
  int json = 0, force = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = 1;
    else if (!strcmp(argv[i], "--color")) force = 1;
    else if (!strcmp(argv[i], "--no-color")) force = 0;
    else { fprintf(stderr, "usage: sysbench [--json] [--color|--no-color]\n"); return 2; }
  }
  color = !json && (force >= 0 ? force : is_tty(1));
  const char *t = getenv("THEME");
  if (t && !strcmp(t, "brand")) th = &brand;
  char os[200];
  os_name(os, sizeof os);

  fd_zero = open("/dev/zero", O_RDONLY);
  fd_null = open("/dev/null", O_WRONLY);
  if (fd_zero < 0 || fd_null < 0 || pipe(pfd)) { fprintf(stderr, "sysbench: cannot open /dev/zero, /dev/null or a pipe\n"); return 1; }
  int fd = open(tmpfile_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { write(fd, "sysbench\n", 9); close(fd); }

  if (!json) { fgc(th->dim); printf("  sysbench: measuring"); reset(); fflush(stdout); }
  for (int i = 0; i < NB; i++) {
    run_bench(&benches[i]);
    if (!json) { fgc(th->dim); putchar('.'); reset(); fflush(stdout); }
  }
  if (!json) { if (color) printf("\r\033[K"); else putchar('\n'); }
  unlink(tmpfile_path);
  if (json) print_json(os); else print_table(os);
  return 0;
}
