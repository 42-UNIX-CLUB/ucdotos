// SPDX-License-Identifier: GPL-2.0-or-later
/* POSIX odds and ends: ioctl and termios, system information,
 * process groups, getopt, libgen, select, locale. */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <locale.h>
#include <libgen.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/select.h>

int ioctl(int fd, unsigned long req, ...) {
  va_list ap; va_start(ap, req); long arg = va_arg(ap, long); va_end(ap);
  return __sysret(__syscall3(SYS_ioctl, fd, req, arg));
}

/* ---- termios ---- */
int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TCGETS, t); }
int tcsetattr(int fd, int act, const struct termios *t) {
  if (act < TCSANOW || act > TCSAFLUSH) { errno = EINVAL; return -1; }
  return ioctl(fd, TCSETS + act, t);
}
void cfmakeraw(struct termios *t) {
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag = (t->c_cflag & ~(CSIZE | PARENB)) | CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}
speed_t cfgetispeed(const struct termios *t) { (void)t; return B38400; }
speed_t cfgetospeed(const struct termios *t) { (void)t; return B38400; }
int cfsetispeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
int cfsetspeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
/* No ioctl for these yet: succeed on a tty, like a line with nothing queued. */
static int tty_ok(int fd) { struct termios t; return tcgetattr(fd, &t); }
int tcflush(int fd, int q) { (void)q; return tty_ok(fd); }
int tcdrain(int fd) { return tty_ok(fd); }
int tcflow(int fd, int a) { (void)a; return tty_ok(fd); }
int tcsendbreak(int fd, int d) { (void)d; return tty_ok(fd); }
pid_t tcgetpgrp(int fd) { int pg; return ioctl(fd, TIOCGPGRP, &pg) < 0 ? -1 : pg; }
int tcsetpgrp(int fd, pid_t pg) { return ioctl(fd, TIOCSPGRP, &pg); }
pid_t tcgetsid(int fd) { return tcgetpgrp(fd); }

/* ---- process groups ---- */
int setpgid(pid_t pid, pid_t pg) { return __sysret(__syscall2(SYS_setpgid, pid, pg)); }
pid_t getpgid(pid_t pid) { return __sysret(__syscall1(SYS_getpgid, pid)); }
pid_t getpgrp(void) { return getpgid(0); }
pid_t setsid(void) { return __sysret(__syscall0(SYS_setsid)); }
uid_t geteuid(void) { return __syscall0(SYS_geteuid); }
gid_t getgid(void) { return __syscall0(SYS_getgid); }
gid_t getegid(void) { return __syscall0(SYS_getegid); }

/* signals: libc/src/signal.c */

/* ---- system information ---- */
int uname(struct utsname *u) { return __sysret(__syscall1(SYS_uname, u)); }
int sysinfo(struct sysinfo *si) { return __sysret(__syscall1(SYS_sysinfo, si)); }
int get_nprocs(void) { return 1; }
int get_nprocs_conf(void) { return 1; }
int gethostname(char *buf, size_t n) {
  struct utsname u;
  if (uname(&u) < 0) return -1;
  if (strlen(u.nodename) >= n) { errno = ENAMETOOLONG; return -1; }
  strcpy(buf, u.nodename);
  return 0;
}
int getpagesize(void) { return 65536; } /* the WebAssembly page */
long sysconf(int name) {
  switch (name) {
  case _SC_ARG_MAX: return ARG_MAX;
  case _SC_CHILD_MAX: return 63;
  case _SC_CLK_TCK: return 100;
  case _SC_OPEN_MAX: return OPEN_MAX;
  case _SC_PAGESIZE: return 65536;
  case _SC_NPROCESSORS_CONF: case _SC_NPROCESSORS_ONLN: return 1;
  case _SC_PHYS_PAGES: { struct sysinfo si; return sysinfo(&si) < 0 ? -1 : (long)(si.totalram / 65536); }
  }
  errno = EINVAL;
  return -1;
}

/* ---- getopt ---- */
char *optarg;
int optind = 1, opterr = 1, optopt;
int getopt(int argc, char *const argv[], const char *opts) {
  static int pos; /* index inside a bundle like -abc */
  if (optind == 0) optind = 1, pos = 0; /* glibc-style reset */
  if (*opts == '+' || *opts == '-') opts++;
  int colon = *opts == ':';
  optarg = 0;
  if (!pos) {
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1]) return -1;
    if (!strcmp(argv[optind], "--")) { optind++; return -1; }
    pos = 1;
  }
  char *arg = argv[optind];
  int c = (unsigned char)arg[pos++];
  const char *spec = c == ':' ? 0 : strchr(opts, c);
  if (!spec) {
    optopt = c;
    if (!arg[pos]) optind++, pos = 0;
    if (opterr && !colon) fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
    return '?';
  }
  if (spec[1] == ':') {
    if (arg[pos]) optarg = arg + pos;
    else if (spec[2] != ':') {
      if (optind + 1 >= argc) {
        optopt = c; optind++; pos = 0;
        if (opterr && !colon) fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
        return colon ? ':' : '?';
      }
      optarg = argv[++optind];
    }
    optind++; pos = 0;
  } else if (!arg[pos]) optind++, pos = 0;
  return c;
}

/* ---- libgen ---- */
char *basename(char *p) {
  if (!p || !*p) return ".";
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/') p[--n] = 0;
  char *s = strrchr(p, '/');
  return s && s[1] ? s + 1 : p;
}
char *dirname(char *p) {
  if (!p || !*p) return ".";
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/') n--;
  while (n && p[n - 1] != '/') n--;
  if (!n) return ".";
  while (n > 1 && p[n - 1] == '/') n--;
  p[n] = 0;
  return p;
}

/* ---- select, on top of ppoll ---- */
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
  struct pollfd pf[FD_SETSIZE];
  int n = 0;
  if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
  for (int fd = 0; fd < nfds; fd++) {
    short ev = (rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0);
    if (ev || (ex && FD_ISSET(fd, ex))) { pf[n].fd = fd; pf[n].events = ev; pf[n].revents = 0; n++; }
  }
  int ms = tv ? (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000) : -1;
  int r = poll(pf, n, ms);
  if (r < 0) return -1;
  if (rd) FD_ZERO(rd);
  if (wr) FD_ZERO(wr);
  if (ex) FD_ZERO(ex);
  int count = 0;
  for (int i = 0; i < n; i++) {
    int fd = pf[i].fd;
    short re = pf[i].revents;
    if (rd && (re & (POLLIN | POLLHUP | POLLERR))) FD_SET(fd, rd), count++;
    if (wr && (re & (POLLOUT | POLLERR))) FD_SET(fd, wr), count++;
  }
  if (tv) tv->tv_sec = tv->tv_usec = 0;
  return count;
}

/* ---- locale: only "C" ---- */
char *setlocale(int cat, const char *name) {
  (void)cat;
  if (!name || !*name || !strcmp(name, "C") || !strcmp(name, "POSIX")) return "C";
  return 0;
}
struct lconv *localeconv(void) {
  static struct lconv c = {".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127,
                           127, 127, 127, 127, 127, 127};
  return &c;
}
