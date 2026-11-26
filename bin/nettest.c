// SPDX-License-Identifier: GPL-2.0-or-later
/* Network stack self-test, over the loopback interface. One process plays both ends,
 * with non-blocking sockets and poll, so a bug shows up as a FAIL, not a hang.
 *   node tools/run.mjs --build=B --init=/bin/nettest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static struct sockaddr_in sin_of(unsigned addr, unsigned short port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(addr);
  return a;
}
#define SA(a) ((struct sockaddr *)&(a))
#define LO INADDR_LOOPBACK
static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
static int wait_fd(int fd, short ev, int ms) {
  struct pollfd p = {fd, ev, 0};
  return poll(&p, 1, ms) == 1 ? p.revents : 0;
}
static int tcp_listener(unsigned short port, int reuse) {
  int l = socket(AF_INET, SOCK_STREAM, 0);
  if (reuse) setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  struct sockaddr_in a = sin_of(LO, port);
  if (bind(l, SA(a), sizeof a) < 0 || listen(l, 8) < 0) { close(l); return -1; }
  return l;
}
/* A connected pair through listener l: c is the client end, s the accepted end. */
static int tcp_pair(int l, unsigned short port, int *c, int *s) {
  *c = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = sin_of(LO, port);
  if (connect(*c, SA(a), sizeof a) < 0) return -1;
  if (!(wait_fd(l, POLLIN, 3000) & POLLIN)) return -1;
  *s = accept(l, NULL, NULL);
  return *s < 0 ? -1 : 0;
}
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + (i >> 8) * 13); }
/* Move n bytes from a to b with both ends non-blocking. Returns bytes that arrived intact. */
static long pump(int a, int b, long n, int ms) {
  static unsigned char buf[16384];
  long sent = 0, got = 0, bad = 0;
  double end = now_ms() + ms;
  nonblock(a); nonblock(b);
  while (got < n && now_ms() < end) {
    struct pollfd p[2] = {{a, sent < n ? POLLOUT : 0, 0}, {b, POLLIN, 0}};
    poll(p, 2, 200);
    if (sent < n && (p[0].revents & POLLOUT)) {
      long k = n - sent > (long)sizeof buf ? (long)sizeof buf : n - sent;
      for (long i = 0; i < k; i++) buf[i] = pattern((unsigned)(sent + i));
      long w = write(a, buf, (size_t)k);
      if (w > 0) sent += w;
    }
    if (p[1].revents & POLLIN) {
      long r = read(b, buf, sizeof buf);
      if (r <= 0) break;
      for (long i = 0; i < r; i++) if (buf[i] != pattern((unsigned)(got + i))) bad++;
      got += r;
    }
  }
  return bad ? -bad : got;
}
static int sockinfo_for(unsigned short lport, unsigned short rport, struct sockinfo *out) {
  for (int i = 0; __syscall2(SYS_netstat, i, out) == 0; i++)
    if (out->proto == IPPROTO_TCP && out->lport == lport && out->rport == rport) return 1;
  return 0;
}
static unsigned short local_port(int fd) {
  struct sockaddr_in a;
  socklen_t l = sizeof a;
  getsockname(fd, SA(a), &l);
  return ntohs(a.sin_port);
}

static void test_basics(void) {
  struct ifinfo i;
  CHECK("lo is interface 0", __syscall2(SYS_ifinfo, 0, &i) == 0 && !strcmp(i.name, "lo"));
  CHECK("lo is up, 127.0.0.1/8, MTU 65535", (i.flags & IFF_UP) && i.addr == 0x7f000001 && i.mask == 0xff000000 && i.mtu == 65535);
  CHECK("gw0 is interface 1, down, no address", __syscall2(SYS_ifinfo, 1, &i) == 0 && !strcmp(i.name, "gw0") && !(i.flags & IFF_UP) && !i.addr);
  CHECK("no third interface", __syscall2(SYS_ifinfo, 2, &i) == -ESRCH);
  unsigned a;
  char s[16];
  CHECK("inet_pton 10.0.0.42", inet_pton(AF_INET, "10.0.0.42", &a) == 1 && a == htonl(0x0a00002a));
  CHECK("inet_pton rejects 1.2.3", inet_pton(AF_INET, "1.2.3", &a) == 0 && inet_pton(AF_INET, "1.2.3.256", &a) == 0);
  CHECK("inet_ntop", inet_ntop(AF_INET, &a, s, sizeof s) && !strcmp(s, "10.0.0.42"));
  struct addrinfo h = {0}, *r = NULL;
  h.ai_socktype = SOCK_STREAM;
  int e = getaddrinfo("localhost", "8080", &h, &r);
  CHECK("getaddrinfo localhost:8080", e == 0 && r && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(LO) &&
        ((struct sockaddr_in *)r->ai_addr)->sin_port == htons(8080) && !r->ai_next);
  if (r) freeaddrinfo(r);
  CHECK("getaddrinfo without a DNS server fails at once (EAI_AGAIN)", getaddrinfo("no.such.host", "1", NULL, &r) == EAI_AGAIN);
  CHECK("socket(AF_INET6) is refused", socket(10, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);
  struct sockaddr_in far = sin_of(0x08080808, 53);
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK("no route beyond loopback", sendto(u, "x", 1, 0, SA(far), sizeof far) < 0 && errno == ENETUNREACH);
  close(u);
}

static void test_udp(void) {
  int srv = socket(AF_INET, SOCK_DGRAM, 0), cli = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = sin_of(LO, 7000), from;
  CHECK("udp bind 127.0.0.1:7000", bind(srv, SA(a), sizeof a) == 0);
  int s2 = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK("udp second bind on 7000 is EADDRINUSE", bind(s2, SA(a), sizeof a) < 0 && errno == EADDRINUSE);
  close(s2);
  CHECK("udp sendto", sendto(cli, "hello", 5, 0, SA(a), sizeof a) == 5);
  char buf[70000];
  socklen_t fl = sizeof from;
  int ok = (wait_fd(srv, POLLIN, 2000) & POLLIN) && recvfrom(srv, buf, sizeof buf, 0, SA(from), &fl) == 5 && !memcmp(buf, "hello", 5);
  CHECK("udp datagram arrives", ok);
  CHECK("udp source is the client's port", from.sin_addr.s_addr == htonl(LO) && ntohs(from.sin_port) == local_port(cli) && local_port(cli) >= 49152);
  CHECK("udp reply", sendto(srv, "world", 5, 0, SA(from), fl) == 5 && (wait_fd(cli, POLLIN, 2000) & POLLIN) && recv(cli, buf, 100, 0) == 5 && !memcmp(buf, "world", 5));
  memset(buf, 'z', sizeof buf);
  CHECK("udp 65507 bytes is the largest", sendto(cli, buf, 65507, 0, SA(a), sizeof a) == 65507);
  CHECK("udp 65508 bytes is EMSGSIZE", sendto(cli, buf, 65508, 0, SA(a), sizeof a) < 0 && errno == EMSGSIZE);
  CHECK("udp big datagram arrives whole", (wait_fd(srv, POLLIN, 2000) & POLLIN) && recv(srv, buf, sizeof buf, 0) == 65507);
  CHECK("udp truncates to the buffer", sendto(cli, "abcdef", 6, 0, SA(a), sizeof a) == 6 && (wait_fd(srv, POLLIN, 2000) & POLLIN) &&
        recv(srv, buf, 3, 0) == 3 && !memcmp(buf, "abc", 3) && wait_fd(srv, POLLIN, 0) == 0);
  nonblock(srv);
  CHECK("udp empty non-blocking recv is EAGAIN", recv(srv, buf, 10, 0) < 0 && errno == EAGAIN);
  close(srv);
  /* A connected socket hears the ICMP port unreachable as ECONNREFUSED. */
  struct sockaddr_in closed = sin_of(LO, 7001);
  CHECK("udp connect", connect(cli, SA(closed), sizeof closed) == 0);
  struct sockaddr_in peer;
  socklen_t pl = sizeof peer;
  CHECK("udp getpeername", getpeername(cli, SA(peer), &pl) == 0 && ntohs(peer.sin_port) == 7001);
  CHECK("udp send to a closed port", send(cli, "?", 1, 0) == 1);
  int r = (wait_fd(cli, POLLIN, 2000) & POLLERR) ? (int)recv(cli, buf, 10, 0) : 0;
  CHECK("udp closed port gives ECONNREFUSED", r < 0 && errno == ECONNREFUSED);
  close(cli);
}

static void test_tcp_basic(void) {
  int c, s;
  struct sockaddr_in a = sin_of(LO, 9);
  c = socket(AF_INET, SOCK_STREAM, 0);
  CHECK("tcp connect to a closed port is refused", connect(c, SA(a), sizeof a) < 0 && errno == ECONNREFUSED);
  close(c);
  int l = tcp_listener(8000, 0);
  CHECK("tcp listen on 8000", l >= 0);
  int l2 = tcp_listener(8000, 0);
  CHECK("tcp second listener on 8000 is EADDRINUSE", l2 < 0 && errno == EADDRINUSE);
  nonblock(l);
  CHECK("tcp accept with nothing waiting is EAGAIN", accept(l, NULL, NULL) < 0 && errno == EAGAIN);
  CHECK("tcp connect and accept", tcp_pair(l, 8000, &c, &s) == 0);
  struct sockaddr_in x, y;
  socklen_t xl = sizeof x, yl = sizeof y;
  getsockname(c, SA(x), &xl);
  getpeername(s, SA(y), &yl);
  CHECK("tcp names agree on both ends", x.sin_port == y.sin_port && x.sin_addr.s_addr == htonl(LO) && y.sin_addr.s_addr == htonl(LO));
  xl = sizeof x;
  getpeername(c, SA(x), &xl);
  CHECK("tcp client's peer is port 8000", ntohs(x.sin_port) == 8000);
  struct sockinfo si;
  CHECK("netstat shows the connection established", sockinfo_for(local_port(c), 8000, &si) && si.state == TCP_ESTABLISHED && si.mss == 65495);
  CHECK("tcp write", write(c, "ping", 4) == 4);
  char buf[64];
  CHECK("tcp read", (wait_fd(s, POLLIN, 2000) & POLLIN) && read(s, buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4));
  CHECK("tcp answer", write(s, "pong!", 5) == 5 && (wait_fd(c, POLLIN, 2000) & POLLIN) && read(c, buf, sizeof buf) == 5);
  /* Half close: the client is done sending, the server still answers. */
  CHECK("tcp shutdown write", shutdown(c, SHUT_WR) == 0);
  CHECK("tcp peer reads end of stream", (wait_fd(s, POLLIN, 2000) & POLLIN) && read(s, buf, sizeof buf) == 0);
  CHECK("tcp write after shutdown is EPIPE", write(c, "x", 1) < 0 && errno == EPIPE);
  CHECK("tcp half-closed peer can still send", write(s, "late", 4) == 4 && (wait_fd(c, POLLIN, 2000) & POLLIN) && read(c, buf, sizeof buf) == 4);
  close(s);
  CHECK("tcp client sees the end", (wait_fd(c, POLLIN, 2000) & POLLIN) && read(c, buf, sizeof buf) == 0);
  unsigned short cport = local_port(c);
  close(c);
  /* The client sent the first FIN, so its side of the connection waits in TIME_WAIT. */
  double t0 = now_ms();
  int tw = 0;
  while (!tw && now_ms() - t0 < 2000) { tw = sockinfo_for(cport, 8000, &si) && si.state == TCP_TIME_WAIT; if (!tw) usleep(5000); }
  CHECK("tcp active closer waits in TIME_WAIT", tw);
  close(l);
}

static void test_tcp_bulk(void) {
  int l = tcp_listener(8001, 1), c, s;
  CHECK("tcp bulk: connect", l >= 0 && tcp_pair(l, 8001, &c, &s) == 0);
  double t0 = now_ms();
  long n = pump(c, s, 4 << 20, 20000);
  double ms = now_ms() - t0;
  CHECK("tcp 4 MiB arrive intact, in order", n == 4 << 20);
  printf("        (%.0f ms, %.1f MiB/s)\n", ms, ms > 0 ? 4 * 1000.0 / ms : 0);
  CHECK("tcp 1 MiB the other way", pump(s, c, 1 << 20, 20000) == 1 << 20);
  /* Fill the receiver: nobody reads, so the window closes and the writer must stop. */
  static char blob[16384];
  long sent = 0, w;
  for (;;) {                         /* write until a write would block and stays blocked */
    while ((w = write(c, blob, sizeof blob)) > 0) sent += w;
    if (w < 0 && errno == EAGAIN && (wait_fd(c, POLLOUT, 300) & POLLOUT)) continue;
    break;
  }
  CHECK("tcp writer stops at a closed window (EAGAIN)", w < 0 && errno == EAGAIN && sent == 65535 + 65536);
  struct sockinfo si;
  CHECK("tcp peer's window is zero", sockinfo_for(local_port(c), 8001, &si) && si.snd_wnd == 0);
  long got = 0, r;
  double end = now_ms() + 5000;
  while (got < sent && now_ms() < end) {
    if (wait_fd(s, POLLIN, 200) & POLLIN) { r = read(s, blob, sizeof blob); if (r > 0) got += r; }
  }
  CHECK("tcp everything arrives once the reader drains", got == sent);
  close(c); close(s); close(l);
}

static void test_tcp_nonblocking(void) {
  int l = tcp_listener(8002, 1);
  int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  struct sockaddr_in a = sin_of(LO, 8002);
  CHECK("non-blocking connect is EINPROGRESS", connect(c, SA(a), sizeof a) < 0 && errno == EINPROGRESS);
  CHECK("second connect is EALREADY", connect(c, SA(a), sizeof a) < 0 && errno == EALREADY);
  CHECK("connect completes: POLLOUT", wait_fd(c, POLLOUT, 2000) & POLLOUT);
  int err = -1;
  socklen_t el = sizeof err;
  CHECK("SO_ERROR is 0", getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
  CHECK("connect again is EISCONN", connect(c, SA(a), sizeof a) < 0 && errno == EISCONN);
  close(c);
  close(accept(l, NULL, NULL));      /* the server end of that connection */
  c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  a = sin_of(LO, 8003);
  connect(c, SA(a), sizeof a);
  int ev = wait_fd(c, POLLOUT, 2000);
  err = 0;
  getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
  CHECK("non-blocking connect to a closed port: POLLERR, SO_ERROR ECONNREFUSED", (ev & POLLERR) && err == ECONNREFUSED);
  close(c);
  /* Closing with unread data resets the connection (RFC 2525). */
  int s;
  CHECK("reset: connect", tcp_pair(l, 8002, &c, &s) == 0);
  write(c, "unread", 6);
  wait_fd(s, POLLIN, 2000);
  close(s);
  char buf[16];
  int r = (wait_fd(c, POLLIN, 2000) & POLLIN) ? (int)read(c, buf, sizeof buf) : 1;
  CHECK("close with unread data: peer gets ECONNRESET", r < 0 && errno == ECONNRESET);
  close(c);
  /* Many short connections. */
  int okn = 0;
  for (int i = 0; i < 20; i++) {
    if (tcp_pair(l, 8002, &c, &s) == 0 && write(c, "x", 1) == 1 && (wait_fd(s, POLLIN, 2000) & POLLIN) && read(s, buf, 1) == 1) okn++;
    close(s); close(c);
  }
  CHECK("20 connections in a row", okn == 20);
  close(l);
  /* The server closed first above, so its port has connections in TIME_WAIT. */
  int b = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in p = sin_of(LO, 8002);
  int one = 1;
  CHECK("rebinding a port in TIME_WAIT needs SO_REUSEADDR", bind(b, SA(p), sizeof p) < 0 && errno == EADDRINUSE);
  setsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  CHECK("with SO_REUSEADDR it binds", bind(b, SA(p), sizeof p) == 0);
  close(b);
}

static void test_lossy(void) {
  __syscall3(SYS_ifctl, 0, IFCTL_MTU, 1500);
  __syscall3(SYS_ifctl, 0, IFCTL_LOSS, 50);   /* 5 % of packets lost */
  __syscall3(SYS_ifctl, 0, IFCTL_DELAY, 5);
  int l = tcp_listener(8004, 1), c = -1, s = -1;
  int conn = 0;
  for (int i = 0; i < 3 && !conn; i++) { conn = tcp_pair(l, 8004, &c, &s) == 0; if (!conn) { close(c); } }
  CHECK("lossy: connect (MTU 1500, 5% loss, 5 ms delay)", conn);
  struct sockinfo si;
  CHECK("lossy: MSS follows the MTU", conn && sockinfo_for(local_port(c), 8004, &si) && si.mss == 1460);
  long n = conn ? pump(c, s, 512 << 10, 60000) : 0;
  CHECK("lossy: 512 KiB arrive intact", n == 512 << 10);
  CHECK("lossy: the sender retransmitted", sockinfo_for(local_port(c), 8004, &si) && si.retransmits > 0);
  printf("        (retransmits %u, srtt %u us, rto %u ms, cwnd %u)\n", si.retransmits, si.srtt_us, si.rto_ms, si.cwnd);
  struct ifinfo i;
  __syscall2(SYS_ifinfo, 0, &i);
  CHECK("lossy: lo counted dropped packets", i.dropped > 0);
  close(c); close(s); close(l);
  __syscall3(SYS_ifctl, 0, IFCTL_LOSS, 0);
  __syscall3(SYS_ifctl, 0, IFCTL_DELAY, 0);
  __syscall3(SYS_ifctl, 0, IFCTL_MTU, 65535);
}

static void test_raw_and_packet(void) {
  int pk = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
  CHECK("packet socket", pk >= 0);
  int r = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  CHECK("raw ICMP socket", r >= 0);
  unsigned char m[16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'u', 'c', '.', 'o', 's', 0, 0, 0};
  unsigned sum = 0;
  for (int i = 0; i < 16; i += 2) sum += (unsigned)(m[i] << 8 | m[i + 1]);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  m[2] = (unsigned char)(~sum >> 8); m[3] = (unsigned char)~sum;
  struct sockaddr_in a = sin_of(LO, 0);
  CHECK("raw: echo request sent", sendto(r, m, sizeof m, 0, SA(a), sizeof a) == sizeof m);
  unsigned char buf[256];
  int reply = 0;
  double end = now_ms() + 2000;
  while (!reply && now_ms() < end && (wait_fd(r, POLLIN, 500) & POLLIN)) {
    long n = recv(r, buf, sizeof buf, 0);
    int ihl = (buf[0] & 15) * 4;
    if (n == ihl + 16 && buf[ihl] == 0 && buf[ihl + 4] == 0x12 && buf[ihl + 7] == 1 && !memcmp(buf + ihl + 8, "uc.os", 5)) reply = 1;
  }
  CHECK("raw: echo reply with the same id, sequence and data", reply);
  int saw_icmp = 0;
  while (wait_fd(pk, POLLIN, 0) & POLLIN) {
    struct sockaddr_ll ll;
    socklen_t ln = sizeof ll;
    long n = recvfrom(pk, buf, sizeof buf, 0, SA(ll), &ln);
    if (n >= 20 && buf[9] == IPPROTO_ICMP && ll.sll_family == AF_PACKET && ll.sll_ifindex == 1) saw_icmp++;
  }
  CHECK("packet socket saw the request and the reply", saw_icmp == 2);
  CHECK("packet socket cannot send", send(pk, buf, 20, 0) < 0 && errno == EOPNOTSUPP);
  close(r); close(pk);
}

/* A one-shot DNS server on 127.0.0.53 (run as a child): example.test is 10.9.8.7,
 * every other name does not exist. */
static int fake_dns(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = sin_of(0x7f000035, 53), from;
  if (bind(fd, SA(a), sizeof a) < 0) return 1;
  for (int k = 0; k < 2; k++) {
    unsigned char q[512];
    socklen_t fl = sizeof from;
    long n = recvfrom(fd, q, sizeof q, 0, SA(from), &fl);
    if (n < 17) return 1;
    int found = memmem(q, (size_t)n, "\7example\4test", 13) != NULL;
    q[2] = (unsigned char)(0x80 | (q[2] & 0x79)); q[3] = found ? 0x80 : 0x83;
    q[7] = (unsigned char)found;
    unsigned char ans[16] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 9, 8, 7};
    if (found) { memcpy(q + n, ans, 16); n += 16; }
    sendto(fd, q, (size_t)n, 0, SA(from), fl);
  }
  return 0;
}
static void test_dns_client(void) {
  char *argv[] = {"nettest", "fake-dns", NULL};
  pid_t pid = spawn("/bin/nettest", argv, NULL, NULL);
  usleep(200000);
  struct addrinfo h = {0}, *r = NULL;
  h.ai_socktype = SOCK_STREAM;
  int e = getaddrinfo("example.test", "80", &h, &r);
  CHECK("resolver: a name from the DNS server", e == 0 && r && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(0x0a090807));
  if (r) freeaddrinfo(r);
  CHECK("resolver: NXDOMAIN is EAI_NONAME", getaddrinfo("nothing.test", "80", &h, &r) == EAI_NONAME);
  int st;
  waitpid(pid, &st, 0);
}

static int gw_ctl(int op, long v) { return (int)__syscall3(SYS_ifctl, 1, op, v); }
static void test_router_kernel(void) {
  int c = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in far = sin_of(0x5db8d822, 80); /* 93.184.216.34 */
  CHECK("router off: an outside address is ENETUNREACH", connect(c, SA(far), sizeof far) < 0 && errno == ENETUNREACH);
  close(c);
  CHECK("gw0 takes an address and the default route", gw_ctl(IFCTL_ADDR, 0x0a00020f) == 0 && gw_ctl(IFCTL_MASK, (long)0xffffff00u) == 0 &&
        gw_ctl(IFCTL_DEFAULT, 1) == 0 && gw_ctl(IFCTL_UP, 1) == 0);
  CHECK("lo cannot be given an address", __syscall3(SYS_ifctl, 0, IFCTL_ADDR, 0x0a000001) == -EPERM);
  c = socket(AF_INET, SOCK_STREAM, 0);
  CHECK("no router listening: an outside address is refused", connect(c, SA(far), sizeof far) < 0 && errno == ECONNREFUSED);
  close(c);
  int local80 = tcp_listener(80, 1);
  int t = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  CHECK("IP_TRANSPARENT on a listener", setsockopt(t, IPPROTO_IP, IP_TRANSPARENT, &one, sizeof one) == 0 && listen(t, 8) == 0);
  c = socket(AF_INET, SOCK_STREAM, 0);
  CHECK("connect to an outside address reaches the router", connect(c, SA(far), sizeof far) == 0);
  int s = (wait_fd(t, POLLIN, 2000) & POLLIN) ? accept(t, NULL, NULL) : -1;
  CHECK("the transparent listener accepts it", s >= 0);
  CHECK("a local listener on port 80 does not answer for outside addresses", wait_fd(local80, POLLIN, 0) == 0);
  struct sockaddr_in o, me;
  socklen_t ol = sizeof o, ml = sizeof me;
  getsockname(s, SA(o), &ol);
  getsockname(c, SA(me), &ml);
  CHECK("getsockname gives the original destination", o.sin_addr.s_addr == htonl(0x5db8d822) && ntohs(o.sin_port) == 80);
  CHECK("the client's address is gw0's", me.sin_addr.s_addr == htonl(0x0a00020f));
  char buf[16];
  CHECK("data both ways through the router's socket", write(c, "GET", 3) == 3 && (wait_fd(s, POLLIN, 2000) & POLLIN) && read(s, buf, 16) == 3 &&
        write(s, "ok", 2) == 2 && (wait_fd(c, POLLIN, 2000) & POLLIN) && read(c, buf, 16) == 2);
  struct linger lg = {1, 0};
  setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  close(s);
  int r = (wait_fd(c, POLLIN, 2000) & POLLIN) ? (int)read(c, buf, sizeof buf) : 1;
  CHECK("SO_LINGER 0: close resets the connection", r < 0 && errno == ECONNRESET);
  close(c);
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in dns = sin_of(0x08080808, 53);
  connect(u, SA(dns), sizeof dns);
  send(u, "?", 1, 0);
  r = (wait_fd(u, POLLIN, 2000) & POLLERR) ? (int)recv(u, buf, sizeof buf, 0) : 1;
  CHECK("UDP to an outside address: host unreachable", r < 0 && errno == EHOSTUNREACH);
  close(u);
  int port80 = 80;
  CHECK("IP_TRANSPARENT_PORT limits the router to one port", setsockopt(t, IPPROTO_IP, IP_TRANSPARENT_PORT, &port80, sizeof port80) == 0);
  struct sockaddr_in ssh = sin_of(0x5db8d822, 22);
  c = socket(AF_INET, SOCK_STREAM, 0);
  CHECK("another port is refused at once", connect(c, SA(ssh), sizeof ssh) < 0 && errno == ECONNREFUSED);
  close(c);
  close(t); close(local80);
  int hn = open("/dev/hostnet", O_RDWR);
  unsigned caps = 0;
  CHECK("/dev/hostnet reports what the host can do", hn >= 0 && ioctl(hn, HNIOC_CAPS, &caps) == 0 && (caps & (1u << HN_RESOLVE)) && (caps & (1u << HN_HTTP)));
  close(hn);
  gw_ctl(IFCTL_DEFAULT, 0); gw_ctl(IFCTL_UP, 0); gw_ctl(IFCTL_ADDR, 0); gw_ctl(IFCTL_MASK, 0);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "fake-dns")) return fake_dns();
  printf("nettest: network stack over loopback\n");
  test_basics();
  test_udp();
  test_tcp_basic();
  test_tcp_bulk();
  test_tcp_nonblocking();
  test_raw_and_packet();
  test_dns_client();
  test_router_kernel();
  test_lossy();
  printf("nettest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
