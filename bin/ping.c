// SPDX-License-Identifier: GPL-2.0-or-later
/* ping: send ICMP echo requests and time the replies, through a raw socket.
 *   ping [-c count] [-i seconds] [-s size] [-t ttl] [-W seconds] [-q] host
 * Without -c it runs until Ctrl-C, which ends it at once: UC.OS delivers no signals,
 * so there is no summary then. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static unsigned short cksum(const unsigned char *p, int n) {
  unsigned s = 0;
  for (int i = 0; i + 1 < n; i += 2) s += (unsigned)(p[i] << 8 | p[i + 1]);
  if (n & 1) s += (unsigned)p[n - 1] << 8;
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return (unsigned short)~s;
}
static void usage(void) {
  fprintf(stderr, "usage: ping [-c count] [-i seconds] [-s size] [-t ttl] [-W seconds] [-q] host\n");
  exit(2);
}

int main(int argc, char **argv) {
  long count = -1, size = 56;
  double interval = 1.0, wait_s = 10.0;
  int ttl = 64, quiet = 0, c;
  while ((c = getopt(argc, argv, "c:i:s:t:W:q")) != -1) {
    switch (c) {
    case 'c': count = atol(optarg); if (count < 1) usage(); break;
    case 'i': interval = strtod(optarg, NULL); if (interval < 0.01) interval = 0.01; break;
    case 's': size = atol(optarg); if (size < 0 || size > 65507) usage(); break;
    case 't': ttl = atoi(optarg); if (ttl < 1 || ttl > 255) usage(); break;
    case 'W': wait_s = strtod(optarg, NULL); break;
    case 'q': quiet = 1; break;
    default: usage();
    }
  }
  if (optind != argc - 1) usage();
  const char *host = argv[optind];
  struct addrinfo hints = {0}, *res;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_RAW;
  hints.ai_protocol = IPPROTO_ICMP;
  int e = getaddrinfo(host, NULL, &hints, &res);
  if (e) { fprintf(stderr, "ping: %s: %s\n", host, gai_strerror(e)); return 2; }
  struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
  freeaddrinfo(res);
  int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (fd < 0) { perror("ping: socket"); return 2; }
  setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &to.sin_addr, ip, sizeof ip);
  printf("PING %s (%s) %ld(%ld) bytes of data.\n", host, ip, size, size + 28);
  fflush(stdout);

  unsigned short id = (unsigned short)getpid();
  unsigned char *pkt = calloc(1, (size_t)size + 8), *buf = malloc(65536);
  long sent = 0, recvd = 0;
  double tmin = 1e18, tmax = 0, tsum = 0, tsum2 = 0, start = now_ms(), next = start, last_sent = start;
  for (;;) {
    double t = now_ms();
    if (t >= next && (count < 0 || sent < count)) {
      pkt[0] = 8; pkt[1] = 0; pkt[2] = pkt[3] = 0;
      pkt[4] = (unsigned char)(id >> 8); pkt[5] = (unsigned char)id;
      unsigned short seq = (unsigned short)(sent + 1);
      pkt[6] = (unsigned char)(seq >> 8); pkt[7] = (unsigned char)seq;
      if (size >= 8) memcpy(pkt + 8, &t, sizeof t);  /* the send time travels in the payload */
      for (long i = 16; i < size + 8; i++) pkt[i] = (unsigned char)i;
      unsigned short s = cksum(pkt, (int)size + 8);
      pkt[2] = (unsigned char)(s >> 8); pkt[3] = (unsigned char)s;
      if (sendto(fd, pkt, (size_t)size + 8, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        fprintf(stderr, "ping: sendto: %s\n", strerror(errno));
        return 2;
      }
      sent++;
      last_sent = t;
      next = t + interval * 1000;
    }
    if (count > 0 && sent >= count && (recvd >= count || t - last_sent >= wait_s * 1000)) break;
    double until = (count < 0 || sent < count) ? next : last_sent + wait_s * 1000;
    int ms = (int)(until - now_ms());
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms > 0 ? ms : 0) <= 0) continue;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    long n = recvfrom(fd, buf, 65536, 0, (struct sockaddr *)&from, &fl);
    if (n < 28) continue;
    int ihl = (buf[0] & 15) * 4;
    unsigned char *m = buf + ihl;
    if (n >= ihl + 8 + 20 + 8 && m[0] == 3) {     /* an error about one of our requests */
      const unsigned char *o = m + 8;
      int oihl = (o[0] & 15) * 4;
      if (n >= ihl + 8 + oihl + 8 && o[9] == IPPROTO_ICMP && (o[oihl + 4] << 8 | o[oihl + 5]) == id) {
        static const char *why[] = {"Network", "Host", "Protocol", "Port"};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        printf("From %s icmp_seq=%u Destination %s Unreachable\n", ip, (unsigned)(o[oihl + 6] << 8 | o[oihl + 7]), m[1] < 4 ? why[m[1]] : "Host");
        fflush(stdout);
      }
      continue;
    }
    if (n < ihl + 8 || m[0] != 0 || (m[4] << 8 | m[5]) != id) continue; /* not our echo reply */
    unsigned seq = (unsigned)(m[6] << 8 | m[7]);
    recvd++;
    double rtt = 0;
    int timed = n - ihl - 8 >= (long)sizeof(double);
    if (timed) { double ts; memcpy(&ts, m + 8, sizeof ts); rtt = now_ms() - ts; }
    if (timed) { if (rtt < tmin) tmin = rtt; if (rtt > tmax) tmax = rtt; tsum += rtt; tsum2 += rtt * rtt; }
    if (!quiet) {
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
      if (timed) printf("%ld bytes from %s: icmp_seq=%u ttl=%d time=%.3f ms\n", n - ihl, ip, seq, buf[8], rtt);
      else printf("%ld bytes from %s: icmp_seq=%u ttl=%d\n", n - ihl, ip, seq, buf[8]);
      fflush(stdout);
    }
  }
  printf("\n--- %s ping statistics ---\n", host);
  printf("%ld packets transmitted, %ld received, %ld%% packet loss, time %.0fms\n", sent, recvd,
         sent ? (sent - recvd) * 100 / sent : 0, now_ms() - start);
  if (recvd && tsum > 0) {
    double avg = tsum / recvd, var = tsum2 / recvd - avg * avg;
    double mdev = 0;
    if (var > 0) { mdev = var; for (int i = 0; i < 20; i++) mdev = (mdev + var / mdev) / 2; } /* square root */
    printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", tmin, avg, tmax, mdev);
  }
  return recvd ? 0 : 1;
}
