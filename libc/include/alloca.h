// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ALLOCA_H
#define _ALLOCA_H
#include <stddef.h>
#define alloca(n) __builtin_alloca(n)
#endif
