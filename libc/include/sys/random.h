// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H
#include <stddef_extra.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
ssize_t getrandom(void *, size_t, unsigned); /* libc/src/tools_compat.c */
#endif
