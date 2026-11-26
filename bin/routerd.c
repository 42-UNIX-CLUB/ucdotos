// SPDX-License-Identifier: GPL-2.0-or-later
/* routerd: the machine's router to the world outside the page.
 *   routerd [-v]      start: give gw0 its address, make it the default route, then serve.
 *                     Without -v it leaves the terminal's session, so closing the
 *                     terminal does not stop it; with -v it logs and stays in the foreground.
 *   routerd -d        stop the running router (its pid is in /run/routerd.pid) and take
 *                     the default route down
 *
 * What it does:
 *   - gives the interface gw0 the address 10.0.2.15/24 and makes it the default route,
 *     so traffic for any address outside the machine is handed to the router;
 *   - answers DNS on 127.0.0.53:53 (A records), asking the host (/dev/hostnet), with a cache;
 *   - accepts every TCP connection to an outside address on one transparent listener, and
 *     carries it with what the host can do:
 *       a host with sockets (UC.OS under Node): a real TCP connection, any port;
 *       a browser: port 80 only, one HTTP request per connection, sent with fetch() as
 *       https. The site must allow cross-origin reads; otherwise the client gets a 502
 *       that says why. Connections to other ports are refused.
 * UDP to outside addresses (other than DNS) and ICMP cannot leave yet; the kernel answers
 * them with "host unreachable". See docs/NETWORKING.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MY_ADDR 0x0a00020fu   /* 10.0.2.15, as QEMU's user network hands out */
#define MY_MASK 0xffffff00u
#define MAX_CONN 24           /* two descriptors each; a process has 64 */
#define MAX_DNSQ 6
#define REQ_MAX (1 << 20)

static int verbose;
static unsigned caps;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static const char *ip_str(unsigned a_host, char *b) { unsigned n = htonl(a_host); return inet_ntop(AF_INET, &n, b, INET_ADDRSTRLEN); }
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
  if (!verbose) return;
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  fprintf(stderr, "routerd: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  __builtin_va_end(ap);
}
static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
static int hostnet_open(int op, const char *host, int port) {
  int fd = open("/dev/hostnet", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -1;
  struct hn_open o;
  memset(&o, 0, sizeof o);
  o.op = op; o.port = port;
  snprintf(o.host, sizeof o.host, "%s", host);
  if (ioctl(fd, HNIOC_OPEN, &o) < 0) { int e = errno; close(fd); errno = e; return -1; }
  nonblock(fd);
  return fd;
}
static int write_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) { long w = write(fd, c, n); if (w <= 0) return -1; c += w; n -= (size_t)w; }
  return 0;
}

/* ---- names: a small cache, also used to show names in the log ---- */
#define NCACHE 64
static struct { char name[256]; unsigned addr[8]; int n; double expires; } cache[NCACHE];
static int cache_find(const char *name) {
  for (int i = 0; i < NCACHE; i++) if (cache[i].n && !strcasecmp(cache[i].name, name) && cache[i].expires > now_s()) return i;
  return -1;
}
static const char *name_of(unsigned addr) {
  for (int i = 0; i < NCACHE; i++) for (int k = 0; k < cache[i].n; k++) if (cache[i].addr[k] == addr) return cache[i].name;
  return NULL;
}
static void cache_put(const char *name, const unsigned *addr, int n, int ttl) {
  int slot = 0;
  for (int i = 0; i < NCACHE; i++) if (cache[i].expires < cache[slot].expires) slot = i;
  snprintf(cache[slot].name, sizeof cache[slot].name, "%s", name);
  memcpy(cache[slot].addr, addr, (size_t)n * 4);
  cache[slot].n = n;
  cache[slot].expires = now_s() + (ttl < 30 ? 30 : ttl > 3600 ? 3600 : ttl);
}

/* ---- DNS server (RFC 1035), A records only ---- */
static int dns_fd;
static struct dnsq { int used, hfd; struct sockaddr_in from; unsigned char q[512]; int qlen; char name[256]; char out[1024]; int outlen; } dq[MAX_DNSQ];

/* Reads the question; returns its end offset, or -1. */
static int dns_question(const unsigned char *q, int len, char *name, int *type) {
  int o = 12, n = 0;
  if (len < 17 || (q[2] & 0x80) || (q[4] << 8 | q[5]) != 1) return -1;
  while (o < len && q[o]) {
    int l = q[o];
    if (l > 63 || o + 1 + l > len || n + l + 1 >= 254) return -1;
    if (n) name[n++] = '.';
    memcpy(name + n, q + o + 1, (size_t)l);
    n += l;
    o += 1 + l;
  }
  name[n] = 0;
  if (o + 5 > len || !n) return -1;
  *type = q[o + 1] << 8 | q[o + 2];
  return o + 5;
}
static void dns_reply(const struct sockaddr_in *to, const unsigned char *q, int qend, int rcode, const unsigned *addr, int n, int ttl) {
  unsigned char r[512];
  memcpy(r, q, (size_t)qend);
  r[2] = (unsigned char)(0x80 | (q[2] & 0x79));
  r[3] = (unsigned char)(0x80 | rcode); /* recursion available */
  r[6] = 0; r[7] = (unsigned char)n; r[8] = r[9] = r[10] = r[11] = 0;
  int o = qend;
  for (int i = 0; i < n && o + 16 <= (int)sizeof r; i++) {
    r[o++] = 0xc0; r[o++] = 12;                 /* the name: a pointer to the question */
    r[o++] = 0; r[o++] = 1; r[o++] = 0; r[o++] = 1;
    r[o++] = (unsigned char)(ttl >> 24); r[o++] = (unsigned char)(ttl >> 16); r[o++] = (unsigned char)(ttl >> 8); r[o++] = (unsigned char)ttl;
    r[o++] = 0; r[o++] = 4;
    memcpy(r + o, &addr[i], 4); o += 4;
  }
  sendto(dns_fd, r, (size_t)o, 0, (const struct sockaddr *)to, sizeof *to);
}
static void dns_query(void) {
  struct dnsq tmp;
  socklen_t fl = sizeof tmp.from;
  long len = recvfrom(dns_fd, tmp.q, sizeof tmp.q, 0, (struct sockaddr *)&tmp.from, &fl);
  if (len <= 0) return;
  int type, qend = dns_question(tmp.q, (int)len, tmp.name, &type);
  if (qend < 0) return;
  if (type != 1) {                              /* AAAA and the rest: no data (IPv4 only) */
    dns_reply(&tmp.from, tmp.q, qend, type == 28 ? 0 : 4, NULL, 0, 0);
    return;
  }
  int c = cache_find(tmp.name);
  if (c >= 0) { dns_reply(&tmp.from, tmp.q, qend, 0, cache[c].addr, cache[c].n, (int)(cache[c].expires - now_s())); return; }
  int slot = -1;
  for (int i = 0; i < MAX_DNSQ; i++) if (!dq[i].used) { slot = i; break; }
  int hfd = slot < 0 ? -1 : hostnet_open(HN_RESOLVE, tmp.name, 0);
  if (hfd < 0) { dns_reply(&tmp.from, tmp.q, qend, 2, NULL, 0, 0); return; } /* server failure */
  tmp.used = 1; tmp.hfd = hfd; tmp.qlen = qend; tmp.outlen = 0;
  dq[slot] = tmp;
}
static void dns_answer(struct dnsq *d) {
  long n = read(d->hfd, d->out + d->outlen, sizeof d->out - 1 - (size_t)d->outlen);
  if (n > 0) { d->outlen += (int)n; return; }
  if (n < 0 && errno == EAGAIN) return;
  int err = n < 0 ? errno : 0;
  close(d->hfd);
  d->used = 0;
  unsigned addr[8];
  int k = 0, ttl = 300;
  d->out[d->outlen] = 0;
  for (char *save, *l = strtok_r(d->out, "\n", &save); l && k < 8; l = strtok_r(NULL, "\n", &save)) {
    char ip[32];
    int t;
    if (sscanf(l, "%31s %d", ip, &t) >= 1 && inet_pton(AF_INET, ip, &addr[k]) == 1) { k++; if (t > 0 && t < ttl) ttl = t; }
  }
  if (err || !k) {
    logf_("dns %s: %s", d->name, err == ENOENT || (!err && !k) ? "no such name" : strerror(err));
    dns_reply(&d->from, d->q, d->qlen, err == ENOENT || (!err && !k) ? 3 : 2, NULL, 0, 0);
    return;
  }
  cache_put(d->name, addr, k, ttl);
  char b[INET_ADDRSTRLEN];
  logf_("dns %s -> %s%s (ttl %d)", d->name, inet_ntop(AF_INET, &addr[0], b, sizeof b), k > 1 ? " and more" : "", ttl);
  dns_reply(&d->from, d->q, d->qlen, 0, addr, k, ttl);
}

/* ---- TCP connections ---- */
enum { K_TCP = 1, K_HTTP };
static struct conn {
  int used, kind, cfd, hfd;
  unsigned src, dst;
  unsigned short sport, dport;
  char *req;              /* HTTP: the request, until it is complete */
  size_t reqlen, reqcap;
  int started, c_eof, h_eof, replied;
  unsigned char pend[16384]; /* bytes from the host waiting to be written to the client */
  size_t plen, poff;
  unsigned long long up, down;
} conns[MAX_CONN];

static void conn_close(struct conn *c, int reset) {
  if (reset) { struct linger lg = {1, 0}; setsockopt(c->cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); }
  close(c->cfd);
  if (c->hfd >= 0) close(c->hfd);
  free(c->req);
  char a[INET_ADDRSTRLEN];
  logf_("%s %s:%u closed (%llu bytes up, %llu down)", c->kind == K_TCP ? "tcp" : "http", ip_str(c->dst, a), c->dport, c->up, c->down);
  memset(c, 0, sizeof *c);
}
static void http_error(struct conn *c, int code, const char *why) {
  char body[512], msg[768];
  int bl = snprintf(body, sizeof body, "UC.OS router: %s\n", why);
  int ml = snprintf(msg, sizeof msg, "HTTP/1.1 %d %s\r\ncontent-type: text/plain\r\ncontent-length: %d\r\nconnection: close\r\n\r\n%s",
                    code, code == 502 ? "Bad Gateway" : code == 501 ? "Not Implemented" : "Bad Request", bl, body);
  write_all(c->cfd, msg, (size_t)ml);
  c->replied = 1;
}
static void on_accept(int lfd) {
  struct sockaddr_in peer, orig;
  socklen_t pl = sizeof peer, ol = sizeof orig;
  int fd = accept(lfd, (struct sockaddr *)&peer, &pl);
  if (fd < 0) return;
  getsockname(fd, (struct sockaddr *)&orig, &ol); /* transparent: where the client wanted to go */
  struct conn *c = NULL;
  for (int i = 0; i < MAX_CONN; i++) if (!conns[i].used) { c = &conns[i]; break; }
  char a[INET_ADDRSTRLEN];
  unsigned dst = ntohl(orig.sin_addr.s_addr);
  unsigned short dport = ntohs(orig.sin_port);
  ip_str(dst, a);
  const char *name = name_of(orig.sin_addr.s_addr);
  if (!c) {
    struct linger lg = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(fd);
    logf_("tcp %s:%u refused: too many connections", ip_str(dst, a), dport);
    return;
  }
  memset(c, 0, sizeof *c);
  c->used = 1; c->cfd = fd; c->hfd = -1;
  c->src = ntohl(peer.sin_addr.s_addr); c->sport = ntohs(peer.sin_port);
  c->dst = dst; c->dport = dport;
  nonblock(fd);
  if (caps & (1u << HN_TCP)) {
    c->kind = K_TCP;
    c->hfd = hostnet_open(HN_TCP, a, dport);
    if (c->hfd < 0) { conn_close(c, 1); return; }
    logf_("tcp %s:%u%s%s%s: through a host socket", a, dport, name ? " (" : "", name ? name : "", name ? ")" : "");
  } else if (dport == 80 && (caps & (1u << HN_HTTP))) {
    c->kind = K_HTTP;
    logf_("http %s:%u%s%s%s: through fetch()", a, dport, name ? " (" : "", name ? name : "", name ? ")" : "");
  } else {
    logf_("tcp %s:%u%s%s%s: reset, this host can only carry HTTP on port 80", a, dport, name ? " (" : "", name ? name : "", name ? ")" : "");
    c->kind = K_TCP;
    conn_close(c, 1);
  }
}
/* HTTP: collect one request; when it is whole, hand it to the host. */
static void http_collect(struct conn *c) {
  char buf[8192];
  long n = read(c->cfd, buf, sizeof buf);
  if (n == 0) { c->c_eof = 1; if (!c->started) conn_close(c, 0); return; }
  if (n < 0) { if (errno != EAGAIN) conn_close(c, 1); return; }
  if (c->started) return;                       /* one request per connection: ignore the rest */
  if (c->reqlen + (size_t)n > REQ_MAX) { http_error(c, 400, "request too large"); conn_close(c, 0); return; }
  if (c->reqlen + (size_t)n > c->reqcap) {
    c->reqcap = (c->reqlen + (size_t)n) * 2;
    c->req = realloc(c->req, c->reqcap + 1);
  }
  memcpy(c->req + c->reqlen, buf, (size_t)n);
  c->reqlen += (size_t)n;
  c->req[c->reqlen] = 0;
  c->up += (unsigned long long)n;
  char *end = strstr(c->req, "\r\n\r\n");
  if (!end) return;
  size_t head = (size_t)(end - c->req) + 4, body = 0;
  char host[256] = "";
  for (char *l = strstr(c->req, "\r\n"); l && l < end; l = strstr(l + 2, "\r\n")) {
    char *h = l + 2;
    if (!strncasecmp(h, "content-length:", 15)) body = (size_t)strtoul(h + 15, NULL, 10);
    else if (!strncasecmp(h, "transfer-encoding:", 18)) { http_error(c, 501, "chunked request bodies are not supported"); conn_close(c, 0); return; }
    else if (!strncasecmp(h, "host:", 5)) { sscanf(h + 5, " %255[^\r\n:]", host); }
  }
  if (c->reqlen < head + body) return;
  char a[INET_ADDRSTRLEN];
  if (!*host) snprintf(host, sizeof host, "%s", ip_str(c->dst, a));
  c->hfd = hostnet_open(HN_HTTP, host, c->dport);
  if (c->hfd < 0) { http_error(c, 502, "the host cannot make HTTP requests"); conn_close(c, 0); return; }
  char line[128];
  sscanf(c->req, "%127[^\r\n]", line);
  logf_("http %s: %s", host, line);
  write_all(c->hfd, c->req, head + body);
  ioctl(c->hfd, HNIOC_DONE, 0);
  c->started = 1;
  free(c->req); c->req = NULL; c->reqlen = c->reqcap = 0;
}
/* Client to host, for a raw TCP stream. */
static void tcp_up(struct conn *c) {
  char buf[16384];
  long n = read(c->cfd, buf, sizeof buf);
  if (n > 0) { write_all(c->hfd, buf, (size_t)n); c->up += (unsigned long long)n; return; }
  if (n < 0 && errno == EAGAIN) return;
  if (n < 0) { conn_close(c, 1); return; }
  c->c_eof = 1;
  if (c->h_eof) { conn_close(c, 0); return; }   /* both directions are done */
  ioctl(c->hfd, HNIOC_DONE, 0);                 /* the client finished sending */
}
/* Host to client. */
static void down(struct conn *c) {
  if (c->plen) return;
  long n = read(c->hfd, c->pend, sizeof c->pend);
  if (n < 0 && errno == EAGAIN) return;
  if (n <= 0) {
    int err = n < 0 ? errno : 0;
    c->h_eof = 1;
    if (c->kind == K_HTTP && err && !c->down) {
      http_error(c, 502, err == EACCES ? "the browser refused this request: the site does not allow cross-origin reads (CORS), or it could not be reached"
                                       : strerror(err));
      conn_close(c, 0);
      return;
    }
    if (c->kind == K_TCP && err) { conn_close(c, 1); return; }
    if (c->kind == K_HTTP || c->c_eof) conn_close(c, 0);
    else shutdown(c->cfd, SHUT_WR);
    return;
  }
  if (c->kind == K_HTTP && !c->down) {
    char line[128] = "";
    sscanf((char *)c->pend, "%127[^\r\n]", line);
    logf_("http answer: %s", line);
  }
  c->plen = (size_t)n; c->poff = 0;
  c->down += (unsigned long long)n;
}
static void flush(struct conn *c) {
  while (c->poff < c->plen) {
    long w = write(c->cfd, c->pend + c->poff, c->plen - c->poff);
    if (w < 0) { if (errno != EAGAIN) conn_close(c, 1); return; }
    c->poff += (size_t)w;
  }
  c->plen = c->poff = 0;
}

static int gw_index(void) {
  struct ifinfo i;
  for (int k = 0; __syscall2(SYS_ifinfo, k, &i) == 0; k++) if (!strcmp(i.name, "gw0")) return k;
  return -1;
}

int main(int argc, char **argv) {
  int down_only = 0, c;
  while ((c = getopt(argc, argv, "vd")) != -1) {
    if (c == 'v') verbose = 1;
    else if (c == 'd') down_only = 1;
    else { fprintf(stderr, "usage: routerd [-v] | routerd -d\n"); return 2; }
  }
  int gw = gw_index();
  if (gw < 0) { fprintf(stderr, "routerd: no gw0 interface in this kernel\n"); return 1; }
  if (down_only) {
    FILE *pf = fopen("/run/routerd.pid", "r");
    int pid = 0;
    if (pf) { if (fscanf(pf, "%d", &pid) != 1) pid = 0; fclose(pf); unlink("/run/routerd.pid"); }
    /* pids are reused at once: only stop it if that pid is still a routerd */
    struct procinfo pi;
    int is_router = 0;
    for (int i = 0; pid > 1 && __syscall2(SYS_procinfo, i, &pi) == 0; i++)
      if (pi.pid == pid && pi.state != 2 && !strcmp(pi.name, "routerd")) is_router = 1;
    if (is_router && pid != getpid() && kill(pid, SIGTERM) == 0) {
      int st;
      waitpid(pid, &st, WNOHANG);
      printf("routerd: stopped pid %d\n", pid);
    }
    __syscall3(SYS_ifctl, gw, IFCTL_DEFAULT, 0);
    __syscall3(SYS_ifctl, gw, IFCTL_UP, 0);
    __syscall3(SYS_ifctl, gw, IFCTL_ADDR, 0);
    __syscall3(SYS_ifctl, gw, IFCTL_MASK, 0);
    printf("routerd: gw0 down, no address, no default route\n");
    return 0;
  }
  int probe = open("/dev/hostnet", O_RDWR);
  if (probe < 0 || ioctl(probe, HNIOC_CAPS, &caps) < 0) { fprintf(stderr, "routerd: /dev/hostnet: %s\n", strerror(errno)); return 1; }
  close(probe);

  dns_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(0x7f000035);
  if (bind(dns_fd, (struct sockaddr *)&a, sizeof a) < 0) { fprintf(stderr, "routerd: DNS on 127.0.0.53:53: %s (is routerd already running?)\n", strerror(errno)); return 1; }
  nonblock(dns_fd);
  int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
  setsockopt(lfd, IPPROTO_IP, IP_TRANSPARENT, &one, sizeof one);
  if (!(caps & (1u << HN_TCP))) {                /* only HTTP can leave: other ports are refused at once */
    int http = 80;
    setsockopt(lfd, IPPROTO_IP, IP_TRANSPARENT_PORT, &http, sizeof http);
  }
  if (listen(lfd, 64) < 0) { fprintf(stderr, "routerd: listen: %s\n", strerror(errno)); return 1; }
  nonblock(lfd);

  if (!verbose) setsid();                        /* a daemon: not part of any terminal's session */
  mkdir("/run", 0755);
  FILE *pf = fopen("/run/routerd.pid", "w");
  if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

  /* Address the machine's side of the link, and route everything else through it. */
  __syscall3(SYS_ifctl, gw, IFCTL_ADDR, MY_ADDR);
  __syscall3(SYS_ifctl, gw, IFCTL_MASK, MY_MASK);
  __syscall3(SYS_ifctl, gw, IFCTL_DEFAULT, 1);
  __syscall3(SYS_ifctl, gw, IFCTL_UP, 1);
  printf("routerd: gw0 10.0.2.15/24 is the default route; DNS on 127.0.0.53\n");
  printf("routerd: this host can carry: DNS%s%s\n", caps & (1u << HN_TCP) ? ", TCP to any port" : "",
         !(caps & (1u << HN_TCP)) && (caps & (1u << HN_HTTP)) ? ", HTTP on port 80 (sent as https through fetch, CORS rules apply)" : "");
  fflush(stdout);

  for (;;) {
    struct pollfd p[2 + MAX_DNSQ + 2 * MAX_CONN];
    void *who[2 + MAX_DNSQ + 2 * MAX_CONN];
    int n = 0;
    p[n] = (struct pollfd){lfd, POLLIN, 0}; who[n++] = NULL;
    p[n] = (struct pollfd){dns_fd, POLLIN, 0}; who[n++] = NULL;
    for (int i = 0; i < MAX_DNSQ; i++) if (dq[i].used) { p[n] = (struct pollfd){dq[i].hfd, POLLIN, 0}; who[n++] = &dq[i]; }
    for (int i = 0; i < MAX_CONN; i++) {
      struct conn *k = &conns[i];
      if (!k->used) continue;
      short ce = (short)((k->c_eof ? 0 : POLLIN) | (k->plen ? POLLOUT : 0));
      p[n] = (struct pollfd){k->cfd, ce, 0}; who[n++] = k;
      if (k->hfd >= 0 && !k->plen && !k->h_eof) { p[n] = (struct pollfd){k->hfd, POLLIN, 0}; who[n++] = k; }
    }
    if (poll(p, (unsigned long)n, -1) < 0) continue;
    if (p[0].revents & POLLIN) on_accept(lfd);
    if (p[1].revents & POLLIN) dns_query();
    for (int i = 2; i < n; i++) {
      if (!p[i].revents || !who[i]) continue;
      if ((struct dnsq *)who[i] >= dq && (struct dnsq *)who[i] < dq + MAX_DNSQ) {
        struct dnsq *d = who[i];
        if (d->used && d->hfd == p[i].fd) dns_answer(d);
        continue;
      }
      struct conn *k = who[i];
      if (!k->used) continue;
      if (p[i].fd == k->cfd && k->c_eof && (k->h_eof || k->kind == K_HTTP) && !k->plen && (p[i].revents & (POLLHUP | POLLERR))) {
        conn_close(k, 0);                         /* nothing left to do in either direction */
        continue;
      }
      if (p[i].fd == k->cfd) {
        if (p[i].revents & POLLOUT) flush(k);
        if (k->used && (p[i].revents & (POLLIN | POLLHUP | POLLERR)) && !k->c_eof) {
          if (k->kind == K_HTTP) http_collect(k); else tcp_up(k);
        }
      } else if (p[i].fd == k->hfd) {
        down(k);
        if (k->used && k->plen) flush(k);
      }
    }
  }
}
