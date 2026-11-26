// SPDX-License-Identifier: GPL-2.0-or-later
/* uname [-asnrvmo] */
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
int main(int argc, char **argv) {
  struct utsname u;
  if (__syscall1(SYS_uname, &u) < 0) { perror("uname"); return 1; }
  int f = 0; /* bits: s n r v m o */
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') { fprintf(stderr, "uname: extra operand '%s'\n", argv[i]); return 1; }
    for (char *c = argv[i] + 1; *c; c++) {
      const char *k = strchr("snrvmo", *c);
      if (*c == 'a') f = 63;
      else if (k) f |= 1 << (k - "snrvmo");
      else { fprintf(stderr, "usage: uname [-asnrvmo]\n"); return 1; }
    }
  }
  if (!f) f = 1;
  const char *v[] = {u.sysname, u.nodename, u.release, u.version, u.machine, "UC.OS"};
  int first = 1;
  for (int i = 0; i < 6; i++) if (f & (1 << i)) { printf("%s%s", first ? "" : " ", v[i]); first = 0; }
  putchar('\n');
  return 0;
}
