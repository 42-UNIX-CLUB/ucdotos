// SPDX-License-Identifier: GPL-2.0-or-later
/* ps: list processes from SYS_procinfo */
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
int main(int argc, char **argv) {
  struct procinfo pi;
  printf("%5s %5s %-5s %8s %9s %10s  %s\n", "PID", "PPID", "STATE", "MEM", "SYSCALLS", "TIME", "NAME");
  for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) {
    const char *st = pi.state == 1 ? "R" : pi.state == 2 ? "Z" : "?";
    unsigned long long ms = pi.cpu_us / 1000;
    char t[32];
    snprintf(t, sizeof t, "%llu:%02llu.%03llu", ms / 60000, ms / 1000 % 60, ms % 1000);
    printf("%5d %5d %-5s %7uK %9llu %10s  %s\n", pi.pid, pi.ppid, st, pi.mem_pages * 64, pi.syscalls, t, pi.name);
  }
  return 0;
}
