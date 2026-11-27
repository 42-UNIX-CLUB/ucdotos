// SPDX-License-Identifier: GPL-2.0-or-later
/* clonetest: the clone primitive (SYS_pclone) used directly, the way a binary engine uses
 * it. A tiny interpreter keeps its whole "CPU" in memory (in main's stack frame, reached
 * through a pointer, so the test also shows that the stack pointer is copied) and returns
 * to its dispatch loop for every "system call". The child re-enters the loop at the same
 * program counter with 0 as the result; the parent continues with the child's pid.
 *   clonetest basic | loop N | fill | pipe | bench MiB N | migrate TOTAL_MiB LIVE_MiB
 * migrate: the same, with SYS_pmigrate: the process moves to a fresh memory holding only its
 * live data and resumes at __migrate_resume. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

enum { OP_SET, OP_FORK, OP_JZ, OP_JMP, OP_PRINT, OP_EXIT, OP_WAIT, OP_DEC_JNZ, OP_END };
struct ins { int op, a, b; const char *s; };
struct cpu { int pc; long r[4]; const struct ins *prog; long sum; };
static struct cpu *cpu; /* points into main's frame */
static enum { M_RUN, M_BENCH, M_FILL, M_PIPE, M_MIGRATE } mode;
static int pfd[2];

static long pclone(void) { return __syscall1(SYS_pclone, 0); }

static void run(void) {
  for (;;) {
    const struct ins *i = &cpu->prog[cpu->pc++];
    switch (i->op) {
    case OP_SET: cpu->r[i->a] = i->b; break;
    case OP_FORK: cpu->r[0] = pclone(); if (cpu->r[0] < 0) { printf("pclone: %ld\n", cpu->r[0]); exit(1); } break;
    case OP_JZ: if (!cpu->r[i->a]) cpu->pc = i->b; break;
    case OP_JMP: cpu->pc = i->b; break;
    case OP_DEC_JNZ: if (--cpu->r[i->a]) cpu->pc = i->b; break;
    case OP_PRINT: printf("%s r0=%s r1=%ld r2=%ld own-pid=%d\n", i->s, cpu->r[0] > 0 ? "pid" : cpu->r[0] ? "?" : "0", cpu->r[1], cpu->r[2], cpu->r[3] == getpid()); fflush(stdout); break;
    case OP_WAIT: { int st; pid_t p = waitpid(-1, &st, 0); cpu->r[2] = p > 0 ? WEXITSTATUS(st) : -1; cpu->sum += cpu->r[2]; break; }
    case OP_EXIT: fflush(stdout); exit(i->a);
    case OP_END: return;
    }
  }
}

/* The child enters here: the same memory and stack pointer, a fresh call stack. */
__attribute__((export_name("__fork_child"))) void __fork_child(void) {
  if (mode == M_BENCH) _exit(0);
  if (mode == M_FILL) for (;;) sleep(10);
  if (mode == M_PIPE) { close(pfd[0]); write(pfd[1], "hello from the child", 20); exit(5); }
  cpu->r[0] = 0; /* the "system call" returns 0 in the child */
  cpu->r[3] = getpid();
  run();
  exit(0);
}

/* r1 = 41; fork; child: r2 = 1, print, exit 7; parent: wait, print */
static const struct ins basic[] = {
  /* 0 */ {OP_SET, 1, 41, 0},
  /* 1 */ {OP_FORK, 0, 0, 0},
  /* 2 */ {OP_JZ, 0, 6, 0},
  /* 3 */ {OP_WAIT, 0, 0, 0},
  /* 4 */ {OP_PRINT, 0, 0, "parent"},
  /* 5 */ {OP_END, 0, 0, 0},
  /* 6 */ {OP_SET, 2, 1, 0},
  /* 7 */ {OP_PRINT, 0, 0, "child"},
  /* 8 */ {OP_EXIT, 7, 0, 0},
};
/* r1 rounds: fork; the child exits 1; the parent waits and adds the status */
static const struct ins loop[] = {
  /* 0 */ {OP_FORK, 0, 0, 0},
  /* 1 */ {OP_JZ, 0, 4, 0},
  /* 2 */ {OP_WAIT, 0, 0, 0},
  /* 3 */ {OP_DEC_JNZ, 1, 0, 0},
  /* 4 */ {OP_JZ, 0, 6, 0}, /* reached with r0 = pid (parent, done) or 0 (child) */
  /* 5 */ {OP_END, 0, 0, 0},
  /* 6 */ {OP_EXIT, 1, 0, 0},
};

static char *keep, *live;
static unsigned rg[2 * 1025], mig_ranges, mig_bytes, live_mib;
static double mig_t0;
static int hold;
static double now_us(void);
/* the new instance after SYS_pmigrate: memory copied range by range, a fresh call stack */
__attribute__((export_name("__migrate_resume"))) void __migrate_resume(void) {
  double dt = now_us() - mig_t0;
  int ok = !strcmp(keep, "kept across the move") && cpu->r[1] == 41;
  for (unsigned i = 0; ok && i < live_mib << 20; i += 4096) ok = live[i] == (char)(i >> 12);
  printf("migrated: state %s, %u ranges, %u KiB copied of %u KiB, pause %.0f us\n", ok ? "intact" : "CORRUPT",
         mig_ranges, mig_bytes >> 10, (unsigned)(__builtin_wasm_memory_size(0) * 64), dt);
  fflush(stdout);
  if (hold) sleep(hold); /* stay alive so that the host's memory can be measured */
  exit(ok ? 0 : 1);
}
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
  struct cpu c; /* on the stack on purpose */
  memset(&c, 0, sizeof c);
  cpu = &c;
  c.r[3] = getpid();
  const char *m = argc > 1 ? argv[1] : "basic";
  if (!strcmp(m, "basic")) {
    c.prog = basic;
    run();
    return 0;
  }
  if (!strcmp(m, "loop")) {
    c.prog = loop;
    c.r[1] = argc > 2 ? atoi(argv[2]) : 100;
    long n = c.r[1];
    run();
    printf("loop %ld sum %ld\n", n, c.sum);
    return 0;
  }
  if (!strcmp(m, "fill")) { /* clone until the process table is full, then clean up */
    mode = M_FILL;
    static long pids[64];
    int n = 0;
    long r;
    while ((r = pclone()) > 0 && n < 64) pids[n++] = r;
    printf("fill %d then %s\n", n, r == -EAGAIN ? "EAGAIN" : "other");
    for (int i = 0; i < n; i++) kill(pids[i], SIGKILL);
    int reaped = 0;
    while (waitpid(-1, 0, 0) > 0) reaped++;
    printf("reaped %d\n", reaped);
    return 0;
  }
  if (!strcmp(m, "pipe")) { /* a pipe made before the clone: the child writes, the parent reads */
    int *p = pfd;
    if (pipe(p)) return 2;
    mode = M_PIPE;
    long r = pclone();
    if (r < 0) return 3;
    c.r[0] = r;
    close(p[1]);
    char buf[64] = {0};
    long k = read(p[0], buf, sizeof buf - 1);
    int st;
    waitpid(c.r[0], &st, 0);
    printf("pipe read %ld: %s, status %d\n", k, buf, WEXITSTATUS(st));
    return 0;
  }
  if (!strcmp(m, "migrate")) {
    unsigned total = argc > 2 ? atoi(argv[2]) : 64;
    live_mib = argc > 3 ? atoi(argv[3]) : 0;
    char *big = malloc((size_t)total << 20);
    if (!big) return 3;
    memset(big, 7, (size_t)total << 20); /* every page touched once */
    live = malloc(((size_t)live_mib << 20) + 1);
    for (unsigned i = 0; i < live_mib << 20; i += 4096) live[i] = (char)(i >> 12);
    keep = malloc(64);
    strcpy(keep, "kept across the move");
    free(big);
    hold = argc > 4 ? atoi(argv[4]) : 0;
    if (argc > 5) { printf("not migrating, holding\n"); fflush(stdout); sleep(hold); return 0; } /* the comparison */
    c.r[1] = 41;
    mode = M_MIGRATE;
    int n = __migrate_ranges(rg, 1025, (((unsigned)(unsigned long)&c) - 4096) & ~15u);
    mig_ranges = n; mig_bytes = 0;
    for (int i = 0; i < n; i++) mig_bytes += rg[2 * i + 1];
    mig_t0 = now_us();
    long r = __syscall3(SYS_pmigrate, rg, n, 0);
    printf("pmigrate failed: %ld\n", r);
    return 1;
  }
  if (!strcmp(m, "bench")) { /* time pclone, and pclone + wait, with a memory of the given size */
    int mib = argc > 2 ? atoi(argv[2]) : 0, n = argc > 3 ? atoi(argv[3]) : 20;
    if (mib) { char *big = malloc((size_t)mib << 20); if (!big) return 3; memset(big, 1, (size_t)mib << 20); }
    mode = M_BENCH;
    static double t[200], tw[200];
    if (n > 200) n = 200;
    for (int i = 0; i < n; i++) {
      double t0 = now_us();
      long r = pclone();
      double t1 = now_us();
      if (r < 0) { printf("pclone: %ld\n", r); return 1; }
      waitpid(r, 0, 0);
      t[i] = t1 - t0; tw[i] = now_us() - t0;
    }
    printf("clone first %.0f us, clone+wait first %.0f us\n", t[0], tw[0]);
    qsort(t, n, sizeof *t, cmpd);
    qsort(tw, n, sizeof *tw, cmpd);
    printf("bench memory %u KiB: clone best %.0f median %.0f us; clone+wait best %.0f median %.0f us (n=%d)\n",
           (unsigned)(__builtin_wasm_memory_size(0) * 64), t[0], t[n / 2], tw[0], tw[n / 2], n);
    return 0;
  }
  return 2;
}
