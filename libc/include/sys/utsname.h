// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
#include <sys/syscall.h> /* struct utsname */
int uname(struct utsname *);
#endif
