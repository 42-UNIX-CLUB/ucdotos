// SPDX-License-Identifier: GPL-2.0-or-later
/* systemd-sysusers: accepted and ignored. UC.OS has one user (uid 1000) and no user
 * database, so the system users that sysusers.d(5) lines ask for (Debian packages were
 * found to use only "u" lines) have nothing to be created in. It succeeds, so that
 * maintainer scripts calling it go on. docs/SERVICES.md lists what this loses. */
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("systemd-sysusers: accepted and ignored on UC.OS (one user, no user database).\n");
      return 0;
    }
  return 0;
}
