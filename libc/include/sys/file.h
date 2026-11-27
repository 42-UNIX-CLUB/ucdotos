// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_FILE_H
#define _SYS_FILE_H
#include <sys/syscall.h>
/* flock(2): advisory whole-file locks, owned by the open file. LOCK_SH, LOCK_EX, LOCK_NB
 * and LOCK_UN come from abi/abi.h. */
int flock(int, int);
#endif
