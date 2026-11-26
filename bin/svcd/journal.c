// SPDX-License-Identifier: GPL-2.0-or-later
/* See journal.h. Layout of a record in the ring (never split across the end: a record
 * that does not fit before the end starts again at offset 0 and the tail is skipped):
 *   u32 len (whole record, 4-aligned), u32 seq, s64 t_ms, s32 pid, u8 prio, u8 ulen,
 *   u8 ilen, u8 pad, u16 tlen, u16 pad, then unit, ident, text. */
#include "journal.h"
#include <stdlib.h>
#include <string.h>

#define HDR 28
static uint8_t *ring;
static uint32_t cap, head, tail, nrec, seq, dropped;
/* head: where the next record goes; tail: the oldest record. A record of length 0 is
 * the end marker: the next record is at offset 0. */

int journal_init(uint32_t bytes) {
  if (bytes < 4096) bytes = 4096;
  ring = malloc(bytes);
  if (!ring) return -1;
  cap = bytes;
  head = tail = nrec = 0;
  return 0;
}
static uint32_t rd32(uint32_t o) { uint32_t v; memcpy(&v, ring + o, 4); return v; }
static void drop_oldest(void) {
  if (!nrec) return;
  if (rd32(tail) == 0) tail = 0; /* the end marker: the oldest record is at the start */
  tail += rd32(tail);
  nrec--;
  dropped++;
  if (!nrec) head = tail = 0;
}
void journal_add(int64_t t, int prio, int pid, const char *unit, const char *ident, const char *text, int tlen) {
  if (!ring) return;
  int ul = unit ? (int)strlen(unit) : 0, il = ident ? (int)strlen(ident) : 0;
  if (ul > 255) ul = 255;
  if (il > 255) il = 255;
  if (tlen > 4000) tlen = 4000;
  if (tlen < 0) tlen = 0;
  uint32_t len = (HDR + ul + il + tlen + 3) & ~3u;
  if (len + 8 > cap / 2) return;
  /* every record leaves 4 bytes after it for an end marker */
  for (;;) {
    if (!nrec) { head = tail = 0; break; }
    if (head > tail || (head == tail && 0)) {
      if (head + len + 4 <= cap) break;
      memset(ring + head, 0, 4); /* end marker; continue at the start */
      head = 0;
      continue;
    }
    /* head <= tail: the free space is [head, tail) */
    if (head < tail && head + len + 4 <= tail) break;
    drop_oldest();
  }
  uint8_t *p = ring + head;
  uint32_t s = ++seq;
  int32_t pd = pid;
  uint16_t tl = (uint16_t)tlen;
  memcpy(p, &len, 4); memcpy(p + 4, &s, 4); memcpy(p + 8, &t, 8); memcpy(p + 16, &pd, 4);
  p[20] = (uint8_t)prio; p[21] = (uint8_t)ul; p[22] = (uint8_t)il; p[23] = 0;
  memcpy(p + 24, &tl, 2); p[26] = p[27] = 0;
  memcpy(p + HDR, unit, ul);
  memcpy(p + HDR + ul, ident, il);
  memcpy(p + HDR + ul + il, text, tlen);
  head += len;
  memset(ring + head, 0, 4);
  nrec++;
}
void journal_walk(uint32_t after, int (*f)(void *, const struct jrec *), void *ctx) {
  uint32_t o = tail;
  for (uint32_t i = 0; i < nrec; i++) {
    uint32_t len = rd32(o);
    if (len == 0) { o = 0; len = rd32(o); }
    struct jrec r;
    uint8_t *p = ring + o;
    memcpy(&r.seq, p + 4, 4); memcpy(&r.t_ms, p + 8, 8); memcpy(&r.pid, p + 16, 4);
    r.prio = p[20]; r.ulen = p[21]; r.ilen = p[22];
    uint16_t tl;
    memcpy(&tl, p + 24, 2);
    r.tlen = tl;
    r.unit = (const char *)p + HDR; r.ident = r.unit + r.ulen; r.text = r.ident + r.ilen;
    if (r.seq > after && f(ctx, &r)) return;
    o += len;
  }
}
uint32_t journal_last_seq(void) { return seq; }
void journal_stats(uint32_t *b, uint32_t *u, uint32_t *r, uint32_t *d) {
  *b = cap; *r = nrec; *d = dropped;
  *u = !nrec ? 0 : head > tail ? head - tail : cap - tail + head;
}
