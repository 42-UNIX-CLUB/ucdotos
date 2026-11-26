// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FNMATCH_H
#define _FNMATCH_H
/* shell-style pattern matching (libc/src/fnmatch.c) */
#define FNM_NOMATCH 1
#define FNM_NOESCAPE 0x02
#define FNM_PATHNAME 0x01
#define FNM_PERIOD 0x04
#define FNM_LEADING_DIR 0x08 /* GNU: the pattern may match a leading part followed by '/' */
#define FNM_CASEFOLD 0x10    /* GNU: ignore case */
#define FNM_FILE_NAME FNM_PATHNAME
int fnmatch(const char *pattern, const char *string, int flags);
#endif
