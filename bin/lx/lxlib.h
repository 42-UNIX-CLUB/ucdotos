// SPDX-License-Identifier: GPL-2.0-or-later
/* Small portability layer: the engine builds against the UC.OS libc and against the
 * host's libc (native runs and tests). Only standard C is used through it. */
#ifndef LX_LIB_H
#define LX_LIB_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define lx_snprintf snprintf
#endif
