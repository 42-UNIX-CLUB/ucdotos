// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding header of the UC.OS C compiler (C11 7.16). va_list is a pointer into
 * the caller's argument area, the same layout clang uses on wasm32. */
#ifndef _CC_STDARG_H
#define _CC_STDARG_H
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)
#define __va_copy(d, s) __builtin_va_copy(d, s)
#endif
