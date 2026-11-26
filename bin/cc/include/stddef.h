// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding header of the UC.OS C compiler (C11 7.19). */
#ifndef _CC_STDDEF_H
#define _CC_STDDEF_H
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef struct { long long __ll __attribute__((aligned(8))); long double __ld __attribute__((aligned(16))); } max_align_t;
#define NULL ((void *)0)
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
