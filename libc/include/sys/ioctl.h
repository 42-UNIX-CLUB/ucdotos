// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#include <sys/syscall.h>
/* request numbers (TCGETS, TIOCGWINSZ, ...) and struct winsize come from abi.h */
#define TIOCSCTTY 0x540E
int ioctl(int, unsigned long, ...);
#endif
