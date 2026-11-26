// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H
#include <sys/syscall.h> /* struct sysinfo */
int sysinfo(struct sysinfo *);
int get_nprocs(void);
int get_nprocs_conf(void);
#endif
