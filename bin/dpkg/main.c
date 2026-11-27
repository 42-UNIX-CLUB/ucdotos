// SPDX-License-Identifier: GPL-2.0-or-later
/* dpkg and dpkg-query: one program; started as dpkg-query (a name in
 * bin/dpkg/ALIASES) it behaves as dpkg-query. The logic is in libpkg. */
#include <string.h>
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) {
  const char *n = argc > 0 && argv[0] ? argv[0] : "dpkg", *s = strrchr(n, '/');
  if (s) n = s + 1;
  return dpkg_main(argc, argv, !strcmp(n, "dpkg-query"));
}
