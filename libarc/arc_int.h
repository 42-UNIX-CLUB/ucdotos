// SPDX-License-Identifier: GPL-2.0-or-later
/* libarc internals shared by the decoders. Not part of the interface. */
#ifndef ARC_INT_H
#define ARC_INT_H
#include "arc.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define IN_SIZE 65536u   /* input buffer */
#define IN_KEEP 16u      /* bytes kept before the read position on refill, so a bit reader can give back what it over-read */

/* Buffered input from the caller's callback. */
struct ain {
  arc_src src;
  void *ctx;
  uint8_t *buf;          /* IN_SIZE bytes */
  size_t pos, end;       /* unread bytes are buf[pos..end) */
  int eof, err;          /* source said 0 / source failed */
  uint64_t got;          /* bytes taken from the source in total */
};
int in_fill(struct ain *in);                       /* reads more; returns bytes added, 0 at end, -1 on error */
int in_byte(struct ain *in);                       /* next byte, or -1 at end or on error */
size_t in_read(struct ain *in, void *dst, size_t n); /* up to n bytes; fewer only at end or on error */
size_t in_avail(struct ain *in, size_t n);          /* tries to have n bytes buffered; returns how many are */

/* Output window: a linear buffer holding recent history followed by fresh
 * output. Decoders write at pos; arc_read hands out buf[rd..pos). When the
 * buffer is full the last `dict` bytes slide to the front. The buffer grows
 * on demand, so a stream that declares a large dictionary but is short only
 * uses what it needs. */
struct awin {
  uint8_t *buf;
  size_t cap, pos, rd;
  size_t dict;           /* history the format may refer back to */
  size_t max;            /* largest the buffer may become */
};
/* Set the history size and the largest unit decoded in one go. Called at the
 * start of each stream or frame, only when all output has been read. Drops
 * any history. */
void win_reset(struct awin *w, size_t dict, size_t unit);
/* Make room for at least `need` bytes after pos (need <= the unit given to
 * win_reset). Only when rd == pos. Returns 0, or -1 when out of memory. */
int win_need(struct awin *w, size_t need);

struct arc_stream {
  int want, fmt;          /* format asked for, format found */
  struct ain in;
  struct awin w;
  struct awin *ow;         /* where arc_read takes output from: &w, or a decoder's own buffer */
  int (*fill)(arc_stream *); /* produce output into w: 1 = some, 0 = end, -1 = error */
  void *dec;               /* decoder state */
  void (*dec_free)(void *);
  int failed, done;
  uint64_t out_total;
  uint64_t memlimit;
  char err[160];
  /* tar reader state */
  uint64_t tar_left;       /* data bytes left in the current entry */
  unsigned tar_pad;        /* padding after them */
};
int arc_fail(arc_stream *s, const char *msg);   /* records msg, returns -1 */

int inflate_start(arc_stream *s);   /* gzip, zlib, raw deflate (by s->fmt) */
int xz_start(arc_stream *s);
int zstd_start(arc_stream *s);

/* Copy a match of len bytes from dst - dist (dist >= 1). Overlap is the
 * point: a short distance repeats a pattern. Writes exactly len bytes. */
static inline void copy_match(uint8_t *dst, size_t dist, size_t len) {
  const uint8_t *src = dst - dist;
  if (dist >= len) { memcpy(dst, src, len); return; }
  if (dist == 1) { memset(dst, *src, len); return; }
  if (dist >= 8) {
    /* the source runs ahead of the destination by >= 8: 8-byte steps are safe */
    while (len >= 8) { uint64_t t; memcpy(&t, src, 8); memcpy(dst, &t, 8); src += 8; dst += 8; len -= 8; }
    while (len--) *dst++ = *src++;
    return;
  }
  while (len--) *dst++ = *src++;
}

#define WILD 32  /* slack the wild copies below may write past the end */

static inline void copy16(uint8_t *d, const uint8_t *s) {
  uint64_t a, b;
  memcpy(&a, s, 8);
  memcpy(&b, s + 8, 8);
  memcpy(d, &a, 8);
  memcpy(d + 8, &b, 8);
}

/* Copies len bytes from a buffer that does not overlap dst, 16 at a time.
 * May read up to WILD bytes past src + len and write up to WILD bytes past
 * dst + len: both buffers must have that slack. */
static inline void copy_wild(uint8_t *dst, const uint8_t *src, size_t len) {
  uint8_t *end = dst + len;
  do { copy16(dst, src); dst += 16; src += 16; } while (dst < end);
}

/* copy_match that may write up to WILD bytes past dst + len. */
static inline void copy_match_wild(uint8_t *dst, size_t dist, size_t len) {
  const uint8_t *src = dst - dist;
  uint8_t *end = dst + len;
  if (dist >= 16) {
    do { copy16(dst, src); dst += 16; src += 16; } while (dst < end);
    return;
  }
  if (dist == 1) { memset(dst, *src, len); return; }
  /* short period: lay down the pattern until it is 8 long, then repeat it */
  while (dist < 8) {
    size_t k = dist;
    src = dst - dist;
    for (size_t i = 0; i < k; i++) dst[i] = src[i];
    dst += k;
    dist += k;  /* the same pattern, read from twice as far back */
    if (dst >= end) return;
  }
  src = dst - dist;
  do { uint64_t t; memcpy(&t, src, 8); memcpy(dst, &t, 8); dst += 8; src += 8; } while (dst < end);
}

/* checksums used inside */
typedef struct { uint64_t v[4], total; uint8_t mem[32]; unsigned n; } xxh64_state;
void xxh64_init(xxh64_state *st, uint64_t seed);
void xxh64_update(xxh64_state *st, const void *p, size_t n);
uint64_t xxh64_digest(const xxh64_state *st);

static inline uint32_t rd16le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd24le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static inline uint32_t rd32le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64le(const uint8_t *p) { return rd32le(p) | (uint64_t)rd32le(p + 4) << 32; }
static inline uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; } /* host order; both targets are little-endian */
static inline void wr32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void wr32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

#endif
