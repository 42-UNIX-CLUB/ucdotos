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

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static long heap_used(void) { struct sysinfo si; return sysinfo(&si) ? -1 : (long)si.bufferram; } /* kernel heap in use */

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child-exit")) return atoi(argv[2]);
  if (argc > 1 && !strcmp(argv[1], "child-crash")) { volatile int *p = (int *)0x7ffffff0; return *p; }
  if (argc > 1 && !strcmp(argv[1], "child-upper")) {
    int c;
    while ((c = getchar()) != EOF) putchar(c >= 'a' && c <= 'z' ? c - 32 : c);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "child-sleeper")) { sleep(100); return 0; }
  if (argc > 1 && !strcmp(argv[1], "child-remote")) { /* open a remote file, expect its text */
    char b[64] = {0};
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) return 2;
    long n = read(fd, b, sizeof b - 1);
    close(fd);
    return n > 0 && !strcmp(b, argv[3]) ? 0 : 1;
  }
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
  CHECK("spawn non-program -> ENOEXEC", spawn("/etc/motd", a1, 0, 0) == -1 && errno == ENOEXEC);
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
    char name[128];
    snprintf(name, sizeof name, "/tmp/%063d", 7);
    fd = open(name, O_CREAT | O_WRONLY, 0644);
    CHECK("a 63-byte name works", fd >= 0 && stat(name, &st) == 0 && close(fd) == 0 && unlink(name) == 0);
    snprintf(name, sizeof name, "/tmp/%064d", 7);
    CHECK("a 64-byte name -> ENAMETOOLONG (it used to be cut, giving duplicates)", open(name, O_CREAT | O_WRONLY, 0644) == -1 && errno == ENAMETOOLONG);
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

  /* remote files (kernel/remote.c; test files from tools/remote-files.sh, served under Node by
   * tools/remote-node.mjs: ".once." only on the first request, ".flaky." not on the first,
   * ".slow." after 300 ms) */
  {
    const char *R = "/usr/share/tests/remote";
    struct stat rs;
    char rb[128];
    int rfd;
    #define RREAD(path, want) ((rfd = open(path, O_RDONLY)) >= 0 && (memset(rb, 0, sizeof rb), read(rfd, rb, sizeof rb - 1)) > 0 && \
                               close(rfd) == 0 && !strcmp(rb, want))
    if (stat("/etc/remote-files", &rs) != 0) printf("  info  no /etc/remote-files in this image: remote file checks skipped\n");
    else {
      int listed = 0;
      DIR *d = opendir(R);
      for (struct dirent *e; d && (e = readdir(d));) listed += !strcmp(e->d_name, "hello.once.txt") || !strcmp(e->d_name, "never.txt");
      if (d) closedir(d);
      CHECK("remote: listed before any fetch", listed == 2);
      CHECK("remote: stat before the fetch has the real size, no blocks",
            stat("/usr/share/tests/remote/hello.once.txt", &rs) == 0 && S_ISREG(rs.st_mode) && rs.st_size == 22 && rs.st_blocks == 0);
      CHECK("remote: first open fetches it", RREAD("/usr/share/tests/remote/hello.once.txt", "fetched on first open\n"));
      CHECK("remote: then it is a file in memory", stat("/usr/share/tests/remote/hello.once.txt", &rs) == 0 && rs.st_blocks == 1);
      CHECK("remote: second open does not fetch again", RREAD("/usr/share/tests/remote/hello.once.txt", "fetched on first open\n"));
      /* two processes wait for the same fetch: a second request for a .once. file would fail */
      char *ca[] = {"ktest", "child-remote", "/usr/share/tests/remote/both.once.slow.txt", "two openers, one fetch\n", 0};
      pid_t cp = spawn("/bin/ktest", ca, 0, 0);
      int waiting = 0;
      for (int tries = 0; tries < 200 && !waiting; tries++) {
        struct procinfo pi;
        for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == cp && pi.state == 3) waiting = 1;
        if (!waiting) usleep(2000);
      }
      int still = stat("/usr/share/tests/remote/both.once.slow.txt", &rs) == 0 && rs.st_blocks == 0;
      CHECK("remote: two openers at once both get it", waiting && still && RREAD("/usr/share/tests/remote/both.once.slow.txt", "two openers, one fetch\n") &&
            waitpid(cp, &status, 0) == cp && WIFEXITED(status) && WEXITSTATUS(status) == 0);
      CHECK("remote: failed fetch -> EIO", open("/usr/share/tests/remote/retry.flaky.txt", O_RDONLY) == -1 && errno == EIO);
      CHECK("remote: and a later open fetches again", RREAD("/usr/share/tests/remote/retry.flaky.txt", "second try works\n"));
      CHECK("remote: wrong SHA-256 -> EIO", open("/usr/share/tests/remote/bad.txt", O_RDONLY) == -1 && errno == EIO);
      CHECK("remote: wrong SHA-256 again on retry, file unchanged",
            open("/usr/share/tests/remote/bad.txt", O_RDONLY) == -1 && errno == EIO && stat("/usr/share/tests/remote/bad.txt", &rs) == 0 && rs.st_size == 34 && rs.st_blocks == 0);
      CHECK("remote: missing on the server -> EIO", open("/usr/share/tests/remote/missing.txt", O_RDONLY) == -1 && errno == EIO);
      int nb = open("/usr/share/tests/remote/nb.slow.txt", O_RDONLY | O_NONBLOCK), nberr = errno, polls = 0;
      while (nb < 0 && errno == EAGAIN && polls < 500) { usleep(2000); polls++; nb = open("/usr/share/tests/remote/nb.slow.txt", O_RDONLY | O_NONBLOCK); }
      CHECK("remote: O_NONBLOCK -> EAGAIN while it arrives, then opens", nberr == EAGAIN && nb >= 0 && polls > 0 && read(nb, rb, 12) == 12 && !memcmp(rb, "not waiting\n", 12));
      if (nb >= 0) close(nb);
      rfd = open("/usr/share/tests/remote/packed.txt", O_RDONLY);
      long total = 0, n;
      int same = rfd >= 0 && read(rfd, rb, 32) == 32 && !memcmp(rb, "line 0 of a file sent compressed", 32);
      while (rfd >= 0 && (n = read(rfd, rb, sizeof rb)) > 0) total += n;
      if (rfd >= 0) close(rfd);
      CHECK("remote: a file sent as .gz arrives unpacked", same && total + 32 == 70890);
      CHECK("remote: a file nobody opened was never fetched", stat("/usr/share/tests/remote/never.txt", &rs) == 0 && rs.st_size == 13 && rs.st_blocks == 0);
      rfd = open("/usr/share/tests/remote/never.txt", O_WRONLY | O_TRUNC);
      CHECK("remote: O_TRUNC for writing needs no fetch", rfd >= 0 && write(rfd, "mine\n", 5) == 5 && close(rfd) == 0 && RREAD("/usr/share/tests/remote/never.txt", "mine\n"));
    }
    #undef RREAD
    (void)R;
  }

  /* informational: cost of the cheapest system call */
  t0 = now_ms();
  long sink = 0;
  for (int i = 0; i < 5000000; i++) sink += __syscall0(SYS_getppid);
  printf("  info  getppid: %.1f ns per call (%ld)\n", (now_ms() - t0) * 1e6 / 5e6, sink ? 1L : 0L);

  printf("ktest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
