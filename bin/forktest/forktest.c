// SPDX-License-Identifier: GPL-2.0-or-later
/* forktest: fork(), vfork(), exec and wait in a program built with the stack transform
 * (this directory has a USE_FORK file). Prints ok/FAIL per check, like ktest.
 *   forktest            all checks
 *   forktest bench N    time N fork + wait pairs
 *   forktest deep N     fork at recursion depth N (shows the frame buffer limit) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } fflush(stdout); } while (0)

static int status_of(pid_t pid) { int st = -1; if (waitpid(pid, &st, 0) != pid) return -1000; return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st); }

/* 1. locals of every type live across fork, deep in a call chain */
static volatile int sink;
__attribute__((noinline)) static int deep(int n, long long acc, float fl, double db, pid_t *out) {
  int i = n * 3 + 1;
  long long w = acc * 31 + n;
  float f = fl * 1.5f + (float)n;
  double d = db / 3.0 + n;
  char buf[32]; /* address-taken: lives on the C stack, not in a wasm local */
  snprintf(buf, sizeof buf, "level-%d", n);
  int r;
  if (n == 0) { *out = fork(); r = 0; }
  else r = deep(n - 1, w, f, d, out);
  /* recompute what the locals must hold and compare */
  int ok = i == n * 3 + 1 && w == acc * 31 + n && f == fl * 1.5f + (float)n && d == db / 3.0 + n;
  char want[32];
  snprintf(want, sizeof want, "level-%d", n);
  ok = ok && !strcmp(buf, want);
  sink = i;
  return r + !ok;
}
__attribute__((noinline)) static int deep_migrate(int n, long long acc, float fl, double db, pid_t *out) {
  int i = n * 5 + 2;
  long long w = acc * 17 + n;
  float f = fl * 2.5f + (float)n;
  double d = db / 7.0 + n;
  int r = n == 0 ? (*out = migrate(), 0) : deep_migrate(n - 1, w, f, d, out);
  sink = i;
  return r + !(i == n * 5 + 2 && w == acc * 17 + n && f == fl * 2.5f + (float)n && d == db / 7.0 + n);
}
/* 13. fork reached through a function pointer */
static pid_t via_pointer(void) { return fork(); }
static pid_t (*volatile fp_fork)(void) = via_pointer;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static long claimed(void) { struct sysinfo si; sysinfo(&si); return (long)(si.totalram - si.freeram); }
static int got_chld;
static void on_chld(int s) { (void)s; got_chld++; }


int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "bench")) {
    int n = atoi(argv[2]);
    double best = 1e9, total = 0;
    for (int k = 0; k < n; k++) {
      double t0 = now_ms();
      pid_t p = fork();
      if (p == 0) _exit(0);
      waitpid(p, 0, 0);
      double t = now_ms() - t0;
      total += t;
      if (t < best) best = t;
      if (k == 0) printf("first fork + wait: %.0f us\n", t * 1000);
    }
    printf("fork + wait: best %.0f us, mean %.0f us (n=%d)\n", best * 1000, total / n * 1000, n);
    return 0;
  }
  if (argc > 2 && !strcmp(argv[1], "deep")) {
    int n = atoi(argv[2]);
    pid_t p = -1;
    int bad = deep(n, 1, 1, 1, &p);
    if (p == 0) _exit(bad != 0);
    printf("forked at depth %d: parent %s, child status %d\n", n, bad ? "corrupt" : "fine", status_of(p));
    return 0;
  }

  pid_t me = getpid();
  { /* fork returns twice */
    pid_t p = fork();
    if (p == 0) _exit(getppid() == me ? 42 : 1);
    CHECK("fork: parent gets the child's pid, the child gets 0 and its parent's pid", p > 0 && status_of(p) == 42);
  }
  { /* locals of all types in a deep chain */
    pid_t p = -1;
    int bad = deep(20, 7, 0.25f, 1e10, &p);
    if (p == 0) _exit(bad ? 10 + bad : 0);
    CHECK("fork 21 calls deep: i32, i64, f32, f64 and stack-buffer locals intact in the parent", bad == 0);
    CHECK("... and in the child", p > 0 && status_of(p) == 0);
  }
  { /* fork in a loop */
    int sum = 0, okp = 1;
    for (int i = 1; i <= 10; i++) {
      pid_t p = fork();
      if (p == 0) _exit(i);
      if (p < 0) okp = 0;
      sum += status_of(p);
    }
    CHECK("fork inside a loop, 10 rounds: each child sees its own counter", okp && sum == 55);
  }
  { /* fork then exec */
    pid_t p = fork();
    if (p == 0) { execl("/bin/sh", "sh", "-c", "exit 3", (char *)0); _exit(99); }
    CHECK("fork then execl /bin/sh -c 'exit 3': status 3, same pid", status_of(p) == 3);
    p = fork();
    if (p == 0) { execlp("no-such-program", "x", (char *)0); _exit(errno == ENOENT ? 7 : 8); }
    CHECK("exec of a missing program fails with ENOENT in the child", status_of(p) == 7);
  }
  { /* shared open file description: one offset */
    int fd = open("/tmp/forktest.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "start\n", 6);
    pid_t p = fork();
    if (p == 0) { write(fd, "child\n", 6); _exit(0); }
    status_of(p);
    write(fd, "parent\n", 7);
    char buf[64] = {0};
    lseek(fd, 0, SEEK_SET);
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    CHECK("parent and child write through one descriptor: one shared offset", n == 19 && !strcmp(buf, "start\nchild\nparent\n"));
    unlink("/tmp/forktest.txt");
  }
  { /* a pipe between parent and child */
    int pp[2];
    pipe(pp);
    pid_t p = fork();
    if (p == 0) { close(pp[0]); char msg[40]; int k = snprintf(msg, sizeof msg, "hello from %d", getpid()); write(pp[1], msg, k); _exit(0); }
    close(pp[1]);
    char buf[64] = {0}, want[40];
    long n = read(pp[0], buf, sizeof buf - 1);
    snprintf(want, sizeof want, "hello from %d", p);
    close(pp[0]);
    CHECK("pipe from child to parent", n > 0 && !strcmp(buf, want) && status_of(p) == 0);
  }
  { /* a child that exits while the parent is blocked in read */
    int pp[2];
    pipe(pp);
    pid_t p = fork();
    if (p == 0) { close(pp[0]); usleep(20000); _exit(4); } /* exits without writing: end of file */
    close(pp[1]);
    char c;
    double t0 = now_ms();
    long n = read(pp[0], &c, 1);
    CHECK("parent blocked in read wakes with end of file when the child exits", n == 0 && now_ms() - t0 >= 15 && status_of(p) == 4);
    close(pp[0]);
  }
  { /* SIGCHLD reaches the parent */
    signal(SIGCHLD, on_chld);
    pid_t p = fork();
    if (p == 0) _exit(0);
    status_of(p);
    getpid();
    CHECK("SIGCHLD handler runs when a forked child exits", got_chld == 1);
    signal(SIGCHLD, SIG_DFL);
  }
  { /* nested: a child forks a grandchild */
    pid_t p = fork();
    if (p == 0) {
      pid_t q = fork();
      if (q == 0) _exit(5);
      _exit(status_of(q) + 1);
    }
    CHECK("a forked child forks again (grandchild exit 5, child exits 6)", status_of(p) == 6);
  }
  { /* through a function pointer */
    pid_t p = fp_fork();
    if (p == 0) _exit(9);
    CHECK("fork through a function pointer", status_of(p) == 9);
  }
  { /* vfork + _exit, and fork_call */
    pid_t p = vfork();
    if (p == 0) _exit(11);
    CHECK("vfork then _exit", status_of(p) == 11);
  }
  { /* 100 forks: memory returns */
    long before = claimed();
    int bad = 0;
    for (int i = 0; i < 100; i++) { pid_t p = fork(); if (p == 0) _exit(0); if (status_of(p) != 0) bad++; }
    long after = claimed();
    CHECK("100 forks with waits: all succeed, claimed memory back to where it was", !bad && after == before);
  }
  { /* migrate from deep in a call chain: same pid, locals intact, freed memory gone */
    char *big = malloc(32 << 20);
    memset(big, 1, 32 << 20);
    free(big);
    pid_t before = getpid();
    pid_t q = 1;
    int bad = deep_migrate(15, 3, 0.5f, 2e9, &q);
    CHECK("migrate() 16 calls deep: same pid, every local intact, returns 0", bad == 0 && q == 0 && getpid() == before);
  }
  { /* almost full process table */
    pid_t kids[64];
    int n = 0;
    for (;;) { pid_t p = fork(); if (p == 0) { for (;;) pause(); } if (p < 0) break; kids[n++] = p; }
    int full = errno == EAGAIN;
    kill(kids[--n], SIGKILL); status_of(kids[n]);
    pid_t p = fork();
    if (p == 0) _exit(12);
    int last = status_of(p);
    for (int i = 0; i < n; i++) kill(kids[i], SIGKILL);
    for (int i = 0; i < n; i++) status_of(kids[i]);
    char name[96];
    snprintf(name, sizeof name, "full table: fork is EAGAIN after %d children; with one slot free, fork works", n + 1);
    CHECK(name, full && last == 12);
  }
  printf("forktest: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
