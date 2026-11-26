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

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child-exit")) return atoi(argv[2]);
  if (argc > 1 && !strcmp(argv[1], "child-crash")) { volatile int *p = (int *)0x7ffffff0; return *p; }
  if (argc > 1 && !strcmp(argv[1], "child-upper")) {
    int c;
    while ((c = getchar()) != EOF) putchar(c >= 'a' && c <= 'z' ? c - 32 : c);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "child-sleeper")) { sleep(100); return 0; }
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

  /* informational: cost of the cheapest system call */
  t0 = now_ms();
  long sink = 0;
  for (int i = 0; i < 5000000; i++) sink += __syscall0(SYS_getppid);
  printf("  info  getppid: %.1f ns per call (%ld)\n", (now_ms() - t0) * 1e6 / 5e6, sink ? 1L : 0L);

  printf("ktest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
