// SPDX-License-Identifier: GPL-2.0-or-later
/* Router self-test: the HTTP path of routerd, end to end. Run by
 * tools/tests/router-http.mjs, which starts a local HTTP server and boots UC.OS with the
 * host in test mode (browser behaviour, every HTTP request to that server, names in
 * .test resolving to 203.0.113.10). This program starts routerd, then talks to it with
 * plain sockets, as any program would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s (errno %d: %s)\n", name, errno, strerror(errno)); } } while (0)
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

#define CAP (3 << 20)
static char resp[CAP];
static long rlen;
static int got_eof;

/* Connect to name:port, send the request (in pieces if split), read until the router
 * closes. Returns 0, or -errno if the connection failed. */
static int http(const char *name, int port, const char *req, size_t len, int split) {
  rlen = 0; got_eof = 0;
  struct addrinfo h = {0}, *r;
  h.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(name, NULL, &h, &r)) return -ENOENT;
  struct sockaddr_in a = *(struct sockaddr_in *)r->ai_addr;
  freeaddrinfo(r);
  a.sin_port = htons((unsigned short)port);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { int e = errno; close(fd); return -e; }
  size_t step = split ? len / 3 + 1 : len;
  for (size_t off = 0; off < len; off += step) {
    size_t n = len - off < step ? len - off : step;
    if (write(fd, req + off, n) != (long)n) { close(fd); return -EIO; }
    if (split) usleep(50000);
  }
  double end = now_ms() + 15000;
  while (now_ms() < end) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
    long n = read(fd, resp + rlen, (size_t)(CAP - 1 - rlen));
    if (n <= 0) { got_eof = n == 0; break; }
    rlen += n;
  }
  resp[rlen] = 0;
  close(fd);
  return 0;
}
static int status(void) { int s = 0; sscanf(resp, "HTTP/1.1 %d", &s); return s; }
static const char *body(void) { char *b = strstr(resp, "\r\n\r\n"); return b ? b + 4 : resp + rlen; }
static long body_len(void) { return rlen - (body() - resp); }
static int has_header(const char *name, const char *value) {
  const char *end = strstr(resp, "\r\n\r\n");
  size_t nl = strlen(name);
  for (const char *l = strstr(resp, "\r\n"); l && end && l < end; l = strstr(l + 2, "\r\n")) {
    if (strncasecmp(l + 2, name, nl) || l[2 + nl] != ':') continue;
    if (!value) return 1;
    const char *v = l + 3 + nl;
    while (*v == ' ') v++;
    return !strncmp(v, value, strlen(value));
  }
  return 0;
}
static int get(const char *path, const char *host) {
  char req[512];
  int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: routertest\r\n\r\n", path, host);
  return http(host, 80, req, (size_t)n, 0);
}

int main(void) {
  printf("routertest: the router's HTTP path\n");
  unsigned caps = 0;
  int hn = open("/dev/hostnet", O_RDWR);
  if (hn >= 0) { ioctl(hn, HNIOC_CAPS, &caps); close(hn); }
  if (!(caps & HNCAP_TEST)) {
    printf("routertest: skipped: the host is not in test mode, so there is no test server to talk to.\n"
           "            Run it from the build machine: node tools/tests/router-http.mjs --build=DIR\n");
    return 0;
  }
  char *argv[] = {"routerd", NULL};
  pid_t router = spawn("/bin/routerd", argv, NULL, NULL);
  CHECK("routerd starts", router > 0);
  struct addrinfo h = {0}, *r = NULL;
  h.ai_socktype = SOCK_STREAM;
  int e = -1;
  for (int i = 0; i < 50 && e; i++) { e = getaddrinfo("site.test", "80", &h, &r); if (e) usleep(100000); }
  CHECK("DNS through routerd: site.test is 203.0.113.10", e == 0 && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(0xcb00710a));
  if (r) freeaddrinfo(r);
  CHECK("DNS through routerd: missing.test does not exist", getaddrinfo("missing.test", "80", &h, &r) == EAI_NONAME);

  CHECK("GET: connected through the router", get("/hello", "site.test") == 0);
  CHECK("GET: 200 OK", status() == 200 && !strncmp(resp, "HTTP/1.1 200 OK\r\n", 17));
  CHECK("GET: the body, exactly", !strcmp(body(), "hello from the test server\n"));
  CHECK("GET: the server's headers pass through", has_header("x-test", "yes") && has_header("content-type", "text/plain"));
  CHECK("GET: connection: close, no content-length", has_header("connection", "close") && !has_header("content-length", NULL));
  CHECK("GET: the router closes after one answer", got_eof);
  CHECK("GET: the request went through fetch()", has_header("x-ucos-fetched", "http://127.0.0.1:"));

  CHECK("the Host header reaches the server", get("/host", "site.test") == 0 && !strcmp(body(), "site.test"));
  static const char split[] = "GET /hello HTTP/1.1\r\nHost: site.test\r\nAccept: */*\r\n\r\n";
  CHECK("a request written in three pieces", http("site.test", 80, split, sizeof split - 1, 1) == 0 && status() == 200 && !strcmp(body(), "hello from the test server\n"));
  static const char head[] = "HEAD /hello HTTP/1.1\r\nHost: site.test\r\n\r\n";
  CHECK("HEAD: headers and no body", http("site.test", 80, head, sizeof head - 1, 0) == 0 && status() == 200 && body_len() == 0);

  static char post[8192];
  int hl = snprintf(post, sizeof post, "POST /echo HTTP/1.1\r\nHost: site.test\r\nContent-Type: application/octet-stream\r\nContent-Length: 5000\r\n\r\n");
  for (int i = 0; i < 5000; i++) post[hl + i] = (char)(i * 31 + 7);
  CHECK("POST: 5000 bytes sent", http("site.test", 80, post, (size_t)hl + 5000, 0) == 0 && status() == 200);
  CHECK("POST: the server got them all, unchanged", body_len() == 5000 && !memcmp(body(), post + hl, 5000));

  int bytes_ok = get("/bytes", "site.test") == 0 && body_len() == 256;
  for (int i = 0; bytes_ok && i < 256; i++) if ((unsigned char)body()[i] != i) bytes_ok = 0;
  CHECK("binary body: bytes 0 to 255", bytes_ok);

  double t0 = now_ms();
  int big_ok = get("/big", "site.test") == 0 && status() == 200 && body_len() == 2 << 20;
  for (long i = 0; big_ok && i < 2 << 20; i++) if ((unsigned char)body()[i] != (unsigned char)(i * 7 + (i >> 8) * 13)) big_ok = 0;
  CHECK("2 MiB download, intact (8 times the kernel's 256 KiB queue)", big_ok);
  printf("        (%.0f ms)\n", now_ms() - t0);

  CHECK("404 passes through with its reason", get("/nothing", "site.test") == 0 && !strncmp(resp, "HTTP/1.1 404 Not Found\r\n", 24));
  CHECK("a redirect is followed, as a browser does", get("/redir", "site.test") == 0 && status() == 200 && !strcmp(body(), "hello from the test server\n"));
  CHECK("gzip: the body arrives decoded, without content-encoding",
        get("/gzip", "site.test") == 0 && !strcmp(body(), "compressed hello\n") && !has_header("content-encoding", NULL));
  CHECK("a fetch that fails: 502 that explains", get("/hello", "down.test") == 0 && status() == 502 && strstr(body(), "cross-origin"));
  static const char chunked[] = "POST /echo HTTP/1.1\r\nHost: site.test\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  CHECK("a chunked request body: 501", http("site.test", 80, chunked, sizeof chunked - 1, 0) == 0 && status() == 501);
  static const char none[] = "x";
  CHECK("port 22 is refused at once (HTTP only, as in a browser)", http("site.test", 22, none, 1, 0) == -ECONNREFUSED);

  struct procinfo pi;
  unsigned long long cpu = 0;
  for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == router) cpu = pi.cpu_us;
  CHECK("routerd sleeps between requests (under 2 s of CPU)", cpu < 2000000);
  printf("        (routerd used %.0f ms of CPU)\n", cpu / 1000.0);

  kill(router, SIGTERM);
  int st;
  waitpid(router, &st, 0);
  printf("routertest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
