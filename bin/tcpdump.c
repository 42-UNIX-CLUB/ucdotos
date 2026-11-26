// SPDX-License-Identifier: GPL-2.0-or-later
/* tcpdump: print every packet the machine sends, through a packet socket.
 *   tcpdump [-c count] [-x] [-q] [expression]
 * The expression is a list of terms joined by "and": tcp, udp, icmp, port N, host A.
 * Sequence numbers are printed in full (like tcpdump -S). Times are UTC. Without -c it
 * runs until Ctrl-C, which ends it without a summary (UC.OS delivers no signals). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int want_proto, want_port = -1, quiet, hex;
static unsigned want_host;
static int have_host;

static unsigned rd16(const unsigned char *p) { return (unsigned)(p[0] << 8 | p[1]); }
static unsigned rd32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static void usage(void) {
  fprintf(stderr, "usage: tcpdump [-c count] [-x] [-q] [tcp|udp|icmp] [and port N] [and host A]\n");
  exit(2);
}
static void parse(int argc, char **argv) {
  for (int i = 0; i < argc; i++) {
    const char *w = argv[i];
    if (!strcmp(w, "and")) continue;
    if (!strcmp(w, "tcp")) want_proto = IPPROTO_TCP;
    else if (!strcmp(w, "udp")) want_proto = IPPROTO_UDP;
    else if (!strcmp(w, "icmp")) want_proto = IPPROTO_ICMP;
    else if (!strcmp(w, "port") && i + 1 < argc) want_port = atoi(argv[++i]);
    else if (!strcmp(w, "host") && i + 1 < argc) {
      if (inet_pton(AF_INET, argv[++i], &want_host) != 1) { fprintf(stderr, "tcpdump: bad host %s\n", argv[i]); exit(2); }
      want_host = ntohl(want_host);
      have_host = 1;
    } else usage();
  }
}
static const char *ip_str(unsigned a, char *b) { unsigned n = htonl(a); return inet_ntop(AF_INET, &n, b, INET_ADDRSTRLEN); }

static void tcp_line(const unsigned char *h, unsigned len) {
  unsigned doff = (unsigned)(h[12] >> 4) * 4, fl = h[13];
  char f[8], *p = f;
  if (fl & 2) *p++ = 'S';
  if (fl & 1) *p++ = 'F';
  if (fl & 4) *p++ = 'R';
  if (fl & 8) *p++ = 'P';
  if (fl & 16) *p++ = '.';
  *p = 0;
  unsigned dlen = len > doff ? len - doff : 0, seq = rd32(h + 4);
  printf("Flags [%s]", f);
  if (dlen) printf(", seq %u:%u", seq, seq + dlen);
  else if (fl & 7) printf(", seq %u", seq);
  if (fl & 16) printf(", ack %u", rd32(h + 8));
  printf(", win %u", rd16(h + 14));
  for (unsigned i = 20; i + 3 < doff; ) {
    if (h[i] == 0) break;
    if (h[i] == 1) { i++; continue; }
    if (h[i] == 2 && h[i + 1] == 4) printf(", options [mss %u]", rd16(h + i + 2));
    if (h[i + 1] < 2) break;
    i += h[i + 1];
  }
  printf(", length %u", dlen);
}
static void icmp_line(const unsigned char *m, unsigned len) {
  printf("ICMP ");
  switch (m[0]) {
  case 0: printf("echo reply, id %u, seq %u", rd16(m + 4), rd16(m + 6)); break;
  case 8: printf("echo request, id %u, seq %u", rd16(m + 4), rd16(m + 6)); break;
  case 3: {
    static const char *why[] = {"net", "host", "protocol", "port"};
    printf("%s unreachable", m[1] < 4 ? why[m[1]] : "destination");
    if (m[1] == 3 && len >= 8 + 20 + 4) {
      const unsigned char *o = m + 8;
      unsigned ihl = (unsigned)(o[0] & 15) * 4;
      if (len >= 8 + ihl + 4 && o[9] == IPPROTO_UDP) {
        char b[INET_ADDRSTRLEN];
        printf(" (udp port %u on %s)", rd16(o + ihl + 2), ip_str(rd32(o + 16), b));
      }
    }
    break;
  }
  default: printf("type %u, code %u", m[0], m[1]);
  }
  printf(", length %u", len);
}

int main(int argc, char **argv) {
  long count = -1;
  int c;
  while ((c = getopt(argc, argv, "c:xqn")) != -1) {
    switch (c) {
    case 'c': count = atol(optarg); break;
    case 'x': hex = 1; break;
    case 'q': quiet = 1; break;
    case 'n': break;
    default: usage();
    }
  }
  parse(argc - optind, argv + optind);
  int fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
  if (fd < 0) { perror("tcpdump: socket"); return 1; }
  fprintf(stderr, "tcpdump: listening on lo, capture size 65535 bytes\n");
  static unsigned char buf[65536];
  long shown = 0;
  while (count < 0 || shown < count) {
    long n = recv(fd, buf, sizeof buf, 0);
    if (n < 20) continue;
    unsigned ihl = (unsigned)(buf[0] & 15) * 4, total = rd16(buf + 2), proto = buf[9];
    if (ihl < 20 || total > (unsigned)n || total < ihl) continue;
    unsigned src = rd32(buf + 12), dst = rd32(buf + 16);
    const unsigned char *t = buf + ihl;
    unsigned tlen = total - ihl;
    unsigned sport = 0, dport = 0;
    if ((proto == IPPROTO_TCP || proto == IPPROTO_UDP) && tlen >= 4) { sport = rd16(t); dport = rd16(t + 2); }
    if (want_proto && (int)proto != want_proto) continue;
    if (want_port >= 0 && (int)sport != want_port && (int)dport != want_port) continue;
    if (have_host && src != want_host && dst != want_host) continue;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long day = (long)(ts.tv_sec % 86400);
    printf("%02ld:%02ld:%02ld.%06ld IP ", day / 3600, day / 60 % 60, day % 60, ts.tv_nsec / 1000);
    char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
    if (sport || dport) printf("%s.%u > %s.%u: ", ip_str(src, a), sport, ip_str(dst, b), dport);
    else printf("%s > %s: ", ip_str(src, a), ip_str(dst, b));
    if (quiet) printf("%s, length %u", proto == IPPROTO_TCP ? "tcp" : proto == IPPROTO_UDP ? "UDP" : proto == IPPROTO_ICMP ? "ICMP" : "ip", tlen);
    else if (proto == IPPROTO_TCP && tlen >= 20) tcp_line(t, tlen);
    else if (proto == IPPROTO_UDP && tlen >= 8) printf("UDP, length %u", rd16(t + 4) - 8);
    else if (proto == IPPROTO_ICMP && tlen >= 8) icmp_line(t, tlen);
    else printf("ip-proto-%u %u", proto, tlen);
    printf("\n");
    if (hex) {
      for (unsigned i = 0; i < total; i += 16) {
        printf("\t0x%04x: ", i);
        for (unsigned j = i; j < i + 16 && j < total; j += 2) printf(j + 1 < total ? " %02x%02x" : " %02x", buf[j], buf[j + 1]);
        printf("\n");
      }
    }
    fflush(stdout);
    shown++;
  }
  fprintf(stderr, "%ld packets captured\n", shown);
  return 0;
}
