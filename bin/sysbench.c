// SPDX-License-Identifier: GPL-2.0-or-later
/* sysbench: time system calls and print nanoseconds per call.
 * Calls run in batches inside noinline functions called many times (so the engine optimises
 * them). A batch lasts at least 1 ms and at least 200 times the clock's resolution, which
 * browsers coarsen (0.1 ms in Chrome, 1 ms in Firefox); the best batch is shown, and the cold
 * first batch next to it. The table fits the terminal width by dropping columns.
 * Usage: sysbench [--json] [--color|--no-color]   (hidden: --clock-res US simulates a coarse clock)
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
static int term_cols(int fd) { struct winsize w; return __syscall3(SYS_ioctl, fd, TIOCGWINSZ, (long)&w) == 0 && w.ws_col ? w.ws_col : 0; }
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
#include <sys/ioctl.h>
static int is_tty(int fd) { struct termios t; return tcgetattr(fd, &t) == 0; }
static int term_cols(int fd) { struct winsize w; return ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col ? w.ws_col : 0; }
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

static double sim_res_ns; /* --clock-res: round readings down to this many ns */
/* Rows are timed on the process CPU clock, so time spent parked by the scheduler (the
 * kernel preempts every 8 ms, and a browser has other work between turns) is not
 * counted. spawn + wait is timed on the wall clock: the work happens in the child. */
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif
static int clk_id = CLOCK_MONOTONIC;
static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(clk_id, &ts) < 0) clock_gettime(CLOCK_MONOTONIC, &ts);
  double t = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
  if (sim_res_ns > 0) t = (double)(long long)(t / sim_res_ns) * sim_res_ns;
  return t;
}
/* smallest non-zero step between consecutive readings, sampled for at least 5 ms */
static double res_ns;
static double clock_resolution(void) {
  double start = now_ns(), prev = start, best = 1e18;
  for (long spins = 0; spins < 200000000L; spins++) {
    double t = now_ns();
    if (t != prev) { if (t - prev < best) best = t - prev; prev = t; }
    if (t - start >= 5e6 && best < 1e18) break;
  }
  return best;
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
  double result, first, avg; long batch; int batches, failed; /* best batch, first (cold) batch, average */
};
static struct bench benches[] = {
  {"getppid", "getppid", "ns", b_getppid, 110, 1662, 172, "getppid", 0, 0, 0, 0, 0, 0},
  {"read", "read 64 B /dev/zero", "ns", b_read, 161, 2247, 172, "read", 0, 0, 0, 0, 0, 0},
  {"write", "write 64 B /dev/null", "ns", b_write, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"pipe", "pipe write+read 64 B", "ns", b_pipe, 417, 7256, 482, "pipe", 0, 0, 0, 0, 0, 0},
  {"open_close", "open + close", "ns", b_open, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"stat", "stat", "ns", b_stat, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"clock_gettime", "clock_gettime", "ns", b_clock, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  {"spawn_wait", "spawn + wait /bin/true", "us", b_spawn, 73, 1178, 57, "fork+exit+wait", 0, 0, 0, 0, 0, 0},
};
#define NB (int)(sizeof benches / sizeof benches[0])

static void run_bench(struct bench *b) {
  double scale = !strcmp(b->unit, "us") ? 1000.0 : 1.0;
  clk_id = !strcmp(b->unit, "us") ? CLOCK_MONOTONIC : CLOCK_PROCESS_CPUTIME_ID;
  double min_batch = 200 * res_ns > 1e6 ? 200 * res_ns : 1e6; /* ns */
  long n = !strcmp(b->unit, "us") ? 1 : 200;
  double t0 = now_ns();
  if (b->fn(n)) { b->failed = 1; return; }
  double t = now_ns() - t0;
  b->first = t >= 20 * res_ns ? t / n / scale : -1; /* -1: too short to time at this resolution */
  while (t < min_batch && n < (1L << 30)) { /* grow the batch, keeping its function warm */
    n = t > 0 && min_batch / t < 8 ? (long)(n * (min_batch / t) * 1.1) + 1 : n * 4;
    t0 = now_ns();
    if (b->fn(n)) { b->failed = 1; return; }
    t = now_ns() - t0;
  }
  /* measure for ~600 ms and at least 3 batches; with coarse clocks batches are longer, so fewer */
  double best = 1e300, total = 0;
  long calls = 0;
  int batches = 0;
  while (total < 600e6 || batches < 3) {
    t0 = now_ns();
    if (b->fn(n)) { b->failed = 1; return; }
    t = now_ns() - t0;
    total += t; calls += n; batches++;
    if (t > 0 && t / n < best) best = t / n;
  }
  b->batch = n;
  b->batches = batches;
  b->result = best / scale;
  b->avg = total / calls / scale;
}

/* --- colours: THEME=deck (gruvbox, default) or THEME=brand --- */
struct theme { const char *fg, *accent, *good, *warn, *dim, *head, *key; };
static const struct theme deck = {"235;219;178", "254;128;25", "184;187;38", "251;73;52", "146;131;116", "250;189;47", "142;192;124"};
static const struct theme brand = {"237;230;197", "254;119;0", "173;199;135", "252;65;58", "122;150;132", "255;199;46", "173;199;135"};
static int color;
static const struct theme *th = &deck;
static void load_theme(void) { /* first word of ~/.config/ucos/theme, else $THEME */
  char path[300], name[32] = "";
  const char *home = getenv("HOME");
  snprintf(path, sizeof path, "%s/.config/ucos/theme", home && *home ? home : "/home/user");
  FILE *f = fopen(path, "r");
  if (f) { if (fscanf(f, "%31s", name) != 1) name[0] = 0; fclose(f); }
  if (!name[0]) { const char *t = getenv("THEME"); snprintf(name, sizeof name, "%s", t ? t : "deck"); }
  th = !strcmp(name, "brand") ? &brand : &deck;
}
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
/* print text word-wrapped to width w, each line indented by two spaces */
static void wrap(const char *text, int w) {
  const char *p = text;
  while (*p) {
    int room = w - 2, len = 0, last_space = -1;
    while (p[len] && len < room) { if (p[len] == ' ') last_space = len; len++; }
    if (p[len] && last_space > 0) len = last_space;
    printf("  %.*s\n", len, p);
    p += len;
    while (*p == ' ') p++;
  }
}
static void fmt_res(char *o, int cap) {
  if (res_ns >= 1e6) snprintf(o, cap, "%.1f ms", res_ns / 1e6);
  else if (res_ns >= 1e3) snprintf(o, cap, "%.1f us", res_ns / 1e3);
  else snprintf(o, cap, "%.0f ns", res_ns);
}

static void print_table(const char *os, int width) {
  /* columns, dropped in this order as the width shrinks: qemu, unkernel, linux, vs linux, first */
  int show_q = 1, show_u = 1, show_l = 1, show_vs = 1, show_first = 1;
  int w;
  for (;;) {
    w = 2 + 24 + 12 + (show_first ? 12 : 0) + (show_vs ? 10 : 0);
    if (show_l || show_u || show_q) w += 1 + (show_l ? 10 : 0) + (show_u ? 11 : 0) + (show_q ? 11 : 0);
    if (w <= width) break;
    if (show_q) show_q = 0; else if (show_u) show_u = 0; else if (show_l) show_l = 0;
    else if (show_vs) show_vs = 0; else if (show_first) show_first = 0; else break;
  }
  int refs = show_l || show_u || show_q;
  char res[32], head[300];
  fmt_res(res, sizeof res);
  bold(); fgc(th->accent); printf("  sysbench"); reset();
  snprintf(head, sizeof head, "%s, clock resolution %s", os, res);
  if (12 + (int)strlen(head) <= width) { fgc(th->dim); printf("  %s", head); reset(); putchar('\n'); }
  else { putchar('\n'); fgc(th->dim); wrap(head, width); reset(); }
  rule(w < width ? w : width);
  bold(); fgc(th->head);
  printf("  %-24s%12s", "call", "best batch");
  if (show_first) printf("%12s", "first");
  if (show_vs) printf("%10s", "vs linux");
  fgc(th->dim);
  if (refs) {
    printf("  %s", color ? "\xe2\x94\x82" : "|");
    if (show_l) printf("%9s", "linux");
    if (show_u) printf("%11s", "unkernel");
    if (show_q) printf("%11s", "qemu");
  }
  reset(); putchar('\n');
  for (int i = 0; i < NB; i++) {
    struct bench *b = &benches[i];
    printf("  "); fgc(th->fg); printf("%-24s", b->label); reset();
    if (b->failed) { fgc(th->warn); printf("%12s", "failed"); if (show_first) printf("%12s", ""); reset(); }
    else {
      bold(); fgc(th->accent); cell(b->result, b->unit, 12); reset();
      if (show_first) { fgc(th->dim); if (b->first < 0) printf("%12s", "n/a"); else cell(b->first, b->unit, 12); reset(); }
    }
    if (show_vs) {
      if (b->ref_linux > 0 && !b->failed && b->result > 0) {
        double x = b->ref_linux / b->result;
        char a[32], s[48];
        snprintf(s, sizeof s, "%sx", num(x, a));
        fgc(x >= 1 ? th->good : th->warn); printf("%10s", s); reset();
      } else printf("%10s", "");
    }
    if (refs) {
      fgc(th->dim);
      printf("  %s", color ? "\xe2\x94\x82" : "|");
      if (show_l) cell(b->ref_linux, b->unit, 9);
      if (show_u) cell(b->ref_unk, b->unit, 11);
      if (show_q) cell(b->ref_qemu, b->unit, 11);
      reset();
    }
    putchar('\n');
  }
  rule(w < width ? w : width);
  fgc(th->dim);
  char foot[600];
  if (!refs) /* narrow window: keep it short so the table and its header stay on screen */
    snprintf(foot, sizeof foot, "%s", show_first ? "first: the cold first 200 calls (n/a: too short for this clock)." : "best of batches lasting 200+ clock steps.");
  else snprintf(foot, sizeof foot, "best batch: the fastest timed batch, each lasting at least 200 clock steps. %s%s",
           show_first ? "first: the cold first batch of 200 calls (n/a when too short for this clock). " : "",
           refs ? "Right of the bar: figures PUBLISHED by other projects (native Linux, unkernel, Linux in qemu-system), not measured here; their spawn row is fork+exit+wait." : "");
  wrap(foot, width);
  reset();
}

static void print_json(const char *os) {
  printf("{\n  \"system\": \"%s\",\n  \"clock_resolution_ns\": %.1f,\n  \"method\": \"value: best batch (each at least 1 ms and 200 clock steps, ~600 ms per row); first_batch: the cold first 200 calls (null when shorter than 20 clock steps); average: all batches; process CPU clock (wall clock for spawn_wait)\",\n  \"results\": [\n", os, res_ns);
  for (int i = 0; i < NB; i++) {
    struct bench *b = &benches[i];
    printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"value\": ", b->name, b->unit);
    if (b->failed) printf("null");
    else {
      printf("%.3f, \"first_batch\": ", b->result);
      if (b->first < 0) printf("null"); else printf("%.3f", b->first);
      printf(", \"average\": %.3f", b->avg);
    }
    printf(", \"batch\": %ld, \"batches\": %d}%s\n", b->batch, b->batches, i + 1 < NB ? "," : "");
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
    else if (!strcmp(argv[i], "--clock-res") && i + 1 < argc) sim_res_ns = atof(argv[++i]) * 1000.0;
    else { fprintf(stderr, "usage: sysbench [--json] [--color|--no-color]\n"); return 2; }
  }
  color = !json && (force >= 0 ? force : is_tty(1));
  load_theme();
  int width = is_tty(1) ? term_cols(1) : 0;
  if (width <= 0) width = 100;
  char os[200];
  os_name(os, sizeof os);

  fd_zero = open("/dev/zero", O_RDONLY);
  fd_null = open("/dev/null", O_WRONLY);
  if (fd_zero < 0 || fd_null < 0 || pipe(pfd)) { fprintf(stderr, "sysbench: cannot open /dev/zero, /dev/null or a pipe\n"); return 1; }
  int fd = open(tmpfile_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { write(fd, "sysbench\n", 9); close(fd); }

  res_ns = clock_resolution();
  if (!json) { fgc(th->dim); printf("  sysbench: measuring"); reset(); fflush(stdout); }
  for (int i = 0; i < NB; i++) {
    run_bench(&benches[i]);
    if (!json) { fgc(th->dim); putchar('.'); reset(); fflush(stdout); }
  }
  if (!json) { if (color) printf("\r\033[K"); else putchar('\n'); }
  unlink(tmpfile_path);
  if (json) print_json(os); else print_table(os, width);
  return 0;
}
