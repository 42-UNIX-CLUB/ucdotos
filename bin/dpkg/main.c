// SPDX-License-Identifier: GPL-2.0-or-later
/* dpkg: install, remove and query Debian packages. The logic is in libpkg. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return dpkg_main(argc, argv, 0); }
