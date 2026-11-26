// SPDX-License-Identifier: GPL-2.0-or-later
/* ifconfig: show or change network interfaces.
 *   ifconfig [iface]                           show one interface, or all
 *   ifconfig iface [up|down] [mtu N] [loss P] [delay MS]
 * loss (percent of packets dropped) and delay (milliseconds added to each packet) are
 * UC.OS's own: they make the loopback behave like a bad network, to watch TCP recover. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void human(char *out, unsigned long long b) {
  const char *u[] = {"B", "KiB", "MiB", "GiB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024 && i < 3) { v /= 1024; i++; }
  sprintf(out, "%.1f %s", v, u[i]);
}
static void show(const struct ifinfo *i) {
  char flags[64] = "";
  if (i->flags & IFF_UP) strcat(flags, "UP,");
  if (i->flags & IFF_LOOPBACK) strcat(flags, "LOOPBACK,");
  if (i->flags & IFF_UP) strcat(flags, "RUNNING,");
  if (*flags) flags[strlen(flags) - 1] = 0;
  unsigned a = htonl(i->addr), m = htonl(i->mask);
  char as[INET_ADDRSTRLEN], ms[INET_ADDRSTRLEN], rx[24], tx[24];
  inet_ntop(AF_INET, &a, as, sizeof as);
  inet_ntop(AF_INET, &m, ms, sizeof ms);
  human(rx, i->rx_bytes);
  human(tx, i->tx_bytes);
  printf("%s: flags=%u<%s>  mtu %u\n", i->name, (i->flags & IFF_UP ? 1 : 0) | (i->flags & IFF_LOOPBACK ? 8 : 0) | (i->flags & IFF_UP ? 64 : 0), flags, i->mtu);
  printf("        inet %s  netmask %s\n", as, ms);
  if (i->flags & IFF_LOOPBACK) printf("        loop  (Local Loopback)\n");
  printf("        RX packets %llu  bytes %llu (%s)\n", i->rx_packets, i->rx_bytes, rx);
  printf("        TX packets %llu  bytes %llu (%s)\n", i->tx_packets, i->tx_bytes, tx);
  printf("        dropped %llu\n", i->dropped);
  if (i->loss_permille || i->delay_ms) printf("        simulated: loss %u.%u%%  delay %u ms\n", i->loss_permille / 10, i->loss_permille % 10, i->delay_ms);
  printf("\n");
}
static int find(const char *name, struct ifinfo *i) {
  for (int k = 0; __syscall2(SYS_ifinfo, k, i) == 0; k++) if (!strcmp(i->name, name)) return k;
  return -1;
}
static int ctl(int idx, int op, long v, const char *what) {
  long r = __syscall3(SYS_ifctl, idx, op, v);
  if (r < 0) { fprintf(stderr, "ifconfig: %s: %s\n", what, strerror((int)-r)); return 1; }
  return 0;
}

int main(int argc, char **argv) {
  struct ifinfo i;
  if (argc == 1) {
    for (int k = 0; __syscall2(SYS_ifinfo, k, &i) == 0; k++) show(&i);
    return 0;
  }
  int idx = find(argv[1], &i);
  if (idx < 0) { fprintf(stderr, "ifconfig: %s: no such interface\n", argv[1]); return 1; }
  if (argc == 2) { show(&i); return 0; }
  int rc = 0;
  for (int a = 2; a < argc; a++) {
    if (!strcmp(argv[a], "up")) rc |= ctl(idx, IFCTL_UP, 1, "up");
    else if (!strcmp(argv[a], "down")) rc |= ctl(idx, IFCTL_UP, 0, "down");
    else if (a + 1 < argc && !strcmp(argv[a], "mtu")) rc |= ctl(idx, IFCTL_MTU, atol(argv[++a]), "mtu");
    else if (a + 1 < argc && !strcmp(argv[a], "loss")) rc |= ctl(idx, IFCTL_LOSS, (long)(strtod(argv[++a], NULL) * 10 + 0.5), "loss");
    else if (a + 1 < argc && !strcmp(argv[a], "delay")) rc |= ctl(idx, IFCTL_DELAY, atol(argv[++a]), "delay");
    else {
      fprintf(stderr, "usage: ifconfig [iface [up|down] [mtu N] [loss PERCENT] [delay MS]]\n");
      return 2;
    }
  }
  return rc;
}
