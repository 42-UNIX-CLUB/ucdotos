// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel self-test: exercises every syscall path and prints ok/FAIL per check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/file.h>
#include <sys/statfs.h>
#include <sys/uio.h>

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static long heap_used(void) { struct sysinfo si; return sysinfo(&si) ? -1 : (long)si.bufferram; } /* kernel heap in use */

/* ---- clone, exec and signals (children are "ktest child-p-NAME") ---- */
#include <signal.h>
#include <sys/time.h>
static volatile int hits[65];
static void on_sig(int s) { hits[s]++; }
static int install(int sig, int flags) { /* a handler with exactly these flags (signal() adds SA_RESTART) */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig;
  sa.sa_flags = flags;
  return sigaction(sig, &sa, 0);
}
static int proc_child(int argc, char **argv) {
  const char *m = argv[1] + 8;
  if (!strcmp(m, "exec")) { /* exec ktest child-p-envexit with an environment */
    char *av[] = {"ktest", "child-p-envexit", 0}, *ev[] = {"K=v", 0};
    execve("/bin/ktest", av, ev);
    return 1;
  }
  if (!strcmp(m, "envexit")) return getenv("K") && !strcmp(getenv("K"), "v") && argc == 2 ? 42 : 2;
  if (!strcmp(m, "execfd")) { /* fd 7 survives exec, fd 8 (close-on-exec) does not */
    dup2(1, 7);
    dup2(1, 8);
    fcntl(8, F_SETFD, FD_CLOEXEC);
    char *av[] = {"ktest", "child-p-fdcheck", 0};
    execve("/bin/ktest", av, 0);
    return 1;
  }
  if (!strcmp(m, "fdcheck")) return write(7, "a", 1) == 1 && fcntl(8, F_GETFD) < 0 && errno == EBADF ? 0 : 3;
  if (!strcmp(m, "execbad")) { /* a text file with the execute bit, so the answer is ENOEXEC at every enforcement level */
    int fd = open("/tmp/ktest-notprog", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    write(fd, "not a program\n", 14); close(fd); chmod("/tmp/ktest-notprog", 0755);
    char *av[] = {"x", 0}; execve("/tmp/ktest-notprog", av, 0); return errno == ENOEXEC ? 0 : 4; }
  if (!strcmp(m, "execsig")) { /* handlers are reset by exec, ignored signals stay ignored */
    install(SIGUSR1, 0);
    signal(SIGUSR2, SIG_IGN);
    char *av[] = {"ktest", "child-p-sigcheck", 0};
    execve("/bin/ktest", av, 0);
    return 1;
  }
  if (!strcmp(m, "sigcheck")) {
    struct sigaction a, b;
    sigaction(SIGUSR1, 0, &a); sigaction(SIGUSR2, 0, &b);
    return a.sa_handler == SIG_DFL && b.sa_handler == SIG_IGN ? 0 : 5;
  }
  if (!strcmp(m, "ignterm")) { signal(SIGTERM, SIG_IGN); write(1, "r", 1); usleep(100000); return 3; }
  if (!strcmp(m, "eintr") || !strcmp(m, "restart")) { /* blocked in read on stdin when SIGUSR1 comes */
    int restart = m[0] == 'r';
    install(SIGUSR1, restart ? SA_RESTART : 0);
    write(1, "r", 1);
    char c;
    long n = read(0, &c, 1);
    if (restart) return n == 1 && c == 'x' && hits[SIGUSR1] == 1 ? 0 : 6;
    return n < 0 && errno == EINTR && hits[SIGUSR1] == 1 ? 0 : 7;
  }
  if (!strcmp(m, "sigpipe")) { int p[2]; pipe(p); close(p[0]); write(p[1], "x", 1); return 8; }
  if (!strcmp(m, "tty")) { /* in the foreground of the terminal on fds 0-2: Ctrl-C and resize interrupt a read */
    int me = getpid();
    setpgid(0, 0);
    __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
    install(SIGINT, 0);
    install(SIGWINCH, 0);
    write(1, "r", 1);
    char c;
    long n = read(0, &c, 1);
    int e1 = n < 0 && errno == EINTR && hits[SIGINT] == 1;
    write(1, "s", 1);
    n = read(0, &c, 1);
    int e2 = n < 0 && errno == EINTR && hits[SIGWINCH] == 1;
    return e1 && e2 ? 0 : 9;
  }
  return 99;
}
static pid_t spawn_k(const char *mode, int in, int out) {
  char name[40];
  snprintf(name, sizeof name, "child-p-%s", mode);
  char *av[] = {"ktest", name, 0};
  int map[3] = {in, out, -1};
  return spawn("/bin/ktest", av, 0, map);
}
static int wait_st(pid_t p) { int st = -1; return waitpid(p, &st, 0) == p ? st : -1; }
/* run clonetest with arguments; its stdout comes back in out */
static int run_clonetest(const char *args, char *out, int cap) {
  int p[2];
  if (pipe2(p, O_CLOEXEC)) return -1;
  char a[64]; strlcpy(a, args, sizeof a);
  char *av[8] = {"clonetest"}; int n = 1;
  for (char *t = strtok(a, " "); t && n < 7; t = strtok(0, " ")) av[n++] = t;
  av[n] = 0;
  int map[3] = {-1, p[1], -1};
  pid_t pid = spawn("/bin/clonetest", av, 0, map);
  close(p[1]);
  int len = 0; long r;
  while (len < cap - 1 && (r = read(p[0], out + len, cap - 1 - len)) > 0) len += r;
  out[len] = 0;
  close(p[0]);
  return pid < 0 ? -1 : wait_st(pid);
}
static void proc_checks(void) {
  char out[512];
  int st, p[2];
  pid_t pid;
  /* the clone primitive */
  CHECK("pclone of a program without __fork_child: ENOEXEC", __syscall1(SYS_pclone, 0) == -ENOEXEC);
  CHECK("pclone with unknown flags: EINVAL", __syscall1(SYS_pclone, 1) == -EINVAL);
  st = run_clonetest("basic", out, sizeof out);
  CHECK("pclone: the child continues from memory (state on the C stack) with 0, the parent with the pid",
        st == 0 && strstr(out, "child r0=0 r1=41 r2=1") && strstr(out, "parent r0=pid r1=41 r2=7"));
  long heap0 = heap_used();
  st = run_clonetest("loop 100", out, sizeof out);
  CHECK("100 clones in a row, each waited for", st == 0 && strstr(out, "loop 100 sum 100"));
  CHECK("... and the kernel heap is back where it was", heap_used() == heap0);
  st = run_clonetest("fill", out, sizeof out);
  int nfill = 0, nreap = -1;
  char *f = strstr(out, "fill ");
  if (f) nfill = atoi(f + 5);
  if ((f = strstr(out, "reaped "))) nreap = atoi(f + 7);
  CHECK("pclone until the table is full: EAGAIN, then every clone killed and reaped", st == 0 && strstr(out, "then EAGAIN") && nfill > 50 && nreap == nfill);
  st = run_clonetest("pipe", out, sizeof out);
  CHECK("pclone: a pipe made before the clone works between the two", st == 0 && strstr(out, "pipe read 20: hello from the child, status 5"));
  /* exec */
  pid = spawn_k("exec", -1, -1);
  st = wait_st(pid);
  CHECK("execve keeps the pid, passes argv and envp (exit status of the new program)", WIFEXITED(st) && WEXITSTATUS(st) == 42);
  pipe2(p, O_CLOEXEC);
  pid = spawn_k("execfd", -1, p[1]);
  close(p[1]);
  char c = 0;
  long n = read(p[0], &c, 1);
  st = wait_st(pid);
  close(p[0]);
  CHECK("execve keeps descriptors and closes the close-on-exec ones", n == 1 && c == 'a' && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  st = wait_st(spawn_k("execbad", -1, -1));
  CHECK("execve of a file that is not a program: ENOEXEC, and the caller goes on", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  st = wait_st(spawn_k("execsig", -1, -1));
  CHECK("execve resets handlers to the default and keeps ignored signals ignored", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  /* signals */
  install(SIGUSR1, 0);
  kill(getpid(), SIGUSR1);
  CHECK("a handler runs for a signal sent to oneself before kill returns", hits[SIGUSR1] == 1);
  sigset_t set, old, pend;
  sigemptyset(&set); sigaddset(&set, SIGUSR1);
  sigprocmask(SIG_BLOCK, &set, &old);
  kill(getpid(), SIGUSR1);
  sigpending(&pend);
  int was = hits[SIGUSR1];
  sigprocmask(SIG_SETMASK, &old, 0);
  CHECK("a blocked signal stays pending and runs when unblocked", was == 1 && sigismember(&pend, SIGUSR1) == 1 && hits[SIGUSR1] == 2);
  sigprocmask(SIG_BLOCK, &set, &old);
  kill(getpid(), SIGUSR1);
  sigset_t none; sigemptyset(&none);
  int r = sigsuspend(&none);
  sigset_t now; sigprocmask(SIG_BLOCK, 0, &now);
  CHECK("sigsuspend: runs the handler, fails with EINTR, puts the mask back", r == -1 && errno == EINTR && hits[SIGUSR1] == 3 && sigismember(&now, SIGUSR1) == 1);
  sigprocmask(SIG_SETMASK, &old, 0);
  install(SIGALRM, 0);
  struct itimerval it = {{0, 0}, {0, 30000}};
  sigset_t alrm; sigemptyset(&alrm); sigaddset(&alrm, SIGALRM);
  sigprocmask(SIG_BLOCK, &alrm, &old); /* a preemption between setitimer and the wait must not lose it */
  double t0 = now_ms();
  setitimer(ITIMER_REAL, &it, 0);
  r = sigsuspend(&old);
  double dt = now_ms() - t0;
  sigprocmask(SIG_SETMASK, &old, 0);
  CHECK("setitimer 30 ms + sigsuspend: SIGALRM handler, EINTR, not early", r == -1 && errno == EINTR && hits[SIGALRM] == 1 && dt >= 25);
  { char *av[] = {"ktest", "child-sleeper", 0}; pid = spawn("/bin/ktest", av, 0, 0); }
  kill(pid, SIGTERM);
  st = wait_st(pid);
  CHECK("SIGTERM with the default action terminates", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
  pipe2(p, O_CLOEXEC);
  pid = spawn_k("ignterm", -1, p[1]);
  close(p[1]);
  read(p[0], &c, 1);
  kill(pid, SIGTERM);
  st = wait_st(pid);
  close(p[0]);
  CHECK("an ignored SIGTERM does nothing", WIFEXITED(st) && WEXITSTATUS(st) == 3);
  for (int restart = 0; restart < 2; restart++) {
    int in[2];
    pipe2(p, O_CLOEXEC); pipe2(in, O_CLOEXEC);
    pid = spawn_k(restart ? "restart" : "eintr", in[0], p[1]);
    close(p[1]); close(in[0]);
    read(p[0], &c, 1);
    usleep(20000); /* let it block in read */
    kill(pid, SIGUSR1);
    usleep(20000);
    if (restart) write(in[1], "x", 1);
    st = wait_st(pid);
    close(p[0]); close(in[1]);
    CHECK(restart ? "SA_RESTART: the read goes on after the handler and gets the data" : "a handler interrupts a blocked read: EINTR", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  st = wait_st(spawn_k("sigpipe", -1, -1));
  CHECK("write to a pipe nobody reads: SIGPIPE terminates the writer", WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);
  signal(SIGPIPE, SIG_IGN);
  pipe(p); close(p[0]);
  n = write(p[1], "x", 1);
  CHECK("... and with SIGPIPE ignored, the write fails with EPIPE", n == -1 && errno == EPIPE);
  close(p[1]);
  signal(SIGPIPE, SIG_DFL);
  { /* a process group */
    char *av[] = {"ktest", "child-sleeper", 0};
    pid_t a = spawn("/bin/ktest", av, 0, 0), b = spawn("/bin/ktest", av, 0, 0);
    setpgid(a, a); setpgid(b, a);
    r = kill(-a, SIGTERM);
    int sa = wait_st(a), sb = wait_st(b);
    CHECK("kill(-pgid) reaches every process of the group", r == 0 && WTERMSIG(sa) == SIGTERM && WTERMSIG(sb) == SIGTERM);
    CHECK("kill of an empty group: ESRCH", kill(-a, SIGTERM) == -1 && errno == ESRCH);
  }
  { /* stop and continue */
    char *av[] = {"ktest", "child-sleeper", 0};
    pid = spawn("/bin/ktest", av, 0, 0);
    usleep(10000);
    kill(pid, SIGSTOP);
    int s1 = -1, s2 = -1;
    pid_t w1 = waitpid(pid, &s1, WUNTRACED);
    kill(pid, SIGCONT);
    pid_t w2 = waitpid(pid, &s2, WCONTINUED);
    kill(pid, SIGKILL);
    st = wait_st(pid);
    CHECK("SIGSTOP: waitpid(WUNTRACED) reports it stopped; SIGCONT: waitpid(WCONTINUED) reports it continued",
          w1 == pid && WIFSTOPPED(s1) && WSTOPSIG(s1) == SIGSTOP && w2 == pid && WIFCONTINUED(s2) && WTERMSIG(st) == SIGKILL);
  }
  { /* SIGCHLD ignored: no zombies */
    signal(SIGCHLD, SIG_IGN);
    char *av[] = {"ktest", "child-exit", "0", 0};
    pid = spawn("/bin/ktest", av, 0, 0);
    usleep(50000);
    r = waitpid(pid, &st, 0);
    CHECK("with SIGCHLD ignored, an exited child leaves no zombie (waitpid: ECHILD)", r == -1 && errno == ECHILD);
    signal(SIGCHLD, SIG_DFL);
  }
  { /* SIGCHLD handler */
    install(SIGCHLD, SA_RESTART);
    char *av[] = {"ktest", "child-exit", "0", 0};
    pid = spawn("/bin/ktest", av, 0, 0);
    wait_st(pid);
    getpid();
    CHECK("SIGCHLD handler runs when a child exits", hits[SIGCHLD] == 1);
    signal(SIGCHLD, SIG_DFL);
  }
  { /* the terminal: Ctrl-C and a resize */
    int m = open("/dev/ptmx", O_RDWR), num = -1;
    char path[32];
    __syscall3(SYS_ioctl, m, TIOCGPTN, &num);
    snprintf(path, sizeof path, "/dev/pts/%d", num);
    int s = open(path, O_RDWR);
    char *av[] = {"ktest", "child-p-tty", 0};
    int map[3] = {s, s, s};
    pid = spawn("/bin/ktest", av, 0, map);
    char b[64];
    int got = 0;
    while (got == 0 && read(m, b, 1) == 1) got = b[0] == 'r';
    usleep(20000);
    write(m, "\x03", 1);
    got = 0;
    while (got == 0 && read(m, b, 1) == 1) got = b[0] == 's';
    usleep(20000);
    struct winsize ws = {30, 100, 0, 0};
    __syscall3(SYS_ioctl, m, TIOCSWINSZ, &ws);
    st = wait_st(pid);
    CHECK("a handler for SIGINT (Ctrl-C) and SIGWINCH (resize) interrupts a read on the terminal", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(s); close(m);
  }
  CHECK("tgkill with a thread id that is not the process: ESRCH; tgkill(pid, pid, 0) works",
        __syscall3(SYS_tgkill, getpid(), getpid() + 1, 0) == -ESRCH && __syscall3(SYS_tgkill, getpid(), getpid(), 0) == 0);
}

/* ---- file system: links, modes, owners, times, locks, statx, /proc (2026-11-26) ---- */
#define FS "/tmp/fs"
static int fs_child(int argc, char **argv) {
  if (!strcmp(argv[1], "child-fs-lock") && argc > 3) { /* take a write lock on argv[2], say so on stdout, hold it argv[3] ms */
    int fd = open(argv[2], O_RDWR);
    struct flock fl = {F_WRLCK, SEEK_SET, 0, 0, 0};
    if (fd < 0 || fcntl(fd, F_SETLK, &fl) < 0) return 2;
    write(1, "L", 1);
    usleep(atoi(argv[3]) * 1000);
    return 0;
  }
  if (!strcmp(argv[1], "child-fs-chroot")) { /* chroot, then: / is the new root and .. stops there */
    struct stat a, b;
    if (chroot(FS "/jail") || chdir("/")) return 2;
    if (stat("/inside", &a) || stat("/../../inside", &b) || a.st_ino != b.st_ino) return 3;
    if (stat("/tmp", &a) == 0) return 4; /* the old root is out of reach */
    char cwd[64];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/")) return 5;
    return 0;
  }
  if (!strcmp(argv[1], "child-fs-stdin")) { /* stdin is a pipe: /dev/stdin reopens that same pipe */
    char b[16] = {0};
    int fd = open("/dev/stdin", O_RDONLY);
    if (fd < 0) return 2;
    if (read(fd, b, 5) != 5 || memcmp(b, "hello", 5)) return 3;
    return 0;
  }
  return 1;
}
static long lsize(const char *p) { struct stat st; return lstat(p, &st) ? -1 : (long)st.st_size; }
static unsigned long long ino_of(const char *p) { struct stat st; return stat(p, &st) ? 0 : st.st_ino; }
static int nlink_of(const char *p) { struct stat st; return lstat(p, &st) ? -1 : (int)st.st_nlink; }
static unsigned mode_of(const char *p) { struct stat st; return lstat(p, &st) ? 0 : st.st_mode; }
static int put(const char *p, const char *s) { int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644); if (fd < 0) return -1; int n = write(fd, s, strlen(s)); close(fd); return n; }
static int get(const char *p, char *buf, int cap) { int fd = open(p, O_RDONLY); if (fd < 0) return -1; int n = read(fd, buf, cap - 1); close(fd); if (n >= 0) buf[n] = 0; return n; }
static void fs_tests(void) {
  char buf[512], b2[512];
  struct stat st, s2;
  int fd, fd2, status;
  printf("--- file system: links, modes, owners, times, locks\n");
  mkdir(FS, 0755);
  mkdir(FS "/d", 0755); mkdir(FS "/d/e", 0755);
  put(FS "/f", "data\n");

  /* symbolic links */
  CHECK("symlink + readlink", symlink("f", FS "/l") == 0 && readlink(FS "/l", buf, sizeof buf) == 1 && buf[0] == 'f');
  CHECK("lstat sees the link, stat the target", lstat(FS "/l", &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 1 &&
        stat(FS "/l", &s2) == 0 && S_ISREG(s2.st_mode) && s2.st_size == 5);
  CHECK("read through a relative link", get(FS "/l", buf, sizeof buf) == 5 && !strcmp(buf, "data\n"));
  CHECK("readlink truncates to the buffer, no NUL", readlink(FS "/l", buf, 0) == -1 && errno == EINVAL && symlink("abcdef", FS "/l6") == 0 && readlink(FS "/l6", buf, 3) == 3 && !memcmp(buf, "abc", 3));
  CHECK("readlink on a file -> EINVAL", readlink(FS "/f", buf, sizeof buf) == -1 && errno == EINVAL);
  CHECK("symlink over an existing name -> EEXIST; empty target -> ENOENT", symlink("x", FS "/f") == -1 && errno == EEXIST && symlink("", FS "/le") == -1 && errno == ENOENT);
  CHECK("dangling link: stat ENOENT, lstat works", symlink("nowhere", FS "/dang") == 0 && stat(FS "/dang", &st) == -1 && errno == ENOENT && lstat(FS "/dang", &st) == 0);
  CHECK("O_CREAT through a dangling link creates the target", (fd = open(FS "/dang", O_CREAT | O_WRONLY, 0644)) >= 0 && close(fd) == 0 && stat(FS "/nowhere", &st) == 0);
  CHECK("O_CREAT|O_EXCL on a link -> EEXIST, even dangling", unlink(FS "/nowhere") == 0 && open(FS "/dang", O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
  CHECK("O_NOFOLLOW on a link -> ELOOP", open(FS "/l", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
  fd = open(FS "/l", O_PATH | O_NOFOLLOW);
  CHECK("O_PATH|O_NOFOLLOW opens the link itself", fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode) && read(fd, buf, 1) == -1);
  CHECK("readlinkat(fd, \"\") reads an O_PATH link", readlinkat(fd, "", buf, sizeof buf) == 1 && buf[0] == 'f');
  close(fd);
  symlink("loop2", FS "/loop1"); symlink("loop1", FS "/loop2");
  CHECK("a loop of links -> ELOOP", stat(FS "/loop1", &st) == -1 && errno == ELOOP);
  {
    char a[64], b[64];
    int ok = 1;
    for (int k = 1; k <= 41; k++) { snprintf(a, sizeof a, FS "/c%d", k); snprintf(b, sizeof b, k == 1 ? "f" : "c%d", k - 1); ok &= symlink(b, a) == 0; }
    CHECK("40 links in a row resolve, 41 -> ELOOP", ok && stat(FS "/c40", &st) == 0 && S_ISREG(st.st_mode) && stat(FS "/c41", &st) == -1 && errno == ELOOP);
    for (int k = 1; k <= 41; k++) { snprintf(a, sizeof a, FS "/c%d", k); unlink(a); }
  }
  CHECK("absolute link to a directory; walk through it", symlink(FS "/d/e", FS "/de") == 0 && put(FS "/de/x", "x") == 1 && stat(FS "/d/e/x", &st) == 0);
  CHECK("'..' after a link is the target's real parent", ino_of(FS "/de/..") == ino_of(FS "/d") && ino_of(FS "/de/..") != ino_of(FS));
  {
    char cwd[256], old[256];
    getcwd(old, sizeof old);
    CHECK("getcwd after chdir through a link is the physical path", chdir(FS "/de") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, FS "/d/e"));
    chdir(old);
  }
  CHECK("relative target is taken from the link's directory", symlink("../f", FS "/d/up") == 0 && get(FS "/d/up", buf, sizeof buf) == 5);
  CHECK("trailing slash: a file -> ENOTDIR", stat(FS "/f/", &st) == -1 && errno == ENOTDIR && open(FS "/f/", O_RDONLY) == -1 && errno == ENOTDIR);
  CHECK("trailing slash follows a link to a directory", lstat(FS "/de/", &st) == 0 && S_ISDIR(st.st_mode) && lstat(FS "/de", &s2) == 0 && S_ISLNK(s2.st_mode));
  CHECK("rmdir(\"link/\") -> ENOTDIR; unlink(link) removes only the link", rmdir(FS "/de/") == -1 && errno == ENOTDIR &&
        unlink(FS "/de") == 0 && stat(FS "/d/e/x", &st) == 0);
  CHECK("rename moves the link, not its target", rename(FS "/l", FS "/l2") == 0 && lsize(FS "/l2") == 1 && lsize(FS "/f") == 5 && readlink(FS "/l2", buf, sizeof buf) == 1);
  CHECK("a file in the middle of a path -> ENOTDIR", stat(FS "/f/x", &st) == -1 && errno == ENOTDIR);
  CHECK("O_CREAT on a missing name with a trailing slash -> EISDIR", open(FS "/newf/", O_CREAT | O_WRONLY, 0644) == -1 && errno == EISDIR);
  CHECK("fstatat AT_SYMLINK_NOFOLLOW", fstatat(AT_FDCWD, FS "/l2", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode) && fstatat(AT_FDCWD, FS "/l2", &st, 0) == 0 && S_ISREG(st.st_mode));
  CHECK("lchown changes the link, chown the target", lchown(FS "/l2", 7, 8) == 0 && lstat(FS "/l2", &st) == 0 && st.st_uid == 7 && stat(FS "/f", &s2) == 0 && s2.st_uid != 7);

  /* hard links */
  CHECK("link: two names, one inode, nlink 2", link(FS "/f", FS "/h") == 0 && ino_of(FS "/h") == ino_of(FS "/f") && nlink_of(FS "/f") == 2 && nlink_of(FS "/h") == 2);
  CHECK("a write through one name shows through the other", put(FS "/h", "other\n") == 6 && get(FS "/f", buf, sizeof buf) == 6 && !strcmp(buf, "other\n"));
  CHECK("unlink one name: the data stays, nlink 1", unlink(FS "/f") == 0 && get(FS "/h", buf, sizeof buf) == 6 && nlink_of(FS "/h") == 1);
  CHECK("link onto an existing name -> EEXIST; to a directory -> EPERM", link(FS "/h", FS "/d/up") == -1 && errno == EEXIST && link(FS "/d", FS "/d2") == -1 && errno == EPERM);
  CHECK("linkat without AT_SYMLINK_FOLLOW links the link itself", linkat(AT_FDCWD, FS "/l2", AT_FDCWD, FS "/l3", 0) == 0 && S_ISLNK(mode_of(FS "/l3")) && nlink_of(FS "/l2") == 2);
  put(FS "/f", "five\n");
  CHECK("linkat with AT_SYMLINK_FOLLOW links the target", linkat(AT_FDCWD, FS "/l2", AT_FDCWD, FS "/l4", AT_SYMLINK_FOLLOW) == 0 && S_ISREG(mode_of(FS "/l4")) && nlink_of(FS "/f") == 2);
  fd = open(FS "/h", O_RDONLY);
  CHECK("the last name of an open file goes: still readable, cannot be linked back", unlink(FS "/h") == 0 && pread(fd, buf, 6, 0) == 6 &&
        linkat(fd, "", AT_FDCWD, FS "/back", AT_EMPTY_PATH) == -1 && errno == ENOENT);
  close(fd);
  {
    int n0 = nlink_of(FS "/d");
    mkdir(FS "/d/sub", 0755);
    int n1 = nlink_of(FS "/d"), ns = nlink_of(FS "/d/sub");
    rmdir(FS "/d/sub");
    CHECK("directory link counts: 2 + subdirectories", ns == 2 && n1 == n0 + 1 && nlink_of(FS "/d") == n0);
  }
  CHECK("rename over another name of the same file does nothing", rename(FS "/f", FS "/l4") == 0 && nlink_of(FS "/f") == 2 && nlink_of(FS "/l4") == 2);

  /* modes, owners, umask */
  mode_t old = umask(027);
  fd = open(FS "/um", O_CREAT | O_WRONLY, 0666);
  close(fd);
  mkdir(FS "/umd", 0777);
  CHECK("umask applies to open and mkdir", old == 022 && (mode_of(FS "/um") & 07777) == 0640 && (mode_of(FS "/umd") & 07777) == 0750 && umask(old) == 027);
  CHECK("chmod, fchmodat, set-user-ID kept", chmod(FS "/um", 04755) == 0 && (mode_of(FS "/um") & 07777) == 04755 && fchmodat(AT_FDCWD, FS "/um", 0600, 0) == 0 && (mode_of(FS "/um") & 07777) == 0600);
  fd = open(FS "/um", O_RDONLY);
  CHECK("fchmod on a read-only descriptor", fchmod(fd, 0444) == 0 && (mode_of(FS "/um") & 07777) == 0444);
  close(fd);
  CHECK("fchmodat AT_SYMLINK_NOFOLLOW on a link -> EOPNOTSUPP", fchmodat(AT_FDCWD, FS "/l2", 0600, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP);
  chmod(FS "/um", 06755);
  CHECK("chown sets ids, -1 keeps one, and clears set-user-ID", chown(FS "/um", 0, 0) == 0 && stat(FS "/um", &st) == 0 && st.st_uid == 0 && st.st_gid == 0 &&
        chown(FS "/um", 42, -1) == 0 && stat(FS "/um", &st) == 0 && st.st_uid == 42 && st.st_gid == 0 && !(st.st_mode & 06000));
  CHECK("a new file belongs to the caller", stat(FS "/d/up", &st) == 0 && lstat(FS "/d", &st) == 0 && st.st_uid == getuid() && st.st_gid == getgid());
  CHECK("files from the image keep their tar owner and mode", stat("/bin/sh", &st) == 0 && st.st_uid == 0 && (st.st_mode & 0111) && stat("/etc/motd", &s2) == 0 && (s2.st_mode & 0777) == 0644);
  if (st.st_uid != 0 || !(st.st_mode & 0111) || (s2.st_mode & 0777) != 0644) printf("  (/bin/sh uid %u mode %o, /etc/motd uid %u mode %o size %lld)\n", st.st_uid, st.st_mode, s2.st_uid, s2.st_mode, s2.st_size);
  CHECK("access(F_OK) on a missing file -> ENOENT", access(FS "/missing", F_OK) == -1 && errno == ENOENT);
  CHECK("access(X_OK) on a program, access(W_OK) on a new file", access("/bin/sh", X_OK) == 0 && access(FS "/d/up", W_OK) == 0);

  /* times */
  {
    struct timespec t[2] = {{1000000000, 123456789}, {1200000000, 987654321}};
    struct statx sx;
    stat(FS "/um", &s2);
    CHECK("utimensat with explicit times, read back to the nanosecond", utimensat(AT_FDCWD, FS "/um", t, 0) == 0 &&
          statx(AT_FDCWD, FS "/um", 0, STATX_BASIC_STATS, &sx) == 0 && sx.stx_atime.tv_sec == 1000000000 && sx.stx_atime.tv_nsec == 123456789 &&
          sx.stx_mtime.tv_sec == 1200000000 && sx.stx_mtime.tv_nsec == 987654321 && sx.stx_ctime.tv_sec >= s2.st_ctime);
    struct timespec o[2] = {{5, UTIME_OMIT}, {0, UTIME_NOW}};
    CHECK("UTIME_OMIT keeps a time, UTIME_NOW sets it", utimensat(AT_FDCWD, FS "/um", o, 0) == 0 && stat(FS "/um", &st) == 0 && st.st_atime == 1000000000 && st.st_mtime >= s2.st_ctime);
    struct timespec bad[2] = {{0, 1000000000}, {0, 0}};
    CHECK("utimensat with a bad tv_nsec -> EINVAL", utimensat(AT_FDCWD, FS "/um", bad, 0) == -1 && errno == EINVAL);
    CHECK("utimensat honours its directory descriptor", (fd = open(FS "/d", O_RDONLY | O_DIRECTORY)) >= 0 && utimensat(fd, "up", t, AT_SYMLINK_NOFOLLOW) == 0 &&
          lstat(FS "/d/up", &st) == 0 && st.st_mtime == 1200000000 && close(fd) == 0);
    fd = open(FS "/um", O_RDONLY);
    CHECK("futimens", futimens(fd, t) == 0 && fstat(fd, &st) == 0 && st.st_mtime == 1200000000);
    close(fd);
    utimensat(AT_FDCWD, FS "/d", t, 0);
    put(FS "/d/new", "n");
    CHECK("creating a name updates the directory's mtime", stat(FS "/d", &st) == 0 && st.st_mtime > 1200000000);
    fd = open(FS "/um", O_WRONLY);
    utimensat(AT_FDCWD, FS "/um", t, 0);
    CHECK("a write updates mtime", write(fd, "z", 1) == 1 && fstat(fd, &st) == 0 && st.st_mtime > 1200000000);
    close(fd);
    { /* make compares modification times: they need nanoseconds, and must never repeat */
      struct stat a1, a2;
      fd = open(FS "/mk", O_CREAT | O_WRONLY | O_TRUNC, 0644);
      int ok = 1;
      for (int k = 0; k < 50 && ok; k++) { fstat(fd, &a1); write(fd, "x", 1); fstat(fd, &a2);
        ok = a2.st_mtim.tv_sec > a1.st_mtim.tv_sec || (a2.st_mtim.tv_sec == a1.st_mtim.tv_sec && a2.st_mtim.tv_nsec > a1.st_mtim.tv_nsec); }
      close(fd);
      CHECK("two writes in a row never get the same mtime (nanoseconds in struct stat)", ok);
      utimensat(AT_FDCWD, FS "/mk", t, 0);
      fd = open(FS "/mk", O_WRONLY | O_TRUNC); /* ": > f" */
      CHECK("open with O_TRUNC updates mtime and ctime", fd >= 0 && fstat(fd, &a1) == 0 && a1.st_mtime > 1200000000 && a1.st_ctim.tv_sec >= a1.st_mtim.tv_sec && close(fd) == 0);
      utimensat(AT_FDCWD, FS "/mk", t, 0);
      CHECK("truncate to the same size updates mtime", truncate(FS "/mk", 0) == 0 && stat(FS "/mk", &a1) == 0 && a1.st_mtime > 1200000000);
      CHECK("stat and statx give the same nanoseconds", statx(AT_FDCWD, FS "/mk", 0, STATX_MTIME, &sx) == 0 && stat(FS "/mk", &a2) == 0 &&
            sx.stx_mtime.tv_sec == a2.st_mtim.tv_sec && sx.stx_mtime.tv_nsec == (unsigned)a2.st_mtim.tv_nsec);
    }
    CHECK("statx agrees with stat", statx(AT_FDCWD, FS "/d/new", 0, STATX_BASIC_STATS, &sx) == 0 && stat(FS "/d/new", &st) == 0 &&
          sx.stx_ino == st.st_ino && sx.stx_size == 1 && sx.stx_nlink == 1 && sx.stx_mode == st.st_mode && (sx.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS);
    CHECK("statx AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH", statx(AT_FDCWD, FS "/l2", AT_SYMLINK_NOFOLLOW, STATX_TYPE, &sx) == 0 && S_ISLNK(sx.stx_mode) &&
          (fd = open(FS "/d", O_RDONLY)) >= 0 && statx(fd, "", AT_EMPTY_PATH, STATX_TYPE, &sx) == 0 && S_ISDIR(sx.stx_mode) && close(fd) == 0);
  }

  /* credentials: one user unless changed */
  {
    uid_t r, e, s;
    gid_t g[4] = {5, 6};
    CHECK("getuid/geteuid/getgid/getegid", getuid() == 1000 && geteuid() == 1000 && getgid() == 1000 && getegid() == 1000);
    CHECK("getresuid", getresuid(&r, &e, &s) == 0 && r == 1000 && e == 1000 && s == 1000);
    CHECK("setgroups + getgroups", setgroups(2, g) == 0 && getgroups(0, 0) == 2 && getgroups(4, g) == 2 && g[1] == 6 && setgroups(0, 0) == 0);
  }

  /* renameat2, mknod, truncate, fallocate, sync */
  put(FS "/ra", "A"); put(FS "/rb", "B");
  CHECK("RENAME_NOREPLACE onto an existing name -> EEXIST", renameat2(AT_FDCWD, FS "/ra", AT_FDCWD, FS "/rb", RENAME_NOREPLACE) == -1 && errno == EEXIST);
  CHECK("RENAME_EXCHANGE swaps two names", renameat2(AT_FDCWD, FS "/ra", AT_FDCWD, FS "/rb", RENAME_EXCHANGE) == 0 && get(FS "/ra", buf, 4) == 1 && buf[0] == 'B' && get(FS "/rb", buf, 4) == 1 && buf[0] == 'A');
  CHECK("renameat2 with unknown flags -> EINVAL", renameat2(AT_FDCWD, FS "/ra", AT_FDCWD, FS "/rc", 64) == -1 && errno == EINVAL);
  CHECK("mkfifo makes a FIFO node; opening it -> ENXIO", mkfifo(FS "/fifo", 0600) == 0 && S_ISFIFO(mode_of(FS "/fifo")) && open(FS "/fifo", O_RDONLY | O_NONBLOCK) == -1 && errno == ENXIO);
  CHECK("mknod of a device -> EPERM", mknod(FS "/dev", S_IFCHR | 0600, 0) == -1 && errno == EPERM);
  CHECK("truncate by path, both ways", truncate(FS "/ra", 100) == 0 && lsize(FS "/ra") == 100 && truncate(FS "/ra", 0) == 0 && lsize(FS "/ra") == 0);
  CHECK("truncate a directory -> EISDIR", truncate(FS "/d", 0) == -1 && errno == EISDIR);
  fd = open(FS "/ra", O_RDWR);
  CHECK("posix_fallocate extends, never shrinks", posix_fallocate(fd, 0, 5000) == 0 && lsize(FS "/ra") == 5000 && posix_fallocate(fd, 0, 10) == 0 && lsize(FS "/ra") == 5000);
  CHECK("fsync, fdatasync, syncfs, sync", fsync(fd) == 0 && fdatasync(fd) == 0 && syncfs(fd) == 0 && (sync(), 1) && fsync(99) == -1 && errno == EBADF);

  /* vectored I/O */
  {
    struct iovec v[3] = {{"ab", 2}, {"", 0}, {"cdef", 4}};
    char x[3], y[8] = {0};
    struct iovec r[2] = {{x, 3}, {y, 3}};
    CHECK("writev + preadv", lseek(fd, 0, SEEK_SET) == 0 && writev(fd, v, 3) == 6 && preadv(fd, r, 2, 0) == 6 && !memcmp(x, "abc", 3) && !memcmp(y, "def", 3));
    CHECK("pwritev does not move the offset; readv", pwritev(fd, v, 1, 10) == 2 && lseek(fd, 0, SEEK_CUR) == 6 && lseek(fd, 9, SEEK_SET) == 9 && readv(fd, r, 1) == 3 && x[1] == 'a');
    CHECK("readv with a negative count -> EINVAL", readv(fd, r, -1) == -1 && errno == EINVAL);
  }
  close(fd);

  /* open flags */
  CHECK("O_TMPFILE -> EOPNOTSUPP (callers fall back)", open(FS, O_TMPFILE | O_RDWR, 0600) == -1 && errno == EOPNOTSUPP);
  CHECK("O_DIRECTORY on a file -> ENOTDIR", open(FS "/ra", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
  fd = open(FS "/ap", O_CREAT | O_WRONLY | O_APPEND, 0644); fd2 = open(FS "/ap", O_WRONLY);
  CHECK("O_APPEND always writes at the end", write(fd, "12", 2) == 2 && write(fd2, "x", 1) == 1 && write(fd, "34", 2) == 2 && get(FS "/ap", buf, sizeof buf) == 4 && !strcmp(buf, "x234"));
  close(fd); close(fd2);

  /* locks */
  fd = open(FS "/lk", O_CREAT | O_RDWR, 0644); fd2 = open(FS "/lk", O_RDWR);
  CHECK("flock: an exclusive lock blocks a second open file", flock(fd, LOCK_EX) == 0 && flock(fd2, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
  CHECK("flock: shared locks share, unlock frees", flock(fd, LOCK_SH) == 0 && flock(fd2, LOCK_SH | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0 && flock(fd2, LOCK_UN) == 0 && flock(fd2, LOCK_EX | LOCK_NB) == 0);
  close(fd2);
  CHECK("flock: closing the file drops its lock", flock(fd, LOCK_EX | LOCK_NB) == 0);
  {
    int p[2];
    pipe2(p, O_CLOEXEC);
    int map[3] = {-1, p[1], -1};
    char *la[] = {"ktest", "child-fs-lock", FS "/lk", "300", 0};
    int pid = spawn("/bin/ktest", la, 0, map);
    close(p[1]);
    char c = 0;
    read(p[0], &c, 1);
    close(p[0]);
    struct flock fl = {F_WRLCK, SEEK_SET, 0, 0, 0};
    CHECK("fcntl F_SETLK against another process's lock -> EAGAIN", c == 'L' && fcntl(fd, F_SETLK, &fl) == -1 && errno == EAGAIN);
    fl.l_type = F_RDLCK; fl.l_start = 10; fl.l_len = 5;
    CHECK("F_GETLK names the holder", fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_WRLCK && fl.l_pid == pid && fl.l_start == 0 && fl.l_len == 0);
    fl.l_type = F_WRLCK; fl.l_start = 0; fl.l_len = 0;
    double t0 = now_ms();
    CHECK("F_SETLKW waits for the holder to go", fcntl(fd, F_SETLKW, &fl) == 0 && now_ms() - t0 > 100);
    waitpid(pid, &status, 0);
    struct flock un = {F_UNLCK, SEEK_SET, 0, 0, 0};
    fcntl(fd, F_SETLK, &un);
    flock(fd, LOCK_UN);
  }
  close(fd);

  /* directory listings */
  {
    mkdir(FS "/t", 0755);
    put(FS "/t/r", "r"); mkdir(FS "/t/dd", 0755); symlink("r", FS "/t/ln"); mkfifo(FS "/t/ff", 0600);
    struct { unsigned short f; char p[108]; } sa = {1, FS "/t/so"};
    int s = __syscall3(SYS_socket, 1, 1, 0);
    __syscall3(SYS_bind, s, &sa, sizeof sa);
    int want = 0;
    DIR *d = opendir(FS "/t");
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (!strcmp(e->d_name, "r")) want |= (e->d_type == DT_REG) << 0;
      if (!strcmp(e->d_name, "dd")) want |= (e->d_type == DT_DIR) << 1;
      if (!strcmp(e->d_name, "ln")) want |= (e->d_type == DT_LNK) << 2;
      if (!strcmp(e->d_name, "ff")) want |= (e->d_type == DT_FIFO) << 3;
      if (!strcmp(e->d_name, "so")) want |= (e->d_type == DT_SOCK) << 4;
    }
    closedir(d);
    close(s);
    CHECK("getdents64 d_type for files, directories, links, FIFOs, sockets", want == 31);
    d = opendir("/dev");
    want = 0;
    while (d && (e = readdir(d))) if (!strcmp(e->d_name, "null")) want = e->d_type == DT_CHR;
    closedir(d);
    CHECK("... and devices", want);
  }
  {
    mkdir(FS "/big", 0755);
    int ok = 1;
    for (int k = 0; k < 3000 && ok; k++) { snprintf(buf, sizeof buf, FS "/big/n%d", k); ok = (fd = open(buf, O_CREAT | O_WRONLY, 0644)) >= 0 && close(fd) == 0; }
    int seen = 0, removed = 0;
    DIR *d = opendir(FS "/big");
    struct dirent *e;
    while (d && (e = readdir(d))) { /* remove while listing: nothing may be skipped or repeated */
      if (e->d_name[0] != 'n') continue;
      seen++;
      snprintf(buf, sizeof buf, FS "/big/%s", e->d_name);
      removed += unlink(buf) == 0;
    }
    closedir(d);
    CHECK("3,000 entries: listing while removing sees each one once", ok && seen == 3000 && removed == 3000 && rmdir(FS "/big") == 0);
  }
  {
    char p[600] = FS;
    for (int k = 0; k < 20; k++) { strcat(p, "/abcdefghijklmnopqrstuvwxyz"); mkdir(p, 0755); }
    CHECK("a path longer than 255 bytes", strlen(p) > 500 && put(strcat(p, "/end"), "e") == 1 && stat(p, &st) == 0);
    char *huge = malloc(5000);
    memset(huge, 'a', 4999); huge[0] = '/'; huge[4999] = 0;
    CHECK("a path of 4,999 bytes -> ENAMETOOLONG", stat(huge, &st) == -1 && errno == ENAMETOOLONG);
    free(huge);
  }

  /* statfs */
  {
    struct statfs sf;
    CHECK("statfs: type, block size, free space, name length", statfs(FS, &sf) == 0 && sf.f_type == UCOSFS_MAGIC && sf.f_bsize == 4096 && sf.f_bfree > 0 && sf.f_blocks >= sf.f_bfree && sf.f_namelen == 255);
    CHECK("statfs on /proc", statfs("/proc", &sf) == 0 && sf.f_type == PROC_SUPER_MAGIC);
  }

  /* /proc and /dev */
  snprintf(b2, sizeof b2, "%d", getpid());
  CHECK("/proc/self is a link to /proc/PID", readlink("/proc/self", buf, sizeof buf) == (long)strlen(b2) && !memcmp(buf, b2, strlen(b2)));
  fd = open(FS "/d/new", O_RDONLY);
  snprintf(b2, sizeof b2, "/proc/self/fd/%d", fd);
  long n = readlink(b2, buf, sizeof buf);
  CHECK("/proc/self/fd/N reads as the file's path", n == (long)strlen(FS "/d/new") && !memcmp(buf, FS "/d/new", n));
  unlink(FS "/d/new");
  CHECK("... and opens the file even after it was unlinked", get(b2, buf, sizeof buf) == 1 && buf[0] == 'n');
  close(fd);
  CHECK("/proc/self/exe and /proc/self/cwd", (n = readlink("/proc/self/exe", buf, sizeof buf)) == 10 && !memcmp(buf, "/bin/ktest", 10) &&
        stat("/proc/self/cwd", &st) == 0 && st.st_ino == ino_of("."));
  {
    char rp[256];
    CHECK("realpath resolves links", realpath(FS "/d/up", rp) && !strcmp(rp, FS "/f"));
  }
  CHECK("/dev/fd is /proc/self/fd; /dev/stdout is fd 1", readlink("/dev/fd", buf, sizeof buf) == 13 && stat("/dev/stdout", &st) == 0 && fstat(1, &s2) == 0 && st.st_ino == s2.st_ino);
  {
    int seen = 0;
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    while (d && (e = readdir(d))) seen |= !strcmp(e->d_name, "1") && e->d_type == DT_LNK;
    closedir(d);
    CHECK("listing /proc/self/fd", seen);
  }
  CHECK("/proc/cpuinfo, meminfo, mounts", get("/proc/cpuinfo", buf, sizeof buf) > 10 && get("/proc/meminfo", buf, sizeof buf) > 10 && !strncmp(buf, "MemTotal:", 9) &&
        get("/proc/mounts", buf, sizeof buf) > 10 && strstr(buf, " / "));
  CHECK("/proc files cannot be written or created", open("/proc/meminfo", O_WRONLY) == -1 && errno == EACCES && open("/proc/new", O_CREAT | O_WRONLY, 0644) == -1);
  {
    int p[2];
    pipe2(p, O_CLOEXEC);
    int map[3] = {p[0], -1, -1};
    char *sa[] = {"ktest", "child-fs-stdin", 0};
    int pid = spawn("/bin/ktest", sa, 0, map);
    write(p[1], "hello", 5);
    close(p[0]); close(p[1]);
    CHECK("/dev/stdin on a pipe reads that pipe", waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  {
    mkdir(FS "/jail", 0755);
    put(FS "/jail/inside", "i");
    char *ca[] = {"ktest", "child-fs-chroot", 0};
    int pid = spawn("/bin/ktest", ca, 0, 0);
    CHECK("chroot: / and .. stay inside", waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  {
    long h0 = heap_used();
    for (int k = 0; k < 2000; k++) {
      put(FS "/m1", "abc"); link(FS "/m1", FS "/m2"); symlink(FS "/m1", FS "/m3"); rename(FS "/m2", FS "/m4");
      unlink(FS "/m1"); unlink(FS "/m3"); unlink(FS "/m4");
    }
    CHECK("2,000 rounds of link, symlink, rename, unlink give all memory back", heap_used() == h0);
  }
}


/* ---- integration of fork, exec and signals with the file system (2026-11-26) ---- */
#define IX "/tmp/ix"
static int ix_lk(int fd, int cmd, int type, long start, long len) {
  struct flock fl = {(short)type, SEEK_SET, start, len, 0};
  return fcntl(fd, cmd, &fl);
}
static int integ_child(int argc, char **argv) {
  const char *m = argv[1] + 8;
  if (!strcmp(m, "exe")) { /* after an exec through symbolic links, /proc/self/exe names the real program */
    char b[64] = {0};
    return readlink("/proc/self/exe", b, sizeof b - 1) > 0 && !strcmp(b, "/bin/ktest") ? 0 : 3;
  }
  if (!strcmp(m, "hold") && argc > 2) { /* lock [0,10), say L; then on 'g' wait for byte 100, on anything else exit */
    int fd = open(argv[2], O_RDWR);
    if (fd < 0 || ix_lk(fd, F_SETLK, F_WRLCK, 0, 10)) return 2;
    write(1, "L", 1);
    char c = 0;
    if (read(0, &c, 1) != 1 || c != 'g') return 0;
    if (ix_lk(fd, F_SETLKW, F_WRLCK, 100, 1) == 0) return 0;
    return errno == EDEADLK ? 3 : 4;
  }
  if (!strcmp(m, "lockwait") && argc > 3) { /* hold byte 100, then wait for [0,10) when SIGUSR1 comes */
    int restart = atoi(argv[3]);
    int fd = open(argv[2], O_RDWR);
    if (fd < 0 || ix_lk(fd, F_SETLK, F_WRLCK, 100, 1)) return 2;
    install(SIGUSR1, restart ? SA_RESTART : 0);
    write(1, "r", 1);
    int r = ix_lk(fd, F_SETLKW, F_WRLCK, 0, 10);
    if (restart) return r == 0 && hits[SIGUSR1] == 1 ? 0 : 5;
    int ok = r < 0 && errno == EINTR && hits[SIGUSR1] == 1;
    write(1, "e", 1);
    usleep(300000); /* keep byte 100 locked a while */
    return ok ? 0 : 6;
  }
  if (!strcmp(m, "execlock") && argc > 3) { /* lock [0,10), exec, and the program after exec checks the lock */
    int fd = open(argv[2], O_RDWR | (atoi(argv[3]) ? O_CLOEXEC : 0));
    if (fd < 0 || ix_lk(fd, F_SETLK, F_WRLCK, 0, 10)) return 2;
    char *av[] = {"ktest", "child-x-afterexec", argv[2], argv[3], 0};
    execve("/bin/ktest", av, 0);
    return 3;
  }
  if (!strcmp(m, "afterexec") && argc > 3) { /* a third process looks at the lock */
    char *av[] = {"ktest", "child-x-getlk", argv[2], argv[3], 0};
    pid_t c = spawn("/bin/ktest", av, 0, 0);
    int st = -1;
    if (c < 0 || waitpid(c, &st, 0) != c) return 4;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 5;
  }
  if (!strcmp(m, "getlk") && argc > 3) { /* arg 1: expect no lock; 0: expect the parent's write lock */
    int fd = open(argv[2], O_RDWR);
    struct flock q = {F_WRLCK, SEEK_SET, 0, 10, 0};
    if (fd < 0 || fcntl(fd, F_GETLK, &q)) return 6;
    if (atoi(argv[3])) return q.l_type == F_UNLCK ? 0 : 7;
    return q.l_type == F_WRLCK && q.l_pid == getppid() ? 0 : 8;
  }
  return 99;
}
static int run_forktest(const char *a1, const char *a2, const char *a3, const char *a4) {
  char *av[] = {"forktest", (char *)a1, (char *)a2, (char *)a3, (char *)a4, 0};
  pid_t c = spawn("/bin/forktest", av, 0, 0);
  int st = -1;
  if (c < 0 || waitpid(c, &st, 0) != c) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}
static int copy_file(const char *from, const char *to, int mode) {
  int a = open(from, O_RDONLY), b = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
  static char buf[8192];
  long n;
  if (a < 0 || b < 0) return -1;
  while ((n = read(a, buf, sizeof buf)) > 0) if (write(b, buf, n) != n) return -1;
  close(a); close(b);
  return chmod(to, mode);
}
static void integ_checks(void) {
  struct stat st0;
  if (stat("/bin/forktest", &st0)) { printf("  info  /bin/forktest is not in the image: integration checks skipped\n"); return; }
  printf("--- fork, exec and signals with the file system\n");
  mkdir(IX, 0755);
  symlink("/bin", IX "/bindir");
  symlink("bindir/ktest", IX "/kt");
  symlink("loop2", IX "/loop1");
  symlink("loop1", IX "/loop2");
  int st;
  { char *av[] = {"ktest", "child-exit", "7", 0}; pid_t c = spawn(IX "/kt", av, 0, 0); st = -1; waitpid(c, &st, 0); }
  CHECK("spawn through a file symlink to a directory symlink runs the program", WIFEXITED(st) && WEXITSTATUS(st) == 7);
  CHECK("fork, then exec through the same symbolic links", run_forktest("x-exec", IX "/kt", "child-exit", "9") == 9);
  CHECK("after exec through symbolic links, /proc/self/exe names the real program", run_forktest("x-exec", IX "/kt", "child-x-exe", 0) == 0);
  CHECK("exec of a symbolic link loop: ELOOP", run_forktest("x-exec", IX "/loop1", 0, 0) == 100 + ELOOP);
  /* the execute bit, by the enforcement level (a kernel build switch; access(X_OK) tells which) */
  copy_file("/bin/ktest", IX "/nox", 0644);
  copy_file("/bin/ktest", IX "/yesx", 0755);
  int level1 = access(IX "/nox", X_OK) != 0;
  printf("  info  execute bit enforced: %s\n", level1 ? "yes (level 1 or 2)" : "no (level 0)");
  int r = run_forktest("x-exec", IX "/nox", "child-exit", "5");
  CHECK("exec of a file without any execute bit: EACCES when enforced, runs when not", level1 ? r == 100 + EACCES : r == 5);
  { char *av[] = {"ktest", "child-exit", "5", 0}; pid_t c = spawn(IX "/nox", av, 0, 0);
    if (c > 0) { st = -1; waitpid(c, &st, 0); }
    CHECK("spawn of the same file: the same answer as exec", level1 ? (c < 0 && errno == EACCES) : (c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 5)); }
  CHECK("exec of a file with the execute bit runs at every level", run_forktest("x-exec", IX "/yesx", "child-exit", "6") == 6);
  /* locks across fork */
  r = run_forktest("x-lock", IX "/lockfile", 0, 0);
  if (r) printf("  info  forktest x-lock returned %d\n", r);
  CHECK("fork: the child does not own the parent's record lock (F_GETLK names the parent, F_SETLK is refused)", r >= 0 && r < 128 && !(r & 3));
  CHECK("fork: a flock lock belongs to the shared open file (the child can use it, a new open cannot)", r >= 0 && r < 128 && !(r & 12));
  CHECK("fork: an OFD lock follows the open file (same for the child, refused through a new open)", r >= 0 && r < 128 && !(r & 48));
  CHECK("fork: the child's close and exit leave the parent's record lock in place", r >= 0 && r < 128 && !(r & 64));
  /* exec keeps record locks; a close-on-exec descriptor's close drops them */
  int fd = open(IX "/execlock", O_RDWR | O_CREAT, 0644); close(fd);
  { char *av[] = {"ktest", "child-x-execlock", IX "/execlock", "0", 0}; pid_t c = spawn("/bin/ktest", av, 0, 0); st = -1; waitpid(c, &st, 0); }
  CHECK("execve keeps the process's record locks (POSIX)", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  { char *av[] = {"ktest", "child-x-execlock", IX "/execlock", "1", 0}; pid_t c = spawn("/bin/ktest", av, 0, 0); st = -1; waitpid(c, &st, 0); }
  CHECK("...but closing a close-on-exec descriptor of the file at exec drops them", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  /* a signal interrupting a lock wait */
  fd = open(IX "/waitlock", O_RDWR | O_CREAT, 0644); close(fd);
  for (int restart = 0; restart < 2; restart++) {
    int hin[2], hout[2], wout[2];
    pipe2(hin, O_CLOEXEC); pipe2(hout, O_CLOEXEC); pipe2(wout, O_CLOEXEC);
    char *ha[] = {"ktest", "child-x-hold", IX "/waitlock", 0};
    int hmap[3] = {hin[0], hout[1], -1};
    pid_t h = spawn("/bin/ktest", ha, 0, hmap);
    close(hin[0]); close(hout[1]);
    char c = 0;
    read(hout[0], &c, 1);
    char *wa[] = {"ktest", "child-x-lockwait", IX "/waitlock", restart ? "1" : "0", 0};
    int wmap[3] = {-1, wout[1], -1};
    pid_t w = spawn("/bin/ktest", wa, 0, wmap);
    close(wout[1]);
    read(wout[0], &c, 1);
    usleep(30000); /* let it block in F_SETLKW */
    kill(w, SIGUSR1);
    int hs = -1, ws = -1;
    if (!restart) {
      c = 0;
      long n = read(wout[0], &c, 1);
      write(hin[1], "g", 1); /* the holder now waits for the byte the interrupted waiter holds */
      waitpid(w, &ws, 0);
      waitpid(h, &hs, 0);
      CHECK("a signal handler interrupts F_SETLKW: EINTR", n == 1 && c == 'e' && WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
      CHECK("...and the interrupted wait leaves no trace: no false EDEADLK for a process that then waits on it", WIFEXITED(hs) && WEXITSTATUS(hs) == 0);
      if (!WIFEXITED(hs) || WEXITSTATUS(hs)) printf("  info  holder status %d\n", hs);
    } else {
      usleep(30000);
      write(hin[1], "x", 1); /* the holder exits, the waiter gets the lock */
      waitpid(h, &hs, 0);
      waitpid(w, &ws, 0);
      CHECK("with SA_RESTART the F_SETLKW wait goes on after the handler and gets the lock", WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
    }
    close(hin[1]); close(hout[0]); close(wout[0]);
  }
  /* a tree made by a forked child (persistence across a reboot: tools/tests/integ-store.mjs) */
  r = run_forktest("x-tree", IX "/tree", 0, 0);
  CHECK("a forked child makes a tree with a symlink, a hard link and modes; the parent sees all of it", r == 0 && run_forktest("x-treecheck", IX "/tree", 0, 0) == 0);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strncmp(argv[1], "child-fs", 8)) return fs_child(argc, argv);
  if (argc > 1 && !strncmp(argv[1], "child-p-", 8)) return proc_child(argc, argv);
  if (argc > 1 && !strncmp(argv[1], "child-x-", 8)) return integ_child(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "child-exit")) return atoi(argv[2]);
  if (argc > 1 && !strcmp(argv[1], "child-crash")) { volatile int *p = (int *)0x7ffffff0; return *p; }
  if (argc > 1 && !strcmp(argv[1], "child-upper")) {
    int c;
    while ((c = getchar()) != EOF) putchar(c >= 'a' && c <= 'z' ? c - 32 : c);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "child-sleeper")) { sleep(100); return 0; }
  if (argc > 1 && !strcmp(argv[1], "child-spin")) { for (;;) __syscall0(SYS_getppid); }
  if (argc > 1 && !strcmp(argv[1], "child-sockflood")) { /* fill a socket, then say who we are from a stack buffer */
    struct { unsigned short fam; char path[108]; } addr = {1, "/tmp/flood.sock"};
    int s = __syscall3(SYS_socket, 1, 1, 0);
    if (s < 0 || __syscall3(SYS_connect, s, &addr, sizeof addr) < 0) return 2;
    static char blob[1 << 20];
    memset(blob, getpid(), sizeof blob);
    for (int i = 0; i < 3; i++) {
      char msg[64];
      int len = snprintf(msg, sizeof msg, "pid %d round %d\n", getpid(), i);
      if (write(s, blob, sizeof blob) != sizeof blob) return 3; /* blocks: the socket holds 512 KiB */
      if (write(1, msg, len) != len) return 4;
    }
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "child-count")) { /* count stdin, report on stdout */
    static char cbuf[4096];
    long total = 0, r;
    while ((r = read(0, cbuf, sizeof cbuf)) > 0) total += r;
    printf("%ld %d\n", total, getpid());
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "child-flood")) { /* writes more than a pipe holds */
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    for (int i = 0; i < 64; i++) if (write(1, buf, sizeof buf) != sizeof buf) return 1;
    return 0;
  }

  if (argc > 1 && !strcmp(argv[1], "child-ttytimed")) { /* a timed read on the tty (fd 0) that a hangup ends */
    struct termios t;
    if (__syscall3(SYS_ioctl, 0, TCGETS, &t)) return 2;
    t.c_lflag &= ~(ICANON | ECHO | ISIG); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 10;
    if (__syscall3(SYS_ioctl, 0, TCSETS, &t)) return 2;
    char c;
    if (read(0, &c, 1) != 0) return 3;
    double t0 = now_ms();
    usleep(2000);
    return now_ms() - t0 > 200 ? 4 : 0; /* a stale deadline would make this sleep last until it */
  }
  if (argc > 1 && (!strcmp(argv[1], "child-ctrlc-self") || !strcmp(argv[1], "child-dup3-hup") || !strcmp(argv[1], "child-hold-pty"))) {
    int m = open("/dev/ptmx", O_RDWR), n = -1;
    char path[32];
    if (m < 0 || __syscall3(SYS_ioctl, m, TIOCGPTN, &n)) return 2;
    snprintf(path, sizeof path, "/dev/pts/%d", n);
    int s = open(path, O_RDWR), me = getpid();
    if (s < 0 || __syscall2(SYS_setpgid, 0, 0) || __syscall3(SYS_ioctl, s, TIOCSPGRP, &me)) return 2;
    if (argv[1][6] == 'c') { /* Ctrl-C at our own terminal, followed by more input */
      static char in[3000];
      memset(in, 'x', sizeof in); in[0] = 3;
      write(m, in, sizeof in);
    } else if (argv[1][6] == 'd') __syscall3(SYS_dup3, s, m, 0); /* closes the master: hangup */
    else sleep(100);
    return 3; /* should not get here */
  }
  if (argc > 1 && !strcmp(argv[1], "child-longsleep")) { struct timespec ts = {1ll << 40, 0}; nanosleep(&ts, 0); return 0; }
  if (argc > 1 && !strcmp(argv[1], "child-sock")) { /* connect, ping, expect pong, then send 2 MiB */
    struct { unsigned short fam; char path[108]; } addr = {1, "/tmp/test.sock"};
    int s = __syscall3(SYS_socket, 1, 1, 0);
    if (s < 0 || __syscall3(SYS_connect, s, &addr, sizeof addr) < 0) return 2;
    char b[8] = {0};
    if (write(s, "ping", 4) != 4 || read(s, b, 4) != 4 || memcmp(b, "pong", 4)) return 3;
    static char blob[65536];
    for (int i = 0; i < 32; i++) { memset(blob, 'a' + i % 26, sizeof blob); if (write(s, blob, sizeof blob) != sizeof blob) return 4; }
    close(s);
    return 0;
  }

  printf("ktest: pid %d, parent %d\n", getpid(), getppid());
  CHECK("argv[0]", argc == 1 && argv[0] && !argv[1]);
  if (getpid() != 1) CHECK("environment", getenv("HOME") && !strcmp(getenv("HOME"), "/home/user"));

  /* files */
  int fd = open("/tmp/a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK("create file", fd >= 0);
  CHECK("write", write(fd, "hello, world\n", 13) == 13);
  CHECK("close", close(fd) == 0);
  char buf[256];
  fd = open("/tmp/a.txt", O_RDONLY);
  CHECK("read back", read(fd, buf, sizeof buf) == 13 && !memcmp(buf, "hello, world\n", 13));
  CHECK("read at EOF", read(fd, buf, sizeof buf) == 0);
  CHECK("lseek", lseek(fd, 7, SEEK_SET) == 7 && read(fd, buf, 5) == 5 && !memcmp(buf, "world", 5));
  struct stat st;
  CHECK("fstat", fstat(fd, &st) == 0 && st.st_size == 13 && S_ISREG(st.st_mode));
  close(fd);
  CHECK("open missing -> ENOENT", open("/nope", O_RDONLY) == -1 && errno == ENOENT);
  CHECK("O_EXCL -> EEXIST", open("/tmp/a.txt", O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
  CHECK("bad pointer -> EFAULT", write(1, (void *)0x7ffffff0, 4) == -1 && errno == EFAULT);
  CHECK("bad fd -> EBADF", read(31, buf, 1) == -1 && errno == EBADF);
  CHECK("mkdir", mkdir("/tmp/d", 0755) == 0 && stat("/tmp/d", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK("chdir + getcwd", chdir("/tmp/d") == 0 && getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp/d"));
  CHECK("relative path", (fd = open("../a.txt", O_RDONLY)) >= 0);
  close(fd);
  chdir("/");
  int found = 0;
  DIR *d = opendir("/tmp");
  for (struct dirent *e; d && (e = readdir(d));) if (!strcmp(e->d_name, "a.txt")) found = 1;
  if (d) closedir(d);
  CHECK("readdir", found);
  CHECK("remove directory", rmdir("/tmp/d") == 0 && stat("/tmp/d", &st) == -1);
  CHECK("unlink", unlink("/tmp/a.txt") == 0 && stat("/tmp/a.txt", &st) == -1);
  fd = open("/bin/ktest", O_RDONLY);
  CHECK("initrd file", fd >= 0 && read(fd, buf, 4) == 4 && !memcmp(buf, "\0asm", 4));
  close(fd);
  fd = open("/dev/zero", O_RDONLY);
  memset(buf, 1, 64);
  CHECK("/dev/zero", read(fd, buf, 64) == 64 && buf[0] == 0 && buf[63] == 0);
  close(fd);
  FILE *f = fopen("/tmp/stdio.txt", "w");
  fprintf(f, "%d %s %5.2f %x\n", 42, "str", 3.14159, 255);
  fclose(f);
  f = fopen("/tmp/stdio.txt", "r");
  CHECK("stdio round trip", f && fgets(buf, sizeof buf, f) && !strcmp(buf, "42 str  3.14 ff\n"));
  if (f) fclose(f);

  /* time */
  double t0 = now_ms();
  usleep(30000);
  double dt = now_ms() - t0;
  CHECK("usleep(30 ms) blocks about 30 ms", dt >= 29 && dt < 80);
  CHECK("realtime clock after 2020", time(0) > 1577836800);

  /* processes */
  char *a1[] = {"ktest", "child-exit", "7", 0};
  int status = 0;
  pid_t pid = spawn("/bin/ktest", a1, 0, 0);
  CHECK("spawn", pid > 1);
  CHECK("waitpid + exit status", waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);
  CHECK("wait with no children -> ECHILD", wait(&status) == -1 && errno == ECHILD);
  CHECK("spawn missing -> ENOENT", spawn("/bin/nope", a1, 0, 0) == -1 && errno == ENOENT);
  { int nf = open("/tmp/notwasm", O_CREAT | O_WRONLY | O_TRUNC, 0755); write(nf, "#!/bin/sh\n", 10); close(nf); }
  CHECK("spawn of an executable non-program -> ENOEXEC", spawn("/tmp/notwasm", a1, 0, 0) == -1 && errno == ENOEXEC);
  { int enforced = access("/etc/motd", X_OK) != 0; /* access() follows the kernel's enforcement switch */
    CHECK("spawn of a file without any execute bit -> EACCES (ENOEXEC with enforcement off)", spawn("/etc/motd", a1, 0, 0) == -1 && errno == (enforced ? EACCES : ENOEXEC)); }
  unlink("/tmp/notwasm");
  char *a2[] = {"ktest", "child-crash", 0};
  pid = spawn("/bin/ktest", a2, 0, 0);
  CHECK("crashing child is killed, kernel survives", waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 11);

  /* pipes: parent -> child stdin, child stdout -> parent */
  int in[2], out[2];
  CHECK("pipe", pipe(in) == 0 && pipe(out) == 0);
  fcntl(in[1], F_SETFD, FD_CLOEXEC);
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  int map[3] = {in[0], out[1], -1};
  char *a3[] = {"ktest", "child-upper", 0};
  pid = spawn("/bin/ktest", a3, 0, map);
  close(in[0]); close(out[1]);
  write(in[1], "pipes work\n", 11);
  close(in[1]);
  int n = 0, r;
  while ((r = read(out[0], buf + n, sizeof buf - n)) > 0) n += r;
  CHECK("pipe through a child", n == 11 && !memcmp(buf, "PIPES WORK\n", 11));
  close(out[0]);
  CHECK("reap pipe child", waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);

  /* a writer that fills the pipe must block and resume */
  pipe(out);
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  int map2[3] = {-1, out[1], -1};
  char *a4[] = {"ktest", "child-flood", 0};
  pid = spawn("/bin/ktest", a4, 0, map2);
  close(out[1]);
  long total = 0;
  static char big[8192];
  while ((r = read(out[0], big, sizeof big)) > 0) total += r;
  close(out[0]);
  waitpid(pid, &status, 0);
  CHECK("256 KiB through a 64 KiB pipe", total == 64 * 4096 && WEXITSTATUS(status) == 0);

  /* poll */
  pipe(in);
  struct pollfd pf = {in[0], POLLIN, 0};
  t0 = now_ms();
  CHECK("poll timeout", poll(&pf, 1, 20) == 0 && now_ms() - t0 >= 19);
  write(in[1], "x", 1);
  CHECK("poll ready", poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN));
  close(in[0]); close(in[1]);

  /* kill a blocked process */
  char *a5[] = {"ktest", "child-sleeper", 0};
  pid = spawn("/bin/ktest", a5, 0, 0);
  usleep(5000);
  CHECK("kill", __sysret(__syscall2(SYS_kill, pid, 9)) == 0);
  CHECK("killed child reaped", waitpid(pid, &status, 0) == pid && WTERMSIG(status) == 9);

  /* time slicing: a child that never blocks must not stop the parent from running */
  {
    char *sp[] = {"ktest", "child-spin", 0};
    pid = spawn("/bin/ktest", sp, 0, 0);
    t0 = now_ms();
    usleep(40000);
    dt = now_ms() - t0;
    struct procinfo pi; int seen = 0;
    for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == pid) seen = pi.syscalls > 1000;
    __syscall2(SYS_kill, pid, 9);
    waitpid(pid, &status, 0);
    if (!(dt >= 39 && dt < 150 && seen)) printf("  (slept %.1f ms, child counted %d)\n", dt, seen);
    CHECK("a spinning process is preempted (parent sleeps 40 ms, wakes on time)", dt >= 39 && dt < 150 && seen);
  }
  /* many short-lived processes: pids and table slots must be reused */
  int ok = 1;
  char *a6[] = {"true", 0};
  for (int i = 0; i < 200 && ok; i++) { pid = spawn("/bin/true", a6, 0, 0); ok = pid > 0 && waitpid(pid, &status, 0) == pid && status == 0; }
  CHECK("200 spawn + wait cycles", ok);

  /* Two processes woken in the same instant: a writer exits while its parent waits for it
   * and a reader is blocked on its pipe. Each must then continue as itself. */
  ok = 1;
  for (int round = 0; round < 40 && ok; round++) {
    int p1[2], p2[2];
    pipe2(p1, O_CLOEXEC); pipe2(p2, O_CLOEXEC);
    int wmap[3] = {-1, p1[1], -1}, rmap[3] = {p1[0], p2[1], -1};
    char *wa[] = {"ktest", "child-flood", 0}, *ra[] = {"ktest", "child-count", 0};
    pid_t rp = spawn("/bin/ktest", ra, 0, rmap);
    pid_t wp = spawn("/bin/ktest", wa, 0, wmap);
    close(p1[0]); close(p1[1]); close(p2[1]);
    ok = waitpid(wp, &status, 0) == wp && status == 0;
    n = 0;
    while ((r = read(p2[0], buf + n, sizeof buf - 1 - n)) > 0) n += r;
    buf[n] = 0;
    close(p2[0]);
    ok = ok && waitpid(rp, &status, 0) == rp && status == 0 && atol(buf) == 64 * 4096 && atoi(strchr(buf, ' ') ? strchr(buf, ' ') : "0") == rp && getpid() == (argc == 1 ? getpid() : 0);
  }
  CHECK("40 pipelines with simultaneous wake-ups keep their identity", ok);

  /* pseudo-terminal */
  int ptm = open("/dev/ptmx", O_RDWR), ptn = -1;
  CHECK("open /dev/ptmx", ptm >= 0 && __syscall3(SYS_ioctl, ptm, TIOCGPTN, &ptn) == 0 && ptn >= 0);
  char pts_path[32];
  snprintf(pts_path, sizeof pts_path, "/dev/pts/%d", ptn);
  int pts = open(pts_path, O_RDWR);
  CHECK("open slave", pts >= 0 && isatty(pts));
  CHECK("slave output gets \\r\\n", write(pts, "hi\n", 3) == 3 && read(ptm, buf, sizeof buf) == 4 && !memcmp(buf, "hi\r\n", 4));
  write(ptm, "abc\x7f" "d\r", 6);
  n = read(pts, buf, sizeof buf);
  CHECK("canonical line editing", n == 4 && !memcmp(buf, "abd\n", 4));
  n = read(ptm, buf, sizeof buf);
  CHECK("echo", n == 9 && !memcmp(buf, "abc\b \bd\r\n", 9));
  struct termios tio;
  CHECK("TCGETS", __syscall3(SYS_ioctl, pts, TCGETS, &tio) == 0 && (tio.c_lflag & ICANON));
  tio.c_lflag &= ~(ICANON | ECHO | ISIG);
  tio.c_iflag &= ~ICRNL;
  CHECK("raw mode", __syscall3(SYS_ioctl, pts, TCSETS, &tio) == 0 && write(ptm, "\x03\r", 2) == 2 && read(pts, buf, 8) == 2 && buf[0] == 3 && buf[1] == '\r');
  struct pollfd tp = {pts, POLLIN, 0};
  CHECK("poll on an idle tty", poll(&tp, 1, 10) == 0);
  struct winsize ws = {40, 120, 0, 0}, ws2 = {0, 0, 0, 0};
  CHECK("window size", __syscall3(SYS_ioctl, ptm, TIOCSWINSZ, &ws) == 0 && __syscall3(SYS_ioctl, pts, TIOCGWINSZ, &ws2) == 0 && ws2.ws_col == 120 && ws2.ws_row == 40);
  tio.c_lflag |= ICANON | ECHO | ISIG;
  __syscall3(SYS_ioctl, pts, TCSETS, &tio);
  __syscall0(SYS_setsid);
  int tmap[3] = {pts, pts, pts};
  pid = spawn("/bin/ktest", a5, 0, tmap); /* a sleeper with the tty as stdio */
  __syscall2(SYS_setpgid, pid, pid);
  __syscall3(SYS_ioctl, pts, TIOCSPGRP, &pid);
  write(ptm, "\x03", 1);
  CHECK("Ctrl-C kills the foreground job, not the session leader", waitpid(pid, &status, 0) == pid && WTERMSIG(status) == SIGINT);
  n = read(ptm, buf, sizeof buf);
  CHECK("^C is echoed", n >= 2 && buf[0] == '^' && buf[1] == 'C');
  { /* Ctrl-C while the session leader itself is in the foreground: not killed, but told */
    int self = getpid();
    __syscall3(SYS_ioctl, pts, TIOCSPGRP, &self);
    __syscall0(SYS_intr);
    write(ptm, "\x03", 1);
    CHECK("Ctrl-C reaches a foreground shell as a pending interrupt", __syscall0(SYS_intr) == 1 && __syscall0(SYS_intr) == 0);
    read(ptm, buf, sizeof buf);
  }
  close(pts);
  CHECK("master sees hangup when the slave closes", read(ptm, buf, sizeof buf) == 0);
  close(ptm);

  /* unix sockets */
  struct { unsigned short fam; char path[108]; } addr = {1, "/tmp/test.sock"};
  int ls = __syscall3(SYS_socket, 1, 1, 0);
  CHECK("socket + bind + listen", ls >= 0 && __syscall3(SYS_bind, ls, &addr, sizeof addr) == 0 && __syscall2(SYS_listen, ls, 8) == 0);
  CHECK("socket file exists", stat("/tmp/test.sock", &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK);
  char *a7[] = {"ktest", "child-sock", 0};
  pid = spawn("/bin/ktest", a7, 0, 0);
  int cs = __syscall1(SYS_accept, ls);
  CHECK("accept", cs >= 0);
  CHECK("request/reply", read(cs, buf, 4) == 4 && !memcmp(buf, "ping", 4) && write(cs, "pong", 4) == 4);
  total = 0;
  int good = 1;
  while ((r = read(cs, big, sizeof big)) > 0) { for (int i = 0; i < r; i++) good &= big[i] == 'a' + ((total + i) / 65536) % 26; total += r; }
  CHECK("2 MiB through a socket, in order", total == 32 * 65536 && good);
  CHECK("socket client exit", waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
  close(cs); close(ls);
  int s2 = __syscall3(SYS_socket, 1, 1, 0);
  CHECK("connect with no listener -> ECONNREFUSED", __syscall3(SYS_connect, s2, &addr, sizeof addr) == -ECONNREFUSED);
  close(s2);

  /* Two clients of one server, both blocked in large socket writes while the server drains
   * slowly, each also writing a message from its own stack. Nothing may cross over. */
  {
    struct { unsigned short fam; char path[108]; } faddr = {1, "/tmp/flood.sock"};
    int fl = __syscall3(SYS_socket, 1, 1, 0), mp[2], cfd[2], cpid[2];
    __syscall3(SYS_bind, fl, &faddr, sizeof faddr);
    __syscall2(SYS_listen, fl, 8);
    pipe2(mp, O_CLOEXEC);
    int fmap[3] = {-1, mp[1], -1};
    char *fa[] = {"ktest", "child-sockflood", 0};
    for (int i = 0; i < 2; i++) { cpid[i] = spawn("/bin/ktest", fa, 0, fmap); cfd[i] = __syscall1(SYS_accept, fl); }
    close(mp[1]);
    int first[2] = {-1, -1}, clean = 1;
    long got[2] = {0, 0};
    for (int open2 = 2; open2;) {
      struct pollfd pf2[2] = {{cfd[0], POLLIN, 0}, {cfd[1], POLLIN, 0}};
      poll(pf2, 2, 1000);
      open2 = 0;
      for (int i = 0; i < 2; i++) {
        if (cfd[i] < 0) continue;
        open2++;
        if (!(pf2[i].revents & (POLLIN | POLLHUP))) continue;
        r = read(cfd[i], big, sizeof big);
        if (r <= 0) { close(cfd[i]); cfd[i] = -1; continue; }
        if (first[i] < 0) first[i] = (unsigned char)big[0];
        for (int j = 0; j < r; j++) clean &= (unsigned char)big[j] == first[i];
        got[i] += r;
        usleep(200); /* drain slowly so both writers stay blocked */
      }
    }
    n = 0;
    while ((r = read(mp[0], buf + n, sizeof buf - 1 - n)) > 0) n += r;
    buf[n] = 0;
    close(mp[0]); close(fl);
    int s0, s1;
    waitpid(cpid[0], &s0, 0); waitpid(cpid[1], &s1, 0);
    int msgs = 0, msg_ok = 1;
    for (char *l = buf; *l; l = strchr(l, '\n') + 1) {
      int p = -1, rd = -1;
      if (sscanf(l, "pid %d round %d", &p, &rd) != 2 || (p != cpid[0] && p != cpid[1]) || rd < 0 || rd > 2) msg_ok = 0;
      msgs++;
      if (!strchr(l, '\n')) break;
    }
    CHECK("two blocked socket writers: streams stay separate", clean && got[0] == 3 << 20 && got[1] == 3 << 20 && first[0] != first[1] && (first[0] == (cpid[0] & 255) || first[0] == (cpid[1] & 255)));
    CHECK("two blocked socket writers: each speaks from its own memory", msgs == 6 && msg_ok && s0 == 0 && s1 == 0);
  }

  /* framebuffer and input */
  int fbfd = open("/dev/fb0", O_RDWR);
  unsigned vinfo[40] = {0};
  CHECK("framebuffer info", fbfd >= 0 && __syscall3(SYS_ioctl, fbfd, 0x4600, vinfo) == 0 && vinfo[0] >= 64 && vinfo[1] >= 64 && vinfo[6] == 32);
  unsigned px = 0xff3377ee, back = 0;
  long fboff = (vinfo[0] * 10 + 20) * 4;
  CHECK("pwrite + pread a pixel", __syscall4(SYS_pwrite64, fbfd, &px, 4, fboff) == 4 && __syscall4(SYS_pread64, fbfd, &back, 4, fboff) == 4 && back == px);
  close(fbfd);
  int evfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
  struct { long long sec, usec; unsigned short type, code; int value; } ev[2];
  CHECK("input: the resize event from boot", evfd >= 0 && read(evfd, ev, sizeof ev) == sizeof ev && ev[0].type == 4 && ev[0].code == 0x11 && (unsigned)ev[0].value == ((vinfo[0] << 16) | vinfo[1]) && ev[1].type == 0);
  CHECK("input: empty queue -> EAGAIN", read(evfd, ev, sizeof ev) == -1 && errno == EAGAIN);
  close(evfd);

  /* ---- hardening: regression checks for bugs found by audit and by kfuzz ---- */
  {
    long h0;
    fd = open("/tmp/big", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK("pwrite at offset 0xfffffff0 -> ENOSPC, no hang", __syscall4(SYS_pwrite64, fd, buf, 64, 0xfffffff0u) == -ENOSPC);
    CHECK("ftruncate to -16 (0xfffffff0) -> EINVAL, no hang", __syscall2(SYS_ftruncate, fd, -16) == -EINVAL);
    CHECK("write of 0 bytes past the end does not extend the file", lseek(fd, 5000, SEEK_SET) == 5000 && __syscall3(SYS_write, fd, buf, 0) == 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    CHECK("lseek past 2 GiB -> EINVAL", __syscall3(SYS_lseek, fd, 0x7fffffff, SEEK_SET) == 0x7fffffff && __syscall3(SYS_lseek, fd, 1, SEEK_CUR) == -EINVAL);
    close(fd);
    fd = open("/tmp/big", O_RDONLY);
    CHECK("pwrite and ftruncate on a read-only descriptor -> EBADF, EINVAL", __syscall4(SYS_pwrite64, fd, "x", 1, 0) == -EBADF && __syscall2(SYS_ftruncate, fd, 10) == -EINVAL);
    CHECK("fcntl F_DUPFD with a negative or too large start -> EINVAL", __syscall3(SYS_fcntl, fd, 0, -1000) == -EINVAL && __syscall3(SYS_fcntl, fd, 0, 64) == -EINVAL);
    CHECK("dup3 with unknown flags -> EINVAL", __syscall3(SYS_dup3, fd, 40, 1) == -EINVAL);
    close(fd);
    unlink("/tmp/big");
    fbfd = open("/dev/fb0", O_RDWR);
    CHECK("lseek on /dev/fb0", lseek(fbfd, 64, SEEK_SET) == 64 && write(fbfd, &px, 4) == 4 && __syscall4(SYS_pread64, fbfd, &back, 4, 64) == 4 && back == px);
    close(fbfd);
    fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK("getdents64 with a buffer too small for one entry -> EINVAL", __syscall3(SYS_getdents64, fd, buf, 10) == -EINVAL);
    close(fd);
    pipe(in);
    CHECK("read of 0 bytes from an empty pipe returns at once", __syscall3(SYS_read, in[0], buf, 0) == 0);
    close(in[0]); close(in[1]);
    char name[300];
    snprintf(name, sizeof name, "/tmp/%0255d", 7);
    fd = open(name, O_CREAT | O_WRONLY, 0644);
    CHECK("a 255-byte name works", fd >= 0 && stat(name, &st) == 0 && close(fd) == 0 && unlink(name) == 0);
    snprintf(name, sizeof name, "/tmp/%0256d", 7);
    CHECK("a 256-byte name -> ENAMETOOLONG (it used to be cut, giving duplicates)", open(name, O_CREAT | O_WRONLY, 0644) == -1 && errno == ENAMETOOLONG);
    mkdir("/tmp/gone", 0755);
    chdir("/tmp/gone");
    rmdir("/tmp/gone");
    CHECK("removed working directory: create -> ENOENT", open("f", O_CREAT | O_WRONLY, 0644) == -1 && errno == ENOENT && mkdir("x", 0755) == -1 && errno == ENOENT);
    CHECK("removed working directory: rmdir(.) and rename(.) -> ENOENT, no hang", rmdir(".") == -1 && errno == ENOENT && __syscall4(SYS_renameat, AT_FDCWD, ".", AT_FDCWD, "/tmp/z") == -ENOENT);
    CHECK("removed working directory: getcwd -> ENOENT", getcwd(buf, sizeof buf) == 0 && errno == ENOENT);
    chdir("/");
    mkdir("/tmp/e1", 0755);
    chdir("/tmp/e1");
    fd = open("/dev/zero", O_RDONLY);
    CHECK("an empty path names nothing: rmdir(\"\") keeps the working directory", rmdir("") == -1 && errno == ENOENT && stat(".", &st) == 0 && open("", O_RDONLY) == -1 && errno == ENOENT);
    CHECK("unlinkat(fd of a file, \"\") -> ENOENT, the file stays", __syscall3(SYS_unlinkat, fd, "", 0) == -ENOENT && stat("/dev/zero", &st) == 0);
    CHECK("a relative path with a descriptor that is not a directory -> ENOTDIR", __syscall4(SYS_openat, fd, "x", O_RDONLY, 0) == -ENOTDIR);
    close(fd);
    chdir("/");
    rmdir("/tmp/e1");
    mkdir("/tmp/r1", 0755); mkdir("/tmp/r2", 0755);
    close(open("/tmp/r1/x", O_CREAT | O_WRONLY, 0644));
    int d1 = open("/tmp/r1", O_RDONLY | O_DIRECTORY), d2 = open("/tmp/r2", O_RDONLY | O_DIRECTORY);
    CHECK("rename onto . or .. -> EINVAL", __syscall4(SYS_renameat, AT_FDCWD, "/tmp/r1/x", AT_FDCWD, "/tmp/r2/..") == -EINVAL && __syscall4(SYS_renameat, AT_FDCWD, "/tmp/r1/x", AT_FDCWD, "/") == -EINVAL);
    CHECK("renameat honours its directory descriptors", __syscall4(SYS_renameat, d1, "x", d2, "y") == 0 && stat("/tmp/r2/y", &st) == 0 && stat("/tmp/r1/x", &st) == -1);
    close(d1); close(d2);
    unlink("/tmp/r2/y"); rmdir("/tmp/r1"); rmdir("/tmp/r2");

    h0 = heap_used();
    ok = 1;
    for (int i = 0; i < 10000 && ok; i++) {
      fd = open("/tmp/churn", O_CREAT | O_WRONLY | O_TRUNC, 0644);
      ok = fd >= 0 && write(fd, buf, 100) == 100 && close(fd) == 0 && unlink("/tmp/churn") == 0;
    }
    CHECK("10,000 create, write, unlink cycles give all memory back", ok && heap_used() == h0);
    h0 = heap_used();
    fd = open("/tmp/held", O_CREAT | O_RDWR, 0644);
    write(fd, "still here", 10);
    unlink("/tmp/held");
    CHECK("an unlinked open file stays readable", stat("/tmp/held", &st) == -1 && __syscall4(SYS_pread64, fd, buf, 10, 0) == 10 && !memcmp(buf, "still here", 10));
    close(fd);
    CHECK("... and its memory is freed at the last close", heap_used() == h0);
    h0 = heap_used();
    ok = 1;
    for (int i = 0; i < 3000 && ok; i++) { pid = spawn("/bin/true", a6, 0, 0); ok = pid > 0 && waitpid(pid, &status, 0) == pid && status == 0; }
    CHECK("3,000 spawn and wait cycles give all memory back", ok && heap_used() == h0);
    struct { unsigned short fam; char path[108]; } caddr = {1, "/tmp/churn.sock"};
    close(open("/dev/ptmx", O_RDWR)); /* the first use of a pty number creates /dev/pts/N for good */
    h0 = heap_used();
    ok = 1;
    for (int i = 0; i < 1000 && ok; i++) {
      int l = __syscall3(SYS_socket, 1, 1 | O_NONBLOCK, 0), c = __syscall3(SYS_socket, 1, 1, 0);
      ok = pipe(in) == 0 && __syscall3(SYS_bind, l, &caddr, sizeof caddr) == 0 && __syscall2(SYS_listen, l, 4) == 0 && __syscall3(SYS_connect, c, &caddr, sizeof caddr) == 0;
      int a = __syscall1(SYS_accept, l), m = open("/dev/ptmx", O_RDWR);
      ok = ok && a >= 0 && m >= 0 && write(c, "x", 1) == 1;
      close(in[0]); close(in[1]); close(l); close(c); close(a); close(m);
      unlink("/tmp/churn.sock");
    }
    CHECK("1,000 pipes, socket connections and ptys give all memory back", ok && heap_used() == h0);
    CHECK("kill with a signal number above 64 -> EINVAL", __syscall2(SYS_kill, getpid(), 65) == -EINVAL);
    struct timespec badt = {0, 1000000000}, negt = {-1, 0};
    CHECK("nanosleep with a bad timespec -> EINVAL", __syscall2(SYS_nanosleep, &badt, 0) == -EINVAL);
    CHECK("ppoll with a negative timeout -> EINVAL", __syscall3(SYS_ppoll, 0, 0, &negt) == -EINVAL);
    long rlen = __syscall2(SYS_getrandom, big, sizeof big);
    CHECK("getrandom fills a buffer", rlen == sizeof big);

    /* process groups, pid reuse and terminals */
    ptm = open("/dev/ptmx", O_RDWR); ptn = -1;
    __syscall3(SYS_ioctl, ptm, TIOCGPTN, &ptn);
    snprintf(pts_path, sizeof pts_path, "/dev/pts/%d", ptn);
    pts = open(pts_path, O_RDWR);
    int nogroup = 60, neg = -5;
    CHECK("TIOCSPGRP to a group that does not exist -> ESRCH, negative -> EINVAL", __syscall3(SYS_ioctl, pts, TIOCSPGRP, &nogroup) == -ESRCH && __syscall3(SYS_ioctl, pts, TIOCSPGRP, &neg) == -EINVAL);
    pid_t ga = spawn("/bin/ktest", a5, 0, 0), gb = spawn("/bin/ktest", a5, 0, 0);
    __syscall2(SYS_setpgid, ga, ga); __syscall2(SYS_setpgid, gb, ga);
    CHECK("TIOCSPGRP to a live group", __syscall3(SYS_ioctl, pts, TIOCSPGRP, &ga) == 0);
    __syscall2(SYS_kill, ga, 9); waitpid(ga, &status, 0);
    pid_t gc = spawn("/bin/true", a6, 0, 0);
    CHECK("a pid that still names a live process group is not handed out again", gc > 0 && gc != ga);
    waitpid(gc, &status, 0);
    __syscall2(SYS_kill, gb, 9); waitpid(gb, &status, 0);
    int fgnow = -1;
    CHECK("when a foreground group is gone, the terminal forgets it", __syscall3(SYS_ioctl, pts, TIOCGPGRP, &fgnow) == 0 && fgnow == 0);
    char *e7[] = {"ktest", "child-exit", "7", 0}, *e8[] = {"ktest", "child-exit", "8", 0};
    pid_t wx = spawn("/bin/ktest", e7, 0, 0), wy = spawn("/bin/ktest", e8, 0, 0);
    __syscall2(SYS_setpgid, wy, wy);
    int sx = 0, sy = 0;
    waitpid(wx, &sx, 0);
    usleep(20000);
    CHECK("wait4(0) only waits for children in the caller's group", waitpid(0, &sy, WNOHANG) == -1 && errno == ECHILD);
    CHECK("wait4(-pgid) waits for that group", waitpid(-wy, &sy, 0) == wy && WEXITSTATUS(sy) == 8 && WEXITSTATUS(sx) == 7);

    int tmap3[3] = {pts, -1, -1};
    char *tt[] = {"ktest", "child-ttytimed", 0};
    fcntl(ptm, F_SETFD, FD_CLOEXEC); fcntl(pts, F_SETFD, FD_CLOEXEC); /* the child must not hold the master */
    pid = spawn("/bin/ktest", tt, 0, tmap3);
    usleep(100000);
    close(pts);
    close(ptm); /* hangup: the child's timed read returns 0 */
    CHECK("a timed tty read ended by a hangup leaves no stale deadline behind", waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    h0 = heap_used();
    char *cs1[] = {"ktest", "child-ctrlc-self", 0}, *cs2[] = {"ktest", "child-dup3-hup", 0}, *cs3[] = {"ktest", "child-hold-pty", 0};
    pid = spawn("/bin/ktest", cs1, 0, 0);
    CHECK("Ctrl-C typed by a process at its own terminal kills it cleanly", waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
    pid = spawn("/bin/ktest", cs2, 0, 0);
    CHECK("dup3 over the last master of its own terminal: the hangup kills the caller cleanly", waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 1);
    pid = spawn("/bin/ktest", cs3, 0, 0);
    usleep(20000);
    __syscall2(SYS_kill, pid, 9);
    CHECK("killing a process whose terminal hangup reaches itself", waitpid(pid, &status, 0) == pid && WTERMSIG(status) == 9);
    CHECK("... and the three terminals are freed", heap_used() == h0);
    int ofd = open("/dev/ptmx", O_RDWR);
    CHECK("ptys still work afterwards", ofd >= 0 && close(ofd) == 0);

    { /* the whole process table, every child parked on one pipe: pid 63 is bit 63 of the wait mask */
      static int kids[64];
      int nk = 0, maxpid = 0, full = 0, fdn = open("/dev/null", O_WRONLY);
      pipe2(in, O_CLOEXEC);
      int kmap[3] = {in[0], fdn, fdn};
      char *ck[] = {"ktest", "child-count", 0};
      for (int i = 0; i < 64; i++) {
        pid = spawn("/bin/ktest", ck, 0, kmap);
        if (pid < 0) { full = errno == EAGAIN; break; }
        kids[nk++] = pid;
        if (pid > maxpid) maxpid = pid;
      }
      usleep(100000); /* let them all reach their read */
      close(in[1]); close(in[0]); close(fdn);
      int allok = 1;
      for (int i = 0; i < nk; i++) allok &= waitpid(kids[i], &status, 0) == kids[i] && status == 0;
      CHECK("a full process table: pid 63 is used, the next spawn is EAGAIN, all wake and exit", full && maxpid == 63 && allok);
    }
    char *ls[] = {"ktest", "child-longsleep", 0};
    pid = spawn("/bin/ktest", ls, 0, 0);
    usleep(60000);
    struct procinfo pi; unsigned long long calls = 0;
    for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == pid) calls = pi.syscalls;
    __syscall2(SYS_kill, pid, 9); waitpid(pid, &status, 0);
    if (calls >= 10) printf("  (the sleeper made %llu calls in 60 ms)\n", calls);
    CHECK("a sleep longer than 24 days waits on one timer, not in a loop", calls > 0 && calls < 10);
  }

  fs_tests();

  proc_checks(); /* clone, exec and signals */
  integ_checks(); /* fork, exec and signals with the file system */

  /* informational: cost of the cheapest system call */
  t0 = now_ms();
  long sink = 0;
  for (int i = 0; i < 5000000; i++) sink += __syscall0(SYS_getppid);
  printf("  info  getppid: %.1f ns per call (%ld)\n", (now_ms() - t0) * 1e6 / 5e6, sink ? 1L : 0L);

  printf("ktest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
