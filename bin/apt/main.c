// SPDX-License-Identifier: GPL-2.0-or-later
/* apt, apt-get and apt-cache: fetch, resolve and install packages from
 * repositories. One program; the name it was started under (argv[0]) picks
 * the behaviour. apt-get and apt-cache are names in bin/apt/ALIASES. The logic
 * is in libpkg. */
#include <string.h>
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) {
  const char *n = argc > 0 && argv[0] ? argv[0] : "apt", *s = strrchr(n, '/');
  if (s) n = s + 1;
  if (!strcmp(n, "apt-get") || !strcmp(n, "apt-cache")) return apt_main(argc, argv, n);
  return apt_main(argc, argv, "apt");
}
