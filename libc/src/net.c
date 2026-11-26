// SPDX-License-Identifier: GPL-2.0-or-later
/* Sockets, address conversion and name lookup. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

int socket(int d, int t, int p) { return (int)__sysret(__syscall3(SYS_socket, d, t, p)); }
int bind(int fd, const struct sockaddr *a, socklen_t l) { return (int)__sysret(__syscall3(SYS_bind, fd, a, l)); }
int listen(int fd, int b) { return (int)__sysret(__syscall2(SYS_listen, fd, b)); }
int accept(int fd, struct sockaddr *a, socklen_t *l) { return (int)__sysret(__syscall3(SYS_accept, fd, a, l)); }
int connect(int fd, const struct sockaddr *a, socklen_t l) { return (int)__sysret(__syscall3(SYS_connect, fd, a, l)); }
ssize_t sendto(int fd, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t l) {
  return __sysret(__syscall6(SYS_sendto, fd, (long)b, (long)n, f, (long)a, (long)l));
}
ssize_t recvfrom(int fd, void *b, size_t n, int f, struct sockaddr *a, socklen_t *l) {
  return __sysret(__syscall6(SYS_recvfrom, fd, (long)b, (long)n, f, (long)a, (long)l));
}
ssize_t send(int fd, const void *b, size_t n, int f) { return sendto(fd, b, n, f, NULL, 0); }
ssize_t recv(int fd, void *b, size_t n, int f) { return recvfrom(fd, b, n, f, NULL, NULL); }
int setsockopt(int fd, int lv, int o, const void *v, socklen_t l) {
  return (int)__sysret(__syscall6(SYS_setsockopt, fd, lv, o, (long)v, (long)l, 0));
}
int getsockopt(int fd, int lv, int o, void *v, socklen_t *l) {
  return (int)__sysret(__syscall6(SYS_getsockopt, fd, lv, o, (long)v, (long)l, 0));
}
int shutdown(int fd, int how) { return (int)__sysret(__syscall2(SYS_shutdown, fd, how)); }
int getsockname(int fd, struct sockaddr *a, socklen_t *l) { return (int)__sysret(__syscall3(SYS_getsockname, fd, a, l)); }
int getpeername(int fd, struct sockaddr *a, socklen_t *l) { return (int)__sysret(__syscall3(SYS_getpeername, fd, a, l)); }

/* ---- addresses ---- */
int inet_pton(int af, const char *s, void *dst) {
  if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
  unsigned char b[4];
  for (int i = 0; i < 4; i++) {
    if (*s < '0' || *s > '9') return 0;
    unsigned v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      if (digits && v == 0) return 0;          /* no leading zeros, as glibc */
      v = v * 10 + (unsigned)(*s++ - '0');
      if (++digits > 3 || v > 255) return 0;
    }
    b[i] = (unsigned char)v;
    if (i < 3 && *s++ != '.') return 0;
  }
  if (*s) return 0;
  memcpy(dst, b, 4);
  return 1;
}
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
  if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
  const unsigned char *b = src;
  char tmp[INET_ADDRSTRLEN];
  int n = snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  if ((socklen_t)n >= size) { errno = ENOSPC; return NULL; }
  memcpy(dst, tmp, (size_t)n + 1);
  return dst;
}
int inet_aton(const char *s, struct in_addr *out) { return inet_pton(AF_INET, s, &out->s_addr) == 1; }
in_addr_t inet_addr(const char *s) {
  struct in_addr a;
  return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}
char *inet_ntoa(struct in_addr a) {
  static char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &a.s_addr, buf, sizeof buf);
  return buf;
}

/* ---- names ---- */
/* Find name in /etc/hosts. Returns 1 and the address (network order) if found. */
static int hosts_lookup(const char *name, unsigned *addr) {
  FILE *f = fopen("/etc/hosts", "r");
  int found = 0;
  if (f) {
    char line[256];
    while (!found && fgets(line, sizeof line, f)) {
      char *hash = strchr(line, '#');
      if (hash) *hash = 0;
      char *save, *tok = strtok_r(line, " \t\r\n", &save);
      unsigned a;
      if (!tok || inet_pton(AF_INET, tok, &a) != 1) continue;
      while ((tok = strtok_r(NULL, " \t\r\n", &save)))
        if (!strcasecmp(tok, name)) { *addr = a; found = 1; break; }
    }
    fclose(f);
  }
  if (!found && !strcasecmp(name, "localhost")) { *addr = htonl(INADDR_LOOPBACK); found = 1; }
  return found;
}
/* A DNS query for A records (RFC 1035) to the name server in /etc/resolv.conf (default
 * 127.0.0.53, where the router answers). Returns the number of addresses found (network
 * order), 0 if the name does not exist, -1 if no server answered. */
static int dns_server(unsigned *addr) {
  *addr = htonl(0x7f000035);
  FILE *f = fopen("/etc/resolv.conf", "r");
  if (!f) return 0;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char ip[64];
    if (sscanf(line, " nameserver %63s", ip) == 1 && inet_pton(AF_INET, ip, addr) == 1) break;
  }
  fclose(f);
  return 0;
}
static int dns_lookup(const char *name, unsigned *out, int max) {
  unsigned char q[300], r[1500];
  size_t nl = strlen(name);
  if (nl == 0 || nl > 253) return 0;
  unsigned short id = (unsigned short)(__syscall0(SYS_getpid) * 7919 + nl);
  int n = 0;
  q[n++] = (unsigned char)(id >> 8); q[n++] = (unsigned char)id;
  q[n++] = 1; q[n++] = 0;                      /* recursion desired */
  q[n++] = 0; q[n++] = 1; memset(q + n, 0, 6); n += 6;
  for (const char *p = name; *p;) {
    const char *dot = strchr(p, '.');
    size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (l == 0 || l > 63) { if (!l && !dot) break; return 0; }
    q[n++] = (unsigned char)l;
    memcpy(q + n, p, l); n += (int)l;
    p += l + (dot ? 1 : 0);
    if (!dot) break;
  }
  q[n++] = 0;
  q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1; /* type A, class IN */
  unsigned server;
  dns_server(&server);
  struct sockaddr_in to = {0};
  to.sin_family = AF_INET; to.sin_port = htons(53); to.sin_addr.s_addr = server;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) { close(fd); return -1; }
  int found = -1;
  for (int attempt = 0; attempt < 2 && found < 0; attempt++) {
    if (send(fd, q, (size_t)n, 0) < 0) break;
    struct pollfd pf = {fd, POLLIN, 0};
    if (poll(&pf, 1, 2500) <= 0) continue;
    long len = recv(fd, r, sizeof r, 0);
    if (len < 12 || r[0] != q[0] || r[1] != q[1] || !(r[2] & 0x80)) { if (len < 0) break; continue; }
    int rcode = r[3] & 15;
    if (rcode == 3) { found = 0; break; }      /* no such name */
    if (rcode != 0) break;
    int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
    long o = 12;
    for (int i = 0; i < qd && o < len; i++) {  /* skip the question */
      while (o < len && r[o] && (r[o] & 0xc0) != 0xc0) o += r[o] + 1;
      o += (o < len && (r[o] & 0xc0) == 0xc0) ? 2 : 1;
      o += 4;
    }
    found = 0;
    for (int i = 0; i < an && o + 10 <= len && found < max; i++) {
      while (o < len && r[o] && (r[o] & 0xc0) != 0xc0) o += r[o] + 1;
      o += (o < len && (r[o] & 0xc0) == 0xc0) ? 2 : 1;
      if (o + 10 > len) break;
      int type = r[o] << 8 | r[o + 1], rdlen = r[o + 8] << 8 | r[o + 9];
      o += 10;
      if (type == 1 && rdlen == 4 && o + 4 <= len) memcpy(&out[found++], r + o, 4);
      o += rdlen;
    }
  }
  close(fd);
  return found;
}
static struct addrinfo *ai_new(unsigned addr, unsigned short port, int socktype, int proto, const char *canon) {
  struct addrinfo *ai = calloc(1, sizeof *ai + sizeof(struct sockaddr_in));
  if (!ai) return NULL;
  struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(port);
  sin->sin_addr.s_addr = addr;
  ai->ai_family = AF_INET;
  ai->ai_socktype = socktype;
  ai->ai_protocol = proto;
  ai->ai_addrlen = sizeof *sin;
  ai->ai_addr = (struct sockaddr *)sin;
  if (canon) ai->ai_canonname = strdup(canon);
  return ai;
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
  int flags = hints ? hints->ai_flags : 0, family = hints ? hints->ai_family : AF_UNSPEC;
  int socktype = hints ? hints->ai_socktype : 0, proto = hints ? hints->ai_protocol : 0;
  if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
  if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM && socktype != SOCK_RAW) return EAI_SOCKTYPE;
  if (!node && !service) return EAI_NONAME;
  unsigned short port = 0;
  if (service && *service) {
    char *end;
    long v = strtol(service, &end, 10);
    if (*end || v < 0 || v > 65535) return EAI_SERVICE;
    port = (unsigned short)v;
  }
  unsigned addrs[8];
  int naddr = 1;
  if (!node) addrs[0] = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
  else if (inet_pton(AF_INET, node, &addrs[0]) == 1) {}
  else if (flags & AI_NUMERICHOST) return EAI_NONAME;
  else if (!hosts_lookup(node, &addrs[0])) {
    naddr = dns_lookup(node, addrs, 8);
    if (naddr < 0) return EAI_AGAIN;
    if (naddr == 0) return EAI_NONAME;
  }
  const char *canon = (flags & AI_CANONNAME) && node ? node : NULL;
  struct addrinfo *head = NULL, **tail = &head;
  static const int types[][2] = {{SOCK_STREAM, IPPROTO_TCP}, {SOCK_DGRAM, IPPROTO_UDP}};
  for (int a = 0; a < naddr; a++) {
    for (int i = 0; i < 2; i++) {
      if (socktype && socktype != types[i][0]) continue;
      struct addrinfo *ai = ai_new(addrs[a], port, types[i][0], proto ? proto : types[i][1], canon);
      if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
      canon = NULL;
      *tail = ai;
      tail = &ai->ai_next;
    }
    if (socktype == SOCK_RAW) {
      struct addrinfo *ai = ai_new(addrs[a], port, SOCK_RAW, proto, canon);
      if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
      canon = NULL;
      *tail = ai;
      tail = &ai->ai_next;
    }
  }
  *res = head;
  return 0;
}
void freeaddrinfo(struct addrinfo *ai) {
  while (ai) {
    struct addrinfo *n = ai->ai_next;
    free(ai->ai_canonname);
    free(ai);
    ai = n;
  }
}
const char *gai_strerror(int code) {
  switch (code) {
  case 0: return "Success";
  case EAI_NONAME: return "Name or service not known";
  case EAI_SERVICE: return "Servname not supported for ai_socktype";
  case EAI_FAMILY: return "ai_family not supported";
  case EAI_SOCKTYPE: return "ai_socktype not supported";
  case EAI_MEMORY: return "Memory allocation failure";
  case EAI_AGAIN: return "Temporary failure in name resolution";
  }
  return "Unknown error";
}
struct hostent *gethostbyname(const char *name) {
  static unsigned addr;
  static char *list[2] = {(char *)&addr, NULL};
  static char *aliases[1] = {NULL};
  static char hname[256];
  static struct hostent h = {hname, aliases, AF_INET, 4, list};
  if (inet_pton(AF_INET, name, &addr) != 1 && !hosts_lookup(name, &addr) && dns_lookup(name, &addr, 1) < 1) return NULL;
  snprintf(hname, sizeof hname, "%s", name);
  return &h;
}
