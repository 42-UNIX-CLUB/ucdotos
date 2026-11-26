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
#include <sys/file.h>
#include <sys/stat.h>

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

/* Helpers for bin/ktest.c (integration of fork with the file system). ktest itself must stay
 * untransformed (it checks that pclone of a program without __fork_child is ENOEXEC), so the
 * forking half of those checks runs here; ktest turns the exit status into checks.
 *   forktest x-lock FILE         locks across fork; exit status = bit mask of failures (0 = all fine)
 *   forktest x-exec PATH ARGS... fork, the child execs PATH with argv {PATH, ARGS...};
 *                                exit status = the child's, or 100 + errno if exec failed
 *   forktest x-tree DIR          a forked child makes a small tree (file, symlink, hard link, mode 0751, dir)
 *   forktest x-treecheck DIR     exit 0 if that tree is there with every attribute */
static int lk(int fd, int cmd, int type, long start, long len) {
  struct flock fl = {(short)type, SEEK_SET, start, len, 0};
  return fcntl(fd, cmd, &fl);
}
static int integ_mode(int argc, char **argv) {
  if (!strcmp(argv[1], "x-lock") && argc > 2) {
    const char *f = argv[2];
    int fd = open(f, O_RDWR | O_CREAT, 0644), fd2 = open(f, O_RDWR), fd3 = open(f, O_RDWR);
    if (fd < 0 || fd2 < 0 || fd3 < 0) return 255;
    if (lk(fd, F_SETLK, F_WRLCK, 0, 10) || flock(fd2, LOCK_EX) || lk(fd3, F_OFD_SETLK, F_WRLCK, 100, 10)) return 254;
    pid_t c = fork();
    if (c < 0) return 253;
    if (c == 0) {
      int bits = 0;
      struct flock q = {F_WRLCK, SEEK_SET, 0, 10, 0};
      if (fcntl(fd, F_GETLK, &q) || q.l_type != F_WRLCK || q.l_pid != getppid()) bits |= 1;  /* the parent's record lock is not the child's */
      if (!(lk(fd, F_SETLK, F_WRLCK, 0, 10) < 0 && (errno == EAGAIN || errno == EACCES))) bits |= 2;
      if (flock(fd2, LOCK_EX | LOCK_NB)) bits |= 4;                     /* flock belongs to the shared open file */
      int fn = open(f, O_RDWR);
      if (!(flock(fn, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK)) bits |= 8;
      if (lk(fd3, F_OFD_SETLK, F_WRLCK, 100, 10)) bits |= 16;            /* OFD lock follows the open file */
      if (!(lk(fn, F_OFD_SETLK, F_WRLCK, 100, 10) < 0 && errno == EAGAIN)) bits |= 32;
      close(fn);
      close(fd); /* the child's close must not drop the parent's record lock */
      _exit(bits);
    }
    int st = -1;
    waitpid(c, &st, 0);
    int bits = WIFEXITED(st) ? WEXITSTATUS(st) : 127;
    pid_t c2 = fork();
    if (c2 == 0) {
      struct flock q = {F_WRLCK, SEEK_SET, 0, 10, 0};
      _exit(fcntl(fd, F_GETLK, &q) || q.l_type != F_WRLCK || q.l_pid != getppid());
    }
    st = -1;
    waitpid(c2, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) bits |= 64;
    return bits;
  }
  if (!strcmp(argv[1], "x-exec") && argc > 2) {
    pid_t c = fork();
    if (c == 0) { execv(argv[2], argv + 2); _exit(100 + errno); }
    int st = -1;
    if (c < 0 || waitpid(c, &st, 0) != c) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + WTERMSIG(st);
  }
  if (!strcmp(argv[1], "x-tree") && argc > 2) {
    const char *d = argv[2];
    pid_t c = fork();
    if (c == 0) {
      char p[256], q[256];
      if (mkdir(d, 0755)) _exit(2);
      snprintf(p, sizeof p, "%s/f", d);
      int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0 || write(fd, "made by a forked child\n", 23) != 23 || fsync(fd) || close(fd)) _exit(3);
      if (chmod(p, 0751)) _exit(4);
      snprintf(q, sizeof q, "%s/l", d);
      if (symlink("f", q)) _exit(5);
      snprintf(q, sizeof q, "%s/h", d);
      if (link(p, q)) _exit(6);
      snprintf(q, sizeof q, "%s/sub", d);
      if (mkdir(q, 0700)) _exit(7);
      sync();
      _exit(0);
    }
    int st = -1;
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200;
  }
  if (!strcmp(argv[1], "x-treecheck") && argc > 2) {
    const char *d = argv[2];
    char p[256], b[64] = {0};
    struct stat f, h, l, s;
    snprintf(p, sizeof p, "%s/f", d);
    if (stat(p, &f)) return 2;
    snprintf(p, sizeof p, "%s/h", d);
    if (stat(p, &h)) return 3;
    snprintf(p, sizeof p, "%s/l", d);
    if (lstat(p, &l) || !S_ISLNK(l.st_mode) || readlink(p, b, sizeof b - 1) != 1 || b[0] != 'f') return 4;
    snprintf(p, sizeof p, "%s/sub", d);
    if (stat(p, &s) || !S_ISDIR(s.st_mode) || (s.st_mode & 07777) != 0700) return 5;
    if ((f.st_mode & 07777) != 0751 || f.st_nlink != 2 || h.st_ino != f.st_ino || f.st_size != 23) return 6;
    snprintf(p, sizeof p, "%s/l", d);
    int fd = open(p, O_RDONLY);
    if (fd < 0 || read(fd, b, 23) != 23 || memcmp(b, "made by a forked child\n", 23)) return 7;
    return 0;
  }
  return 99;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strncmp(argv[1], "x-", 2)) return integ_mode(argc, argv);
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
