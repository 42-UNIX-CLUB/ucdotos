// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _UTIME_H
#define _UTIME_H
#include <stddef_extra.h>
struct utimbuf { time_t actime, modtime; };
int utime(const char *, const struct utimbuf *);
#endif
