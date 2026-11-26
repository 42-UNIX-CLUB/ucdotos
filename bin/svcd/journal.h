// SPDX-License-Identifier: GPL-2.0-or-later
/* The journal: one ring buffer in the manager's memory, allocated once, so its size
 * never changes. Each record is a line from a service (or from the manager) with its
 * time, priority, unit, identifier and pid. When the ring is full the oldest records
 * go, whatever unit they belong to (journald also has one budget for everything). */
#ifndef SVCD_JOURNAL_H
#define SVCD_JOURNAL_H
#include <stdint.h>
struct jrec { uint32_t seq; int64_t t_ms; int prio, pid; const char *unit, *ident, *text; int ulen, ilen, tlen; };
int journal_init(uint32_t bytes);       /* 0, or -1 if the memory cannot be had */
void journal_add(int64_t t_ms, int prio, int pid, const char *unit, const char *ident, const char *text, int tlen);
/* Walk records with seq > after, oldest first; f returns non-zero to stop. */
void journal_walk(uint32_t after, int (*f)(void *ctx, const struct jrec *r), void *ctx);
uint32_t journal_last_seq(void);
void journal_stats(uint32_t *bytes, uint32_t *used, uint32_t *records, uint32_t *dropped);
#endif
