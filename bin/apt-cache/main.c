// SPDX-License-Identifier: GPL-2.0-or-later
/* apt-cache: questions about repository indexes. The logic is in libpkg. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return apt_main(argc, argv, "apt-cache"); }
