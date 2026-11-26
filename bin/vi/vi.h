// SPDX-License-Identifier: GPL-2.0-or-later
/* vi: shared declarations between vi.c (editor) and regex.c (pattern matcher). */
#ifndef VI_H
#define VI_H
int re_search(const char *re, const char *s, int len, int from, int *ms, int *me);
#endif
