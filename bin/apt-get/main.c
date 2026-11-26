// SPDX-License-Identifier: GPL-2.0-or-later
/* apt-get: the older spelling of apt, with its own output. The logic is in libpkg. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return apt_main(argc, argv, "apt-get"); }
