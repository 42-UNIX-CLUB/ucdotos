// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _POLL_H
#define _POLL_H
#include <sys/syscall.h>
int poll(struct pollfd *, unsigned long, int timeout_ms);
#endif
