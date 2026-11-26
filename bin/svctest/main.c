// SPDX-License-Identifier: GPL-2.0-or-later
/* svctest: a stand-in daemon for the service manager's tests (rootfs/usr/share/tests/svc/).
 *   svctest run MS [CODE]       print "running PID", sleep MS (0 = forever), exit CODE
 *   svctest notify MS           sleep MS, send READY=1 and STATUS= to NOTIFY_SOCKET, sleep forever
 *   svctest children N          start N children ("svctest run 0"), sleep forever
 *   svctest daemonize PIDFILE   start one child that sleeps forever, write its pid, exit 0 (Type=forking)
 *   svctest listen              accept on the socket passed as fd 3 (LISTEN_FDS), answer "echo: LINE", exit
 *   svctest client PATH TEXT    connect, send TEXT, print the answer
 *   svctest lines N             print N lines
 *   svctest mem MIB             allocate and touch MIB mebibytes, sleep forever
 *   svctest append FILE TEXT    append TEXT and a newline to FILE
 *   svctest now                 print the monotonic clock in ms
 * Under any other name (a real daemon's path in a Debian root) it acts as a generic
 * daemon: "-t"/"configtest" exits 0 at once; a first argument that is not an option
 * (logrotate /etc/logrotate.conf) means a one-off job: print and exit 0; otherwise
 * READY=1 is sent if NOTIFY_SOCKET is set and it runs until it is stopped. */
#include "../../libsvc/os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void msleep(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
}
static void forever(void) { for (;;) msleep(3600000); }
static int notify(const char *msg) {
  const char *s = getenv("NOTIFY_SOCKET");
  if (!s) return -1;
  int fd = svc_connect_unix(s);
  if (fd < 0) return -1;
  char b[512];
  int n = snprintf(b, sizeof b, "%sSENDER_PID=%d\n", msg, getpid());
  int ok = write(fd, b, n) == n;
  close(fd);
  return ok ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *me = strrchr(argv[0], '/');
  me = me ? me + 1 : argv[0];
  if (strcmp(me, "svctest")) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "configtest") || !strcmp(argv[i], "-T")) return 0;
    if (argc > 1 && argv[1][0] != '-') { printf("%s (stand-in) did its job with %s\n", me, argv[1]); return 0; }
    printf("%s (stand-in) running as pid %d\n", me, getpid());
    fflush(stdout);
    if (getenv("NOTIFY_SOCKET")) notify("READY=1\nSTATUS=stand-in ready\n");
    forever();
  }
  if (argc < 2) { fprintf(stderr, "usage: svctest MODE ...\n"); return 2; }
  const char *m = argv[1];
  if (!strcmp(m, "run")) {
    long ms = argc > 2 ? atol(argv[2]) : 0;
    printf("running %d\n", getpid());
    fflush(stdout);
    if (ms <= 0 && !(argc > 3)) forever();
    if (ms > 0) msleep(ms);
    return argc > 3 ? atoi(argv[3]) : 0;
  }
  if (!strcmp(m, "notify")) {
    msleep(argc > 2 ? atol(argv[2]) : 0);
    if (notify("READY=1\nSTATUS=serving\n") < 0) { fprintf(stderr, "svctest: cannot notify\n"); return 1; }
    printf("ready\n");
    fflush(stdout);
    forever();
  }
  if (!strcmp(m, "children") || !strcmp(m, "daemonize")) {
    int n = !strcmp(m, "children") ? (argc > 2 ? atoi(argv[2]) : 1) : 1;
    extern char **environ;
    int last = 0;
    for (int i = 0; i < n; i++) {
      char *av[] = {"/bin/svctest", "run", "0", 0};
      last = svc_spawn("/bin/svctest", av, environ, 0, 0, 0, -1);
      printf("child %d\n", last);
    }
    fflush(stdout);
    if (!strcmp(m, "daemonize")) {
      if (argc > 2) {
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char b[16];
        int l = snprintf(b, sizeof b, "%d\n", last);
        if (fd >= 0) { if (write(fd, b, l) < 0) {} close(fd); }
      }
      return 0;
    }
    forever();
  }
  if (!strcmp(m, "listen")) {
    const char *lf = getenv("LISTEN_FDS");
    if (!lf || atoi(lf) < 1) { fprintf(stderr, "svctest: no LISTEN_FDS\n"); return 1; }
    int c = svc_accept(3, 0);
    if (c < 0) { fprintf(stderr, "svctest: accept: %d\n", c); return 1; }
    char b[256];
    ssize_t r = read(c, b, sizeof b - 1);
    if (r < 0) r = 0;
    b[r] = 0;
    char o[300];
    int n = snprintf(o, sizeof o, "echo: %s", b);
    if (write(c, o, n) < 0) {}
    close(c);
    printf("served one connection\n");
    return 0;
  }
  if (!strcmp(m, "client") && argc > 3) {
    int fd = svc_connect_unix(argv[2]);
    if (fd < 0) { printf("connect failed %d\n", fd); return 1; }
    if (write(fd, argv[3], strlen(argv[3])) < 0) {}
    char b[300];
    int n = 0;
    ssize_t r;
    while ((r = read(fd, b + n, sizeof b - 1 - n)) > 0) n += r;
    b[n] = 0;
    printf("%s\n", b);
    close(fd);
    return 0;
  }
  if (!strcmp(m, "lines")) {
    int n = argc > 2 ? atoi(argv[2]) : 10;
    for (int i = 1; i <= n; i++) printf("line %d of %d\n", i, n);
    return 0;
  }
  if (!strcmp(m, "mem")) {
    int mib = argc > 2 ? atoi(argv[2]) : 1;
    for (int i = 0; i < mib; i++) { char *p = malloc(1 << 20); if (p) memset(p, 1, 1 << 20); }
    printf("holding %d MiB\n", mib);
    fflush(stdout);
    forever();
  }
  if (!strcmp(m, "append") && argc > 3) {
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return 1;
    char b[300];
    int n = snprintf(b, sizeof b, "%s\n", argv[3]);
    if (write(fd, b, n) < 0) {}
    close(fd);
    return 0;
  }
  if (!strcmp(m, "now")) { printf("%lld\n", (long long)svc_now_ms()); return 0; }
  fprintf(stderr, "svctest: unknown mode %s\n", m);
  return 2;
}
