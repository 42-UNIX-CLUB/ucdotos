// SPDX-License-Identifier: GPL-2.0-or-later
/* nc: read and write across a TCP or UDP connection.
 *   nc [-N] [-u] [-v] [-w secs] host port     connect, then copy stdin to the socket and the socket to stdout
 *   nc -l [-N] [-u] [-v] [-k] [host] port     wait for one connection (-k: keep accepting, one at a time)
 *   nc -z [-v] host port[-port]               report which TCP ports accept a connection
 * nc ends when the other side closes. When stdin ends, the connection stays open (as
 * OpenBSD nc); -N half-closes it then (shutdown), which some servers wait for and some
 * web servers answer by closing without a reply. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int verbose, udp, half_close, timeout_ms = -1;
static void usage(void) {
  fprintf(stderr, "usage: nc [-N] [-u] [-v] [-w secs] host port\n"
                  "       nc -l [-N] [-u] [-v] [-k] [host] port\n"
                  "       nc -z [-v] host port[-port]\n");
  exit(2);
}
static int resolve(const char *host, unsigned short port, struct sockaddr_in *out) {
  struct addrinfo hints = {0}, *r;
  hints.ai_family = AF_INET;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  hints.ai_flags = host ? 0 : AI_PASSIVE;
  char svc[8];
  snprintf(svc, sizeof svc, "%u", port);
  int e = getaddrinfo(host, svc, &hints, &r);
  if (e) { fprintf(stderr, "nc: %s: %s\n", host ? host : "*", gai_strerror(e)); return -1; }
  *out = *(struct sockaddr_in *)r->ai_addr;
  freeaddrinfo(r);
  return 0;
}
static int parse_port(const char *s) {
  char *end;
  long v = strtol(s, &end, 10);
  return (*end || v < 1 || v > 65535) ? -1 : (int)v;
}
static const char *addr_str(const struct sockaddr_in *a) {
  static char b[INET_ADDRSTRLEN];
  return inet_ntop(AF_INET, &a->sin_addr, b, sizeof b);
}
static int write_all(int fd, const char *p, long n) {
  while (n > 0) {
    long w = write(fd, p, (size_t)n);
    if (w < 0) return -1;
    p += w; n -= w;
  }
  return 0;
}

/* Copy stdin to the socket and the socket to stdout until the socket ends. */
static int relay(int s) {
  static char buf[16384];
  int in_open = 1;
  for (;;) {
    struct pollfd p[2] = {{s, POLLIN, 0}, {0, in_open ? POLLIN : 0, 0}};
    int r = poll(p, in_open ? 2 : 1, timeout_ms);
    if (r == 0) return 0;                            /* -w: idle too long */
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      long n = read(s, buf, sizeof buf);
      if (n < 0) { fprintf(stderr, "nc: %s\n", strerror(errno)); return 1; }
      if (n == 0) return 0;
      if (write_all(1, buf, n)) return 1;
    }
    if (in_open && (p[1].revents & (POLLIN | POLLHUP))) {
      long n = read(0, buf, sizeof buf);
      if (n <= 0) {
        in_open = 0;
        if (udp) return 0;
        if (half_close) shutdown(s, SHUT_WR);
      } else if (write_all(s, buf, n)) { fprintf(stderr, "nc: %s\n", strerror(errno)); return 1; }
    }
  }
}

static int scan(const char *host, const char *ports) {
  int lo, hi;
  const char *dash = strchr(ports, '-');
  if (dash) {
    char a[8] = {0};
    if (dash - ports >= (long)sizeof a) usage();
    memcpy(a, ports, (size_t)(dash - ports));
    lo = parse_port(a); hi = parse_port(dash + 1);
  } else lo = hi = parse_port(ports);
  if (lo < 0 || hi < lo) usage();
  int open_any = 0;
  for (int port = lo; port <= hi; port++) {
    struct sockaddr_in a;
    if (resolve(host, (unsigned short)port, &a)) return 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
      open_any = 1;
      if (verbose) fprintf(stderr, "Connection to %s %d port [tcp/*] succeeded!\n", host, port);
    } else if (verbose) fprintf(stderr, "nc: connect to %s port %d (tcp) failed: %s\n", host, port, strerror(errno));
    close(s);
  }
  return open_any ? 0 : 1;
}

int main(int argc, char **argv) {
  int listen_mode = 0, zero = 0, keep = 0, c;
  while ((c = getopt(argc, argv, "lukvzNw:")) != -1) {
    switch (c) {
    case 'l': listen_mode = 1; break;
    case 'u': udp = 1; break;
    case 'k': keep = 1; break;
    case 'v': verbose = 1; break;
    case 'z': zero = 1; break;
    case 'N': half_close = 1; break;
    case 'w': timeout_ms = atoi(optarg) * 1000; break;
    default: usage();
    }
  }
  int nargs = argc - optind;
  if (zero) { if (nargs != 2) usage(); return scan(argv[optind], argv[optind + 1]); }
  if (listen_mode ? (nargs < 1 || nargs > 2) : nargs != 2) usage();
  const char *host = nargs == 2 ? argv[optind] : NULL;
  int port = parse_port(argv[argc - 1]);
  if (port < 0) { fprintf(stderr, "nc: invalid port: %s\n", argv[argc - 1]); return 2; }
  struct sockaddr_in a;
  if (resolve(host, (unsigned short)port, &a)) return 1;
  int s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
  if (s < 0) { perror("nc: socket"); return 1; }

  if (!listen_mode) {
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
      if (verbose || errno != ECONNREFUSED) fprintf(stderr, "nc: connect to %s port %d (%s) failed: %s\n", host, port, udp ? "udp" : "tcp", strerror(errno));
      return 1;
    }
    if (verbose) fprintf(stderr, "Connection to %s %d port [%s/*] succeeded!\n", host, port, udp ? "udp" : "tcp");
    return relay(s);
  }

  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { fprintf(stderr, "nc: bind: %s\n", strerror(errno)); return 1; }
  if (verbose) fprintf(stderr, "Listening on %s %d\n", addr_str(&a), port);
  if (udp) {                                         /* the first datagram decides who we talk to */
    static char buf[65536];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    long n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) { perror("nc: recvfrom"); return 1; }
    if (verbose) fprintf(stderr, "Connection received on %s %d\n", addr_str(&from), ntohs(from.sin_port));
    write_all(1, buf, n);
    connect(s, (struct sockaddr *)&from, sizeof from);
    return relay(s);
  }
  if (listen(s, 1) < 0) { fprintf(stderr, "nc: listen: %s\n", strerror(errno)); return 1; }
  int rc;
  do {
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int cfd = accept(s, (struct sockaddr *)&from, &fl);
    if (cfd < 0) { perror("nc: accept"); return 1; }
    if (verbose) fprintf(stderr, "Connection received on %s %d\n", addr_str(&from), ntohs(from.sin_port));
    rc = relay(cfd);
    close(cfd);
  } while (keep);
  return rc;
}
