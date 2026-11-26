// SPDX-License-Identifier: GPL-2.0-or-later
/* whoami: print the name of the effective user, from /etc/passwd */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc > 1) { fprintf(stderr, "whoami: extra operand '%s'\nusage: whoami\n", argv[1]); return 1; }
  unsigned uid = geteuid();
  FILE *f = fopen("/etc/passwd", "r");
  char line[1024];
  while (f && fgets(line, sizeof line, f)) {
    char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    unsigned id;
    if (c2 && sscanf(c2 + 1, "%u", &id) == 1 && id == uid) { *c1 = 0; puts(line); fclose(f); return 0; }
  }
  if (f) fclose(f);
  fprintf(stderr, "whoami: cannot find name for user ID %u\n", uid);
  return 1;
}
