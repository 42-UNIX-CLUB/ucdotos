// SPDX-License-Identifier: GPL-2.0-or-later
/* hostname [-s]: print the name of this machine (setting it is not supported) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int i = 1, shrt = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--short")) shrt = 1;
    else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fqdn") || !strcmp(argv[i], "--long")) ;
    else { fprintf(stderr, "hostname: invalid option '%s'\nusage: hostname [-s]\n", argv[i]); return 1; }
  }
  if (i < argc) { fprintf(stderr, "hostname: setting the host name is not supported\n"); return 1; }
  char name[256];
  if (gethostname(name, sizeof name) != 0) { perror("hostname"); return 1; }
  name[sizeof name - 1] = 0;
  if (shrt) name[strcspn(name, ".")] = 0;
  puts(name);
  return 0;
}
