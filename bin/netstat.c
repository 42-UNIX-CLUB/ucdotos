// SPDX-License-Identifier: GPL-2.0-or-later
/* netstat: the kernel's sockets and interfaces.
 *   netstat [-t] [-u] [-w] [-x] [-l] [-a] [-n] [-p] [-e]   connections (-e: TCP internals)
 *   netstat -i                                            interfaces
 * Without -l or -a, listening sockets are left out. Addresses are always numeric. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *state_name(int s) {
  static const char *n[] = {"", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT",
                            "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING"};
  return s > 0 && s <= 11 ? n[s] : "?";
}
static void endpoint(char *out, unsigned addr, unsigned short port) {
  unsigned a = htonl(addr);
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &a, ip, sizeof ip);
  if (port) sprintf(out, "%s:%u", ip, port); else sprintf(out, "%s:*", ip);
}
static const char *prog(int pid) {
  static char b[48];
  if (!pid) return "-";
  struct procinfo pi;
  for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++)
    if (pi.pid == pid) { snprintf(b, sizeof b, "%d/%s", pid, pi.name); return b; }
  return "-";
}
static void interfaces(void) {
  printf("Kernel Interface table\n");
  printf("%-8s %6s %10s %10s %10s %10s %10s  %s\n", "Iface", "MTU", "RX-OK", "RX-BYTES", "TX-OK", "TX-BYTES", "DROPPED", "Flg");
  struct ifinfo i;
  for (int k = 0; __syscall2(SYS_ifinfo, k, &i) == 0; k++)
    printf("%-8s %6u %10llu %10llu %10llu %10llu %10llu  %s%s\n", i.name, i.mtu, i.rx_packets, i.rx_bytes, i.tx_packets,
           i.tx_bytes, i.dropped, i.flags & IFF_LOOPBACK ? "L" : "", i.flags & IFF_UP ? "RU" : "");
}

int main(int argc, char **argv) {
  int tcp = 0, udp = 0, raw = 0, listening = 0, all = 0, pids = 0, ext = 0, ifs = 0, c;
  while ((c = getopt(argc, argv, "tuwxlanpei")) != -1) {
    switch (c) {
    case 't': tcp = 1; break;
    case 'u': udp = 1; break;
    case 'w': raw = 1; break;
    case 'x': break;                                   /* unix sockets: not listed (yet) */
    case 'l': listening = 1; break;
    case 'a': all = 1; break;
    case 'n': break;
    case 'p': pids = 1; break;
    case 'e': ext = 1; break;
    case 'i': ifs = 1; break;
    default:
      fprintf(stderr, "usage: netstat [-tuwlanpe] | netstat -i\n");
      return 2;
    }
  }
  if (ifs) { interfaces(); return 0; }
  if (!tcp && !udp && !raw) tcp = udp = raw = 1;
  printf("Active Internet connections (%s)\n", all ? "servers and established" : listening ? "only servers" : "w/o servers");
  printf("Proto Recv-Q Send-Q %-23s %-23s %-12s", "Local Address", "Foreign Address", "State");
  if (pids) printf(" %-20s", "PID/Program name");
  if (ext) printf(" %8s %8s %8s %6s %8s %8s", "cwnd", "ssthresh", "srtt-ms", "rto", "retrans", "wnd");
  printf("\n");
  struct sockinfo s;
  for (int k = 0; __syscall2(SYS_netstat, k, &s) == 0; k++) {
    int is_tcp = s.proto == IPPROTO_TCP, is_udp = s.proto == IPPROTO_UDP;
    int is_raw = !is_tcp && !is_udp && s.proto != 255;
    if (s.proto == 255) continue;                      /* packet sockets have no addresses */
    if ((is_tcp && !tcp) || (is_udp && !udp) || (is_raw && !raw)) continue;
    int server = is_tcp ? s.state == TCP_LISTEN : s.state != TCP_ESTABLISHED;
    if (!all && server != listening) continue;
    char l[32], r[32];
    endpoint(l, s.laddr, s.lport);
    endpoint(r, s.raddr, s.rport);
    const char *st = is_tcp ? state_name(s.state) : s.state == TCP_ESTABLISHED ? "ESTABLISHED" : "";
    printf("%-5s %6u %6u %-23s %-23s %-12s", is_tcp ? "tcp" : is_udp ? "udp" : "raw", s.rx_queue, s.tx_queue, l, r, st);
    if (pids) printf(" %-20s", prog(s.owner));
    if (ext && is_tcp && s.state != TCP_LISTEN) {
      char th[12];
      if (s.ssthresh >= 0x7fffffffu) strcpy(th, "-"); else snprintf(th, sizeof th, "%u", s.ssthresh);
      printf(" %8u %8s %8.2f %6u %8u %8u", s.cwnd, th, s.srtt_us / 1000.0, s.rto_ms, s.retransmits, s.snd_wnd);
    }
    printf("\n");
  }
  return 0;
}
