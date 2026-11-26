// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ASSERT_H
#define _ASSERT_H
void __assert_fail(const char *, const char *, int);
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__))
#endif
