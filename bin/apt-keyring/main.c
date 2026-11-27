// SPDX-License-Identifier: GPL-2.0-or-later
/* apt-keyring: list the keys apt trusts and check signed files with them. The logic is in libpkg/keyring.c. */
#include "../../libpkg/pkg.h"
int main(int argc, char **argv) { return keyring_main(argc, argv); }
