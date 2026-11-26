// SPDX-License-Identifier: GPL-2.0-or-later
/* dpkg-deb: look inside .deb files. The logic is in libpkg. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return dpkg_deb_main(argc, argv); }
