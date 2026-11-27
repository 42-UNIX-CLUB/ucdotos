// SPDX-License-Identifier: GPL-2.0-or-later
/* Zstandard decoder, written from RFC 8878: frames (and skippable frames),
 * raw, RLE and compressed blocks, Huffman-coded literals, FSE-coded
 * sequences, repeat offsets, and the XXH64 content checksum.
 * Frames that need an external dictionary are refused with a clear error. */
#include "arc_int.h"

#define BMAX 131072            /* largest block, compressed or not */
#define UNIT (BMAX + 64)

enum { Z_FRAME, Z_BLOCK, Z_CHECK, Z_DONE };

struct fse { uint16_t base; uint8_t sym, nb; };
struct huf { uint8_t sym, nb; };

struct zstdd {
  int phase, frames, last, has_sum, has_fcs;
  uint64_t fcs, window, frame_out;
  uint32_t bmax;
  xxh64_state xh;
  uint32_t rep[3];
  /* tables kept between blocks of a frame */
  int huf_bits, huf_ok;
  struct huf huf[1 << 11];
  struct fse ll[512], of[256], ml[512];
  int ll_al, of_al, ml_al, ll_ok, of_ok, ml_ok;
  uint8_t blk[BMAX + WILD];  /* slack for copy_wild reading raw literals */
  uint8_t lit[BMAX + WILD];
};

/* ---- bit readers ---- */

/* Backward bit stream: the last byte holds a 1 marker above the first bits;
 * bits are read from there towards the start. Bits past the start read as
 * zero; `left` goes negative when that happens. */
struct bbr {
  const uint8_t *base;
  size_t bpos;   /* bytes [0, bpos) not loaded yet */
  uint64_t c;    /* next bits at the top */
  int cnt;
  int64_t left;
};

static inline void bb_refill(struct bbr *b) {
  if (b->bpos >= 8 && b->cnt <= 56) {
    uint64_t v = ld64(b->base + b->bpos - 8);
    int k = (63 - b->cnt) >> 3;
    b->c |= v >> b->cnt;
    b->bpos -= k;
    b->cnt += 8 * k;
    return;
  }
  while (b->cnt <= 56 && b->bpos > 0) {
    b->bpos--;
    b->c |= (uint64_t)b->base[b->bpos] << (56 - b->cnt);
    b->cnt += 8;
  }
}

static int bb_init(struct bbr *b, const uint8_t *p, size_t n) {
  if (n == 0 || p[n - 1] == 0) return -1;
  int hb = 31 - __builtin_clz(p[n - 1]);
  b->base = p;
  b->bpos = n;
  b->c = 0;
  b->cnt = 0;
  bb_refill(b);
  b->c <<= 8 - hb;
  b->cnt -= 8 - hb;
  b->left = (int64_t)n * 8 - (8 - hb);
  return 0;
}

static inline uint32_t bb_read(struct bbr *b, int n) {
  if (n == 0) return 0;
  uint32_t v = (uint32_t)(b->c >> (64 - n));
  b->c <<= n;
  b->cnt -= n;
  b->left -= n;
  return v;
}

/* Forward little-endian bit peek with zeros past the end. */
static uint32_t fpeek(const uint8_t *p, size_t n, uint64_t bitpos, int k) {
  uint64_t v = 0;
  size_t i = bitpos >> 3;
  for (int j = 0; j < 4; j++) if (i + j < n) v |= (uint64_t)p[i + j] << (8 * j);
  return (uint32_t)(v >> (bitpos & 7)) & ((1u << k) - 1);
}

/* ---- FSE ---- */

/* Reads a distribution; returns bytes used or -1. */
static int read_ncount(const uint8_t *p, size_t n, int16_t *norm, int *nsym, int *alog, int maxsym, int maxal) {
  if (n == 0) return -1;
  uint64_t bp = 4;
  int al = (p[0] & 15) + 5;
  if (al > maxal) return -1;
  int remaining = (1 << al) + 1, threshold = 1 << al, nb = al + 1, s = 0;
  while (remaining > 1) {
    if (s > maxsym) return -1;
    int max = (2 * threshold - 1) - remaining;
    uint32_t v = fpeek(p, n, bp, nb);
    int count;
    if ((int)(v & (threshold - 1)) < max) { count = v & (threshold - 1); bp += nb - 1; }
    else { count = v & (2 * threshold - 1); if (count >= threshold) count -= max; bp += nb; }
    count--;
    remaining -= count < 0 ? -count : count;
    norm[s++] = count;
    if (remaining < 1) return -1;
    if (count == 0) {
      for (;;) {
        uint32_t r = fpeek(p, n, bp, 2);
        bp += 2;
        for (uint32_t k = 0; k < r; k++) { if (s > maxsym) return -1; norm[s++] = 0; }
        if (r < 3) break;
      }
    }
    while (remaining < threshold) { nb--; threshold >>= 1; }
    if ((bp + 7) >> 3 > n) return -1;
  }
  if (remaining != 1) return -1;
  *nsym = s;
  *alog = al;
  return (int)((bp + 7) >> 3);
}

static int build_fse(struct fse *t, const int16_t *norm, int nsym, int al) {
  int size = 1 << al, high = size - 1;
  uint16_t next[256];
  for (int s = 0; s < nsym; s++) {
    if (norm[s] == -1) { t[high--].sym = s; next[s] = 1; }
    else next[s] = norm[s];
  }
  int pos = 0, step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
  for (int s = 0; s < nsym; s++)
    for (int i = 0; i < norm[s]; i++) {
      t[pos].sym = s;
      do pos = (pos + step) & mask; while (pos > high);
    }
  if (pos != 0) return -1;
  for (int u = 0; u < size; u++) {
    unsigned x = next[t[u].sym]++;
    int nb = al - (31 - __builtin_clz(x));
    t[u].nb = nb;
    t[u].base = (x << nb) - size;
  }
  return 0;
}

static const int16_t ll_def[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t ml_def[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t of_def[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
static const uint32_t ll_base[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40,
                                     48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t ll_bits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t ml_base[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
                                     30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195,
                                     16387, 32771, 65539};
static const uint8_t ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* Sets up one sequence table according to its mode. Returns bytes used or -1. */
static int seq_table(struct fse *t, int *al, int *ok, int mode, const uint8_t *p, size_t n,
                     const int16_t *def, int ndef, int defal, int maxsym, int maxal) {
  int16_t norm[64];
  int nsym, a, used = 0;
  switch (mode) {
  case 0: nsym = ndef; a = defal; memcpy(norm, def, ndef * sizeof *def); break;
  case 1:
    if (n < 1 || p[0] > maxsym) return -1;
    t[0].sym = p[0]; t[0].nb = 0; t[0].base = 0;
    *al = 0; *ok = 1;
    return 1;
  case 2:
    used = read_ncount(p, n, norm, &nsym, &a, maxsym, maxal);
    if (used < 0) return -1;
    break;
  default:
    return *ok ? 0 : -1;
  }
  if (build_fse(t, norm, nsym, a) < 0) return -1;
  *al = a;
  *ok = 1;
  return used;
}

/* ---- literals ---- */

static int read_huf_table(struct zstdd *d, const uint8_t *p, size_t n) {
  if (n < 1) return -1;
  uint8_t w[256];
  int nw = 0, used;
  unsigned hb = p[0];
  if (hb >= 128) {
    nw = hb - 127;
    used = 1 + (nw + 1) / 2;
    if ((size_t)used > n) return -1;
    for (int i = 0; i < nw; i++) w[i] = i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4;
  } else {
    used = 1 + hb;
    if (hb == 0 || (size_t)used > n) return -1;
    int16_t norm[16];
    int nsym, al;
    struct fse t[64];
    int k = read_ncount(p + 1, hb, norm, &nsym, &al, 11, 6);
    if (k < 0 || build_fse(t, norm, nsym, al) < 0) return -1;
    struct bbr b;
    if (bb_init(&b, p + 1 + k, hb - k) < 0) return -1;
    bb_refill(&b);
    unsigned s1 = bb_read(&b, al), s2 = bb_read(&b, al);
    for (;;) {
      if (nw > 253) return -1;
      bb_refill(&b);
      w[nw++] = t[s1].sym;
      s1 = t[s1].base + bb_read(&b, t[s1].nb);
      if (b.left < 0) { w[nw++] = t[s2].sym; break; }
      w[nw++] = t[s2].sym;
      s2 = t[s2].base + bb_read(&b, t[s2].nb);
      if (b.left < 0) { w[nw++] = t[s1].sym; break; }
    }
  }
  if (nw > 255) return -1;
  uint32_t sum = 0;
  for (int i = 0; i < nw; i++) {
    if (w[i] > 11) return -1;
    if (w[i]) sum += 1u << (w[i] - 1);
  }
  if (sum == 0) return -1;
  int maxbits = 32 - __builtin_clz(sum);
  if (maxbits > 11) return -1;
  uint32_t rest = (1u << maxbits) - sum;
  if (rest & (rest - 1)) return -1;
  w[nw++] = 32 - __builtin_clz(rest);  /* the implied last weight: log2(rest) + 1 */
  if (w[nw - 1] > maxbits) return -1;
  int idx = 0;
  for (int wt = 1; wt <= maxbits; wt++)
    for (int s = 0; s < nw; s++)
      if (w[s] == wt) {
        int cnt = 1 << (wt - 1);
        for (int k = 0; k < cnt; k++) { d->huf[idx + k].sym = s; d->huf[idx + k].nb = maxbits + 1 - wt; }
        idx += cnt;
      }
  if (idx != 1 << maxbits) return -1;
  d->huf_bits = maxbits;
  d->huf_ok = 1;
  return used;
}

static int huf_stream(struct zstdd *d, const uint8_t *p, size_t n, uint8_t *out, size_t count) {
  struct bbr b;
  if (bb_init(&b, p, n) < 0) return -1;
  const int hb = d->huf_bits;
  const struct huf *t = d->huf;
  size_t i = 0;
  while (count - i >= 4) {
    bb_refill(&b);
    for (int k = 0; k < 4; k++) {
      struct huf e = t[b.c >> (64 - hb)];
      out[i++] = e.sym;
      b.c <<= e.nb; b.cnt -= e.nb; b.left -= e.nb;
    }
  }
  while (i < count) {
    bb_refill(&b);
    struct huf e = t[b.c >> (64 - hb)];
    out[i++] = e.sym;
    b.c <<= e.nb; b.cnt -= e.nb; b.left -= e.nb;
  }
  return b.left == 0 ? 0 : -1;
}

/* Four streams decoded side by side: the lookups of one stream overlap the
 * shifts of the others. Ends each stream exactly as huf_stream does. */
static int huf_4streams(struct zstdd *d, const uint8_t *r, size_t s1, size_t s2, size_t s3, size_t s4, size_t each, size_t last) {
  struct bbr b[4];
  const uint8_t *p[4] = {r, r + s1, r + s1 + s2, r + s1 + s2 + s3};
  size_t n[4] = {s1, s2, s3, s4}, cnt[4] = {each, each, each, last};
  uint8_t *o[4] = {d->lit, d->lit + each, d->lit + 2 * each, d->lit + 3 * each};
  for (int k = 0; k < 4; k++) if (bb_init(&b[k], p[k], n[k]) < 0) return -1;
  const int hb = d->huf_bits;
  const struct huf *t = d->huf;
  size_t i = 0, common = last < each ? last : each;
  while (common - i >= 4) {
    for (int k = 0; k < 4; k++) bb_refill(&b[k]);
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) {
        struct huf e = t[b[k].c >> (64 - hb)];
        o[k][i + j] = e.sym;
        b[k].c <<= e.nb; b[k].cnt -= e.nb; b[k].left -= e.nb;
      }
    i += 4;
  }
  for (int k = 0; k < 4; k++)
    for (size_t j = i; j < cnt[k]; j++) {
      bb_refill(&b[k]);
      struct huf e = t[b[k].c >> (64 - hb)];
      o[k][j] = e.sym;
      b[k].c <<= e.nb; b[k].cnt -= e.nb; b[k].left -= e.nb;
    }
  for (int k = 0; k < 4; k++) if (b[k].left != 0) return -1;
  return 0;
}

/* Decodes the literals section; sets *lit and *nlit. Returns bytes used or -1. */
static int literals(struct zstdd *d, const uint8_t *p, size_t n, const uint8_t **lit, size_t *nlit) {
  if (n < 1) return -1;
  int type = p[0] & 3, sf = (p[0] >> 2) & 3;
  size_t regen, comp, hl;
  if (type < 2) {
    if (sf == 0 || sf == 2) { hl = 1; regen = p[0] >> 3; }
    else if (sf == 1) { hl = 2; if (n < 2) return -1; regen = (p[0] >> 4) + ((size_t)p[1] << 4); }
    else { hl = 3; if (n < 3) return -1; regen = (p[0] >> 4) + ((size_t)p[1] << 4) + ((size_t)p[2] << 12); }
    if (regen > BMAX) return -1;
    if (type == 0) {
      if (hl + regen > n) return -1;
      *lit = p + hl;
      *nlit = regen;
      return (int)(hl + regen);
    }
    if (hl + 1 > n) return -1;
    memset(d->lit, p[hl], regen);
    *lit = d->lit;
    *nlit = regen;
    return (int)(hl + 1);
  }
  int streams = sf == 0 ? 1 : 4;
  if (sf < 2) {
    hl = 3; if (n < 3) return -1;
    uint32_t v = rd24le(p);
    regen = (v >> 4) & 0x3ff; comp = v >> 14;
  } else if (sf == 2) {
    hl = 4; if (n < 4) return -1;
    uint32_t v = rd32le(p);
    regen = (v >> 4) & 0x3fff; comp = v >> 18;
  } else {
    hl = 5; if (n < 5) return -1;
    uint64_t v = rd32le(p) | (uint64_t)p[4] << 32;
    regen = (v >> 4) & 0x3ffff; comp = v >> 22;
  }
  if (regen > BMAX || hl + comp > n) return -1;
  const uint8_t *q = p + hl;
  size_t qn = comp;
  if (type == 2) {
    int k = read_huf_table(d, q, qn);
    if (k < 0) return -1;
    q += k; qn -= k;
  } else if (!d->huf_ok) return -1;
  if (streams == 1) {
    if (huf_stream(d, q, qn, d->lit, regen) < 0) return -1;
  } else {
    if (qn < 6) return -1;
    size_t s1 = rd16le(q), s2 = rd16le(q + 2), s3 = rd16le(q + 4);
    if (s1 + s2 + s3 > qn - 6) return -1;
    size_t s4 = qn - 6 - s1 - s2 - s3, each = (regen + 3) / 4;
    if (3 * each > regen) return -1;
    const uint8_t *r = q + 6;
    if (huf_4streams(d, r, s1, s2, s3, s4, each, regen - 3 * each) < 0) return -1;
  }
  *lit = d->lit;
  *nlit = regen;
  return (int)(hl + comp);
}

/* ---- blocks ---- */

static int compressed_block(arc_stream *s, struct zstdd *d, size_t size) {
  const uint8_t *p = d->blk, *end = d->blk + size;
  const uint8_t *lit;
  size_t nlit;
  int k = literals(d, p, size, &lit, &nlit);
  if (k < 0) return arc_fail(s, "corrupt zstd data (literals)");
  p += k;
  const uint8_t *litend = lit + nlit;
  struct awin *w = &s->w;
  uint8_t *out = w->buf + w->pos, *ostart = out, *olimit = out + d->bmax;
  if (p >= end) return arc_fail(s, "corrupt zstd data (no sequences section)");
  uint32_t nseq = *p++;
  if (nseq >= 128) {
    if (nseq < 255) { if (p >= end) return arc_fail(s, "corrupt zstd data"); nseq = ((nseq - 128) << 8) + *p++; }
    else { if (end - p < 2) return arc_fail(s, "corrupt zstd data"); nseq = p[0] + (p[1] << 8) + 0x7F00; p += 2; }
  }
  if (nseq) {
    if (p >= end) return arc_fail(s, "corrupt zstd data");
    unsigned modes = *p++;
    if (modes & 3) return arc_fail(s, "corrupt zstd data (reserved bits)");
    if ((k = seq_table(d->ll, &d->ll_al, &d->ll_ok, modes >> 6, p, end - p, ll_def, 36, 6, 35, 9)) < 0) return arc_fail(s, "corrupt zstd data (literal length table)");
    p += k;
    if ((k = seq_table(d->of, &d->of_al, &d->of_ok, (modes >> 4) & 3, p, end - p, of_def, 29, 5, 31, 8)) < 0) return arc_fail(s, "corrupt zstd data (offset table)");
    p += k;
    if ((k = seq_table(d->ml, &d->ml_al, &d->ml_ok, (modes >> 2) & 3, p, end - p, ml_def, 53, 6, 52, 9)) < 0) return arc_fail(s, "corrupt zstd data (match length table)");
    p += k;
    struct bbr b;
    if (bb_init(&b, p, end - p) < 0) return arc_fail(s, "corrupt zstd data (sequence bit stream)");
    bb_refill(&b);
    unsigned sl = bb_read(&b, d->ll_al), so = bb_read(&b, d->of_al), sm = bb_read(&b, d->ml_al);
    uint32_t r0 = d->rep[0], r1 = d->rep[1], r2 = d->rep[2];
    for (uint32_t i = 0; i < nseq; i++) {
      bb_refill(&b);
      unsigned ofc = d->of[so].sym, llc = d->ll[sl].sym, mlc = d->ml[sm].sym;
      uint32_t ofv = (1u << ofc) + bb_read(&b, ofc);
      if (b.cnt < 32) bb_refill(&b);  /* the two lengths take at most 32 bits */
      uint32_t ml = ml_base[mlc] + bb_read(&b, ml_bits[mlc]);
      uint32_t ll = ll_base[llc] + bb_read(&b, ll_bits[llc]);
      if (i + 1 < nseq) {
        if (b.cnt < 26) bb_refill(&b);  /* the three state updates take at most 26 */
        sl = d->ll[sl].base + bb_read(&b, d->ll[sl].nb);
        sm = d->ml[sm].base + bb_read(&b, d->ml[sm].nb);
        so = d->of[so].base + bb_read(&b, d->of[so].nb);
      }
      uint32_t off;
      if (ofv > 3) { off = ofv - 3; r2 = r1; r1 = r0; r0 = off; }
      else {
        unsigned idx = ofv - 1 + (ll == 0);
        if (idx == 0) off = r0;
        else {
          off = idx == 1 ? r1 : idx == 2 ? r2 : r0 - 1;
          if (idx > 1) r2 = r1;
          r1 = r0;
          r0 = off;
        }
      }
      if (ll > (size_t)(litend - lit)) return arc_fail(s, "corrupt zstd data (too few literals)");
      if ((size_t)ll + ml > (size_t)(olimit - out)) return arc_fail(s, "corrupt zstd data (block too large)");
      copy_wild(out, lit, ll);  /* the window keeps WILD bytes of slack past the block limit */
      out += ll;
      lit += ll;
      size_t have = out - w->buf;
      if (off == 0 || off > have || off > d->window) return arc_fail(s, "corrupt zstd data (offset too far back)");
      copy_match_wild(out, off, ml);
      out += ml;
    }
    if (b.left != 0) return arc_fail(s, "corrupt zstd data (sequence bit stream not fully used)");
    d->rep[0] = r0; d->rep[1] = r1; d->rep[2] = r2;
  } else if (p != end) return arc_fail(s, "corrupt zstd data (bytes after the literals)");
  size_t rest = litend - lit;
  if (rest > (size_t)(olimit - out)) return arc_fail(s, "corrupt zstd data (block too large)");
  memcpy(out, lit, rest);
  out += rest;
  w->pos += out - ostart;
  return 0;
}

static int trunc_err(arc_stream *s) { return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data"); }

static int frame_header(arc_stream *s, struct zstdd *d) {
  uint8_t h[14];
  if (in_read(&s->in, h, 1) != 1) return trunc_err(s);
  unsigned fd = h[0], fcs_flag = fd >> 6, single = (fd >> 5) & 1, did_flag = fd & 3;
  if (fd & 8) return arc_fail(s, "corrupt zstd frame header (reserved bit)");
  static const uint8_t did_size[4] = {0, 1, 2, 4}, fcs_size[4] = {0, 2, 4, 8};
  unsigned fsz = fcs_size[fcs_flag];
  if (fcs_flag == 0 && single) fsz = 1;
  unsigned n = !single + did_size[did_flag] + fsz;
  if (in_read(&s->in, h + 1, n) != n) return trunc_err(s);
  const uint8_t *p = h + 1;
  uint64_t window = 0;
  if (!single) {
    unsigned e = p[0] >> 3, m = p[0] & 7;
    uint64_t base = 1ull << (10 + e);
    window = base + (base / 8) * m;
    p++;
  }
  uint32_t did = 0;
  if (did_flag == 1) did = p[0];
  else if (did_flag == 2) did = rd16le(p);
  else if (did_flag == 3) did = rd32le(p);
  p += did_size[did_flag];
  if (did) return arc_fail(s, "zstd frame needs a dictionary (not supported)");
  d->has_fcs = fsz != 0;
  if (fsz == 1) d->fcs = p[0];
  else if (fsz == 2) d->fcs = rd16le(p) + 256;
  else if (fsz == 4) d->fcs = rd32le(p);
  else if (fsz == 8) d->fcs = rd64le(p);
  if (single) window = d->fcs;
  d->has_sum = (fd >> 2) & 1;
  uint64_t need = window;
  if (d->has_fcs && d->fcs < need) need = d->fcs;
  if (need > s->memlimit || need > (size_t)-1 / 2) {
    char m[120];
    snprintf(m, sizeof m, "zstd window of %llu MiB is over the memory limit of %llu MiB",
             (unsigned long long)(need >> 20), (unsigned long long)(s->memlimit >> 20));
    return arc_fail(s, m);
  }
  d->window = window;
  d->bmax = window < BMAX ? (uint32_t)window : BMAX;
  win_reset(&s->w, (size_t)need, UNIT);
  d->frame_out = 0;
  d->rep[0] = 1; d->rep[1] = 4; d->rep[2] = 8;
  d->huf_ok = d->ll_ok = d->of_ok = d->ml_ok = 0;
  xxh64_init(&d->xh, 0);
  return 0;
}

static int zstd_fill(arc_stream *s) {
  struct zstdd *d = s->dec;
  for (;;) {
    switch (d->phase) {
    case Z_FRAME: {
      uint8_t m[4];
      size_t got = in_read(&s->in, m, 4);
      if (got == 0 && !s->in.err && d->frames) { d->phase = Z_DONE; return 0; }
      if (got != 4) return trunc_err(s);
      uint32_t magic = rd32le(m);
      if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) { /* skippable frame */
        uint8_t z[4];
        if (in_read(&s->in, z, 4) != 4) return trunc_err(s);
        uint32_t left = rd32le(z);
        while (left) {
          uint8_t tmp[4096];
          size_t k = left < sizeof tmp ? left : sizeof tmp;
          if (in_read(&s->in, tmp, k) != k) return trunc_err(s);
          left -= k;
        }
        d->frames++;
        break;
      }
      if (magic != 0xFD2FB528u) return arc_fail(s, d->frames ? "trailing garbage after zstd data" : "not in zstd format");
      if (frame_header(s, d) < 0) return -1;
      d->frames++;
      d->phase = Z_BLOCK;
      break;
    }
    case Z_BLOCK: {
      uint8_t h[3];
      if (in_read(&s->in, h, 3) != 3) return trunc_err(s);
      uint32_t bh = rd24le(h), type = (bh >> 1) & 3, size = bh >> 3;
      d->last = bh & 1;
      if (win_need(&s->w, UNIT) < 0) return arc_fail(s, "out of memory");
      size_t start = s->w.pos;
      if (type == 3) return arc_fail(s, "corrupt zstd data (reserved block type)");
      if (size > d->bmax) return arc_fail(s, "corrupt zstd data (block too large)");
      if (type == 0) {
        if (in_read(&s->in, s->w.buf + start, size) != size) return trunc_err(s);
        s->w.pos += size;
      } else if (type == 1) {
        int c = in_byte(&s->in);
        if (c < 0) return trunc_err(s);
        memset(s->w.buf + start, c, size);
        s->w.pos += size;
      } else {
        if (in_read(&s->in, d->blk, size) != size) return trunc_err(s);
        if (compressed_block(s, d, size) < 0) return -1;
      }
      size_t n = s->w.pos - start;
      if (d->has_sum) xxh64_update(&d->xh, s->w.buf + start, n);
      d->frame_out += n;
      if (d->has_fcs && d->frame_out > d->fcs) return arc_fail(s, "zstd frame is larger than its header says");
      if (d->last) d->phase = Z_CHECK;
      if (n) return 1;
      break;
    }
    case Z_CHECK:
      if (d->has_fcs && d->frame_out != d->fcs) return arc_fail(s, "zstd frame size does not match its header");
      if (d->has_sum) {
        uint8_t c[4];
        if (in_read(&s->in, c, 4) != 4) return trunc_err(s);
        if (rd32le(c) != (uint32_t)xxh64_digest(&d->xh)) return arc_fail(s, "zstd checksum mismatch: the data is corrupt");
      }
      d->phase = Z_FRAME;
      break;
    default:
      return 0;
    }
  }
}

int zstd_start(arc_stream *s) {
  struct zstdd *d = calloc(1, sizeof *d);
  if (!d) return arc_fail(s, "out of memory");
  s->dec = d;
  s->fill = zstd_fill;
  d->phase = Z_FRAME;
  return 0;
}
