// SPDX-License-Identifier: GPL-2.0-or-later
/* apt: fetch, resolve and install packages from repositories. The logic is in libpkg. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return apt_main(argc, argv, "apt"); }
