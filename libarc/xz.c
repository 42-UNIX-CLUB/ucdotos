// SPDX-License-Identifier: GPL-2.0-or-later
/* xz decoder: the .xz container (stream header, blocks, index, footer,
 * concatenated streams with padding), the LZMA2 chunk layer, the LZMA
 * decoder, and the delta filter. Written from the published .xz format
 * description and the LZMA specification.
 *
 * Checks verified: header and footer CRC32, block header CRC32, the block
 * check (none, CRC32, CRC64 or SHA-256), the index CRC32, and that the index
 * agrees with the blocks actually seen (count, sizes). The last part is done
 * with running sums and a hash so memory does not grow with the block count. */
#include "arc_int.h"

#define CHUNK 65536
#define UNIT (CHUNK + 512)   /* a match may run 273 bytes past the stop point */

enum { X_HEAD, X_BLOCK, X_DATA, X_BEND, X_INDEX, X_FOOT, X_PAD, X_DONE };
enum { L_CTRL, L_COPY, L_LZMA };

struct lend { uint16_t choice, choice2, low[16][8], mid[16][8], high[256]; };

struct lzma {
  /* probabilities */
  uint16_t is_match[12 * 16], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12 * 16];
  uint16_t slot[4][64], special[115], align[16];
  struct lend len, rep;
  uint16_t lit[0x300 * 16];
  unsigned state, rep0, rep1, rep2, rep3;
  unsigned lc, lp, pb;
  /* range decoder */
  uint32_t range, code;
  const uint8_t *ip, *ilim, *ibase;
  int bad;               /* ran past the chunk or the input */
};

struct xzd {
  int phase, check, flags;
  unsigned check_size;
  /* LZMA2 */
  int l2, need_dict, need_props;
  uint32_t dict;
  uint64_t dpos;              /* bytes since the last dictionary reset */
  uint32_t unpacked, packed;  /* left in the current chunk */
  /* block */
  uint64_t blk_start_in, blk_out, decl_comp, decl_unc;
  int has_comp, has_unc;
  unsigned hsize;
  int delta;
  unsigned delta_dist;
  uint8_t delta_hist[256], delta_pos;
  uint32_t crc32;
  uint64_t crc64;
  arc_sha256 sha;
  struct awin dout;           /* output after the delta filter */
  /* stream */
  uint64_t nblocks, sum_unpadded, sum_unc, rec_hash;
  uint64_t index_size;
  struct lzma z;
};

static const uint8_t check_sizes[16] = {0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64};

static int trunc_err(arc_stream *s) { return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data"); }

/* ---- range decoder ---- */

static void rc_sync(arc_stream *s, struct xzd *d) {
  struct lzma *z = &d->z;
  size_t used = z->ip - z->ibase;
  s->in.pos += used;
  d->packed -= used;
  z->ibase = z->ip = s->in.buf + s->in.pos;
  size_t avail = s->in.end - s->in.pos;
  if (avail > d->packed) avail = d->packed;
  z->ilim = z->ip + avail;
}

static uint8_t rc_slow(arc_stream *s, struct xzd *d) {
  struct lzma *z = &d->z;
  rc_sync(s, d);
  if (d->packed == 0) { z->bad = 1; return 0; }
  if (z->ip == z->ilim) {
    if (in_fill(&s->in) <= 0) { z->bad = 2; return 0; }
    z->ibase = z->ip = s->in.buf + s->in.pos;  /* in_fill moved the buffer */
    rc_sync(s, d);
    if (z->ip == z->ilim) { z->bad = 2; return 0; }
  }
  return *z->ip++;
}

#define RC_BYTE() (z->ip < z->ilim ? *z->ip++ : rc_slow(s, d))
#define NORM() do { if (range < (1u << 24)) { range <<= 8; code = code << 8 | RC_BYTE(); } } while (0)
/* decode one bit with probability *p into variable b */
#define BIT(p, b) do { \
    uint32_t bound_ = (range >> 11) * *(p); \
    if (code < bound_) { range = bound_; *(p) += (2048 - *(p)) >> 5; b = 0; } \
    else { range -= bound_; code -= bound_; *(p) -= *(p) >> 5; b = 1; } \
    NORM(); } while (0)

static void lzma_reset_state(struct lzma *z) {
  uint16_t *a = z->is_match;
  /* every probability array starts at one half (1024 of 2048) */
  for (uint16_t *p = a; p < z->lit + 0x300 * 16; p++) *p = 1024;
  z->state = 0;
  z->rep0 = z->rep1 = z->rep2 = z->rep3 = 0;
}

static int rc_init(arc_stream *s, struct xzd *d) {
  struct lzma *z = &d->z;
  uint32_t range = 0, code = 0;
  z->ibase = z->ip = s->in.buf + s->in.pos;
  z->ilim = z->ip;
  rc_sync(s, d);
  uint8_t first = RC_BYTE();
  for (int i = 0; i < 4; i++) code = code << 8 | RC_BYTE();
  rc_sync(s, d);
  if (z->bad) return z->bad == 2 ? trunc_err(s) : arc_fail(s, "corrupt xz data (LZMA chunk too short)");
  if (first != 0) return arc_fail(s, "corrupt xz data (bad range coder start)");
  z->range = 0xFFFFFFFFu;
  z->code = code;
  (void)range;
  return 0;
}

/* Decodes symbols until the chunk is done or the window position reaches limit. */
static int lzma_run(arc_stream *s, struct xzd *d, size_t limit) {
  struct lzma *z = &d->z;
  struct awin *w = &s->w;
  uint8_t *buf = w->buf;
  size_t pos = w->pos;
  uint32_t range = z->range, code = z->code;
  unsigned state = z->state, rep0 = z->rep0;
  const unsigned pbmask = (1u << z->pb) - 1, lpmask = (1u << z->lp) - 1, lc = z->lc;
  uint64_t dpos = d->dpos;
  uint32_t left = d->unpacked;
  const char *err = 0;
  z->ibase = z->ip = s->in.buf + s->in.pos;
  z->ilim = z->ip;
  rc_sync(s, d);
  /* the input pointer lives in locals (no address taken) so it can stay in registers */
  const uint8_t *ip = z->ip, *ilim = z->ilim;
  uint8_t slow_byte;
#undef RC_BYTE
#define RC_BYTE() (ip < ilim ? *ip++ : (z->ip = ip, slow_byte = rc_slow(s, d), ip = z->ip, ilim = z->ilim, slow_byte))

  while (left && pos < limit) {
    unsigned ps = (unsigned)dpos & pbmask, b;
    BIT(&z->is_match[state * 16 + ps], b);
    if (!b) { /* literal */
      unsigned prev = dpos ? buf[pos - 1] : 0;
      uint16_t *probs = z->lit + 0x300 * ((((unsigned)dpos & lpmask) << lc) + (prev >> (8 - lc)));
      unsigned sym = 1;
      if (state >= 7) {
        if ((uint64_t)rep0 + 1 > dpos) { err = "corrupt xz data (distance before start)"; break; }
        unsigned mb = buf[pos - rep0 - 1];
        do {
          unsigned mbit = (mb >> 7) & 1;
          mb <<= 1;
          BIT(&probs[((1 + mbit) << 8) + sym], b);
          sym = sym << 1 | b;
          if (mbit != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) { BIT(&probs[sym], b); sym = sym << 1 | b; }
      buf[pos++] = (uint8_t)sym;
      dpos++;
      left--;
      state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
      continue;
    }
    unsigned len;
    struct lend *ld;
    BIT(&z->is_rep[state], b);
    if (!b) { /* match */
      z->rep3 = z->rep2; z->rep2 = z->rep1; z->rep1 = rep0;
      ld = &z->len;
      state = state < 7 ? 7 : 10;
    } else {
      if (!dpos) { err = "corrupt xz data (repeat before any data)"; break; }
      BIT(&z->is_rep0[state], b);
      if (!b) {
        BIT(&z->is_rep0_long[state * 16 + ps], b);
        if (!b) { /* short rep: one byte at rep0 */
          if ((uint64_t)rep0 + 1 > dpos) { err = "corrupt xz data (distance before start)"; break; }
          state = state < 7 ? 9 : 11;
          buf[pos] = buf[pos - rep0 - 1];
          pos++; dpos++; left--;
          continue;
        }
      } else {
        unsigned dist;
        BIT(&z->is_rep1[state], b);
        if (!b) dist = z->rep1;
        else {
          BIT(&z->is_rep2[state], b);
          if (!b) dist = z->rep2;
          else { dist = z->rep3; z->rep3 = z->rep2; }
          z->rep2 = z->rep1;
        }
        z->rep1 = rep0;
        rep0 = dist;
      }
      ld = &z->rep;
      state = state < 7 ? 8 : 11;
    }
    /* length */
    {
      uint16_t *t;
      unsigned nb, base;
      BIT(&ld->choice, b);
      if (!b) { t = ld->low[ps]; nb = 3; base = 0; }
      else {
        BIT(&ld->choice2, b);
        if (!b) { t = ld->mid[ps]; nb = 3; base = 8; }
        else { t = ld->high; nb = 8; base = 16; }
      }
      unsigned m = 1;
      for (unsigned i = 0; i < nb; i++) { BIT(&t[m], b); m = m << 1 | b; }
      len = base + m - (1u << nb);
    }
    if (ld == &z->len) { /* distance for a new match */
      unsigned ls = len < 3 ? len : 3, m = 1;
      for (int i = 0; i < 6; i++) { BIT(&z->slot[ls][m], b); m = m << 1 | b; }
      unsigned slot = m - 64;
      if (slot < 4) rep0 = slot;
      else {
        unsigned nd = (slot >> 1) - 1;
        rep0 = (2 | (slot & 1)) << nd;
        if (slot < 14) {
          uint16_t *p = z->special + rep0 - slot;
          unsigned mm = 1;
          for (unsigned i = 0; i < nd; i++) { BIT(&p[mm], b); mm = mm << 1 | b; rep0 |= b << i; }
        } else {
          for (unsigned i = 0; i < nd - 4; i++) {
            range >>= 1;
            uint32_t t = (code - range) >> 31; /* 1 if code < range */
            code -= range & (t - 1);
            rep0 |= (1 - t) << (nd - 1 - i);
            NORM();
          }
          unsigned mm = 1;
          for (unsigned i = 0; i < 4; i++) { BIT(&z->align[mm], b); mm = mm << 1 | b; rep0 |= b << i; }
        }
        if (rep0 == 0xFFFFFFFFu) { err = "corrupt xz data (end marker inside LZMA2)"; break; }
      }
    }
    len += 2;
    if ((uint64_t)rep0 + 1 > dpos || rep0 >= d->dict) { err = "corrupt xz data (distance too far back)"; break; }
    if (len > left) { err = "corrupt xz data (match runs past the chunk)"; break; }
    copy_match_wild(buf + pos, (size_t)rep0 + 1, len);
    pos += len; dpos += len; left -= len;
  }
  z->ip = ip;
  rc_sync(s, d);
  z->range = range; z->code = code; z->state = state; z->rep0 = rep0;
  w->pos = pos;
  d->dpos = dpos;
  d->unpacked = left;
  if (err) return arc_fail(s, err);
  if (z->bad) return z->bad == 2 ? trunc_err(s) : arc_fail(s, "corrupt xz data (LZMA chunk overrun)");
  if (!left) {
    if (d->packed != 0 || code != 0) return arc_fail(s, "corrupt xz data (LZMA chunk size mismatch)");
    d->l2 = L_CTRL;
  }
  return 0;
}

/* One LZMA2 step: reads a chunk header or decodes up to limit. Returns 1
 * when more follows, 0 at the end of the LZMA2 data, -1 on error. */
static int lzma2_step(arc_stream *s, struct xzd *d, size_t limit) {
  struct awin *w = &s->w;
  if (d->l2 == L_CTRL) {
    int c = in_byte(&s->in);
    if (c < 0) return trunc_err(s);
    if (c == 0) return 0;
    if (c == 1 || c == 2) {
      if (c == 1) { d->dpos = 0; d->need_dict = 0; }
      else if (d->need_dict) return arc_fail(s, "corrupt xz data (no dictionary reset)");
      uint8_t h[2];
      if (in_read(&s->in, h, 2) != 2) return trunc_err(s);
      d->unpacked = ((uint32_t)h[0] << 8 | h[1]) + 1;
      d->l2 = L_COPY;
      return 1;
    }
    if (c < 0x80) return arc_fail(s, "corrupt xz data (bad LZMA2 control byte)");
    uint8_t h[5];
    int reset = (c >> 5) & 3;
    if (in_read(&s->in, h, reset >= 2 ? 5 : 4) != (size_t)(reset >= 2 ? 5 : 4)) return trunc_err(s);
    d->unpacked = ((uint32_t)(c & 31) << 16 | (uint32_t)h[0] << 8 | h[1]) + 1;
    d->packed = ((uint32_t)h[2] << 8 | h[3]) + 1;
    if (reset == 3) { d->dpos = 0; d->need_dict = 0; }
    else if (d->need_dict) return arc_fail(s, "corrupt xz data (no dictionary reset)");
    if (reset >= 2) {
      unsigned p = h[4];
      if (p >= 9 * 5 * 5) return arc_fail(s, "corrupt xz data (bad LZMA properties)");
      d->z.lc = p % 9; p /= 9;
      d->z.lp = p % 5;
      d->z.pb = p / 5;
      if (d->z.lc + d->z.lp > 4) return arc_fail(s, "corrupt xz data (lc + lp > 4)");
      d->need_props = 0;
    } else if (d->need_props) return arc_fail(s, "corrupt xz data (no LZMA properties)");
    if (reset >= 1) lzma_reset_state(&d->z);
    d->z.bad = 0;
    if (rc_init(s, d) < 0) return -1;
    d->l2 = L_LZMA;
    return 1;
  }
  if (d->l2 == L_COPY) {
    size_t k = d->unpacked;
    if (k > limit - w->pos) k = limit - w->pos;
    if (in_read(&s->in, w->buf + w->pos, k) != k) return trunc_err(s);
    w->pos += k;
    d->dpos += k;
    d->unpacked -= k;
    if (!d->unpacked) d->l2 = L_CTRL;
    return 1;
  }
  return lzma_run(s, d, limit) < 0 ? -1 : 1;
}

/* ---- container ---- */

static int get_vli(const uint8_t *p, unsigned n, unsigned *off, uint64_t *v) {
  uint64_t r = 0;
  for (int i = 0; i < 9; i++) {
    if (*off >= n) return -1;
    uint8_t b = p[(*off)++];
    r |= (uint64_t)(b & 0x7f) << (7 * i);
    if (!(b & 0x80)) {
      if (b == 0 && i > 0) return -1; /* not the shortest form */
      *v = r;
      return 0;
    }
  }
  return -1;
}

/* Reads a variable-length integer from the input, adding its bytes to *crc. */
static int read_vli(arc_stream *s, uint32_t *crc, uint64_t *v, uint64_t *count) {
  uint8_t b[9];
  unsigned n = 0;
  for (;;) {
    int c = in_byte(&s->in);
    if (c < 0) return trunc_err(s);
    b[n++] = c;
    if (!(c & 0x80) || n == 9) break;
  }
  *crc = arc_crc32(*crc, b, n);
  *count += n;
  unsigned off = 0;
  if (get_vli(b, n, &off, v) < 0) return arc_fail(s, "corrupt xz index");
  return 0;
}

static int stream_header(arc_stream *s, struct xzd *d) {
  uint8_t h[12];
  if (in_read(&s->in, h, 12) != 12) return trunc_err(s);
  if (memcmp(h, "\xfd" "7zXZ\0", 6)) return arc_fail(s, "not in xz format");
  if (arc_crc32(0, h + 6, 2) != rd32le(h + 8)) return arc_fail(s, "xz stream header is corrupt");
  if (h[6] != 0 || h[7] > 15) return arc_fail(s, "unsupported xz stream flags");
  d->flags = h[7];
  d->check = h[7];
  if (d->check != 0 && d->check != 1 && d->check != 4 && d->check != 10)
    return arc_fail(s, "unsupported xz integrity check type");
  d->check_size = check_sizes[d->check];
  d->nblocks = d->sum_unpadded = d->sum_unc = d->rec_hash = 0;
  return 0;
}

static int block_header(arc_stream *s, struct xzd *d, int first) {
  uint8_t h[1024];
  h[0] = first;
  d->hsize = (first + 1) * 4;
  if (in_read(&s->in, h + 1, d->hsize - 1) != d->hsize - 1) return trunc_err(s);
  if (arc_crc32(0, h, d->hsize - 4) != rd32le(h + d->hsize - 4)) return arc_fail(s, "xz block header is corrupt");
  unsigned n = d->hsize - 4, off = 2, flags = h[1];
  if (flags & 0x3c) return arc_fail(s, "unsupported xz block flags");
  d->has_comp = flags & 0x40;
  d->has_unc = flags & 0x80;
  if (d->has_comp && (get_vli(h, n, &off, &d->decl_comp) < 0 || d->decl_comp == 0)) return arc_fail(s, "xz block header is corrupt");
  if (d->has_unc && get_vli(h, n, &off, &d->decl_unc) < 0) return arc_fail(s, "xz block header is corrupt");
  int nf = (flags & 3) + 1, have_lzma2 = 0;
  d->delta = 0;
  for (int i = 0; i < nf; i++) {
    uint64_t id, psize;
    if (get_vli(h, n, &off, &id) < 0 || get_vli(h, n, &off, &psize) < 0 || psize > n - off)
      return arc_fail(s, "xz block header is corrupt");
    const uint8_t *pp = h + off;
    off += psize;
    if (have_lzma2) return arc_fail(s, "xz filter chain is invalid (LZMA2 is not last)");
    if (id == 0x21) {
      if (psize != 1 || pp[0] > 40) return arc_fail(s, "xz LZMA2 properties are invalid");
      uint32_t dict = pp[0] == 40 ? 0xFFFFFFFFu : (uint32_t)(2 | (pp[0] & 1)) << (pp[0] / 2 + 11);
      if (dict > s->memlimit) {
        char m[120];
        snprintf(m, sizeof m, "xz dictionary of %u MiB is over the memory limit of %llu MiB",
                 (unsigned)(dict >> 20), (unsigned long long)(s->memlimit >> 20));
        return arc_fail(s, m);
      }
      d->dict = dict;
      have_lzma2 = 1;
    } else if (id == 0x03) {
      if (psize != 1 || d->delta) return arc_fail(s, "unsupported xz delta filter setup");
      d->delta = 1;
      d->delta_dist = pp[0] + 1;
      memset(d->delta_hist, 0, sizeof d->delta_hist);
      d->delta_pos = 0;
    } else if (id >= 0x04 && id <= 0x0b) {
      return arc_fail(s, "xz branch/call/jump (BCJ) filters are not supported");
    } else return arc_fail(s, "unsupported xz filter");
  }
  if (!have_lzma2) return arc_fail(s, "xz filter chain has no LZMA2 filter");
  for (; off < n; off++) if (h[off]) return arc_fail(s, "xz block header is corrupt");
  return 0;
}

/* The index must list exactly the blocks seen. Instead of keeping a list,
 * keep a count, two sums and a CRC64 over the records, and compute the same
 * from the index. */
static void rec_add(uint64_t *n, uint64_t *su, uint64_t *sc, uint64_t *h, uint64_t unpadded, uint64_t unc) {
  uint8_t r[16];
  for (int i = 0; i < 8; i++) { r[i] = unpadded >> (8 * i); r[8 + i] = unc >> (8 * i); }
  *h = arc_crc64(*h, r, 16);
  (*n)++;
  *su += unpadded;
  *sc += unc;
}

static void record(struct xzd *d, uint64_t unpadded, uint64_t unc) {
  rec_add(&d->nblocks, &d->sum_unpadded, &d->sum_unc, &d->rec_hash, unpadded, unc);
}

static int read_index(arc_stream *s, struct xzd *d) {
  uint8_t zero = 0;
  uint32_t crc = arc_crc32(0, &zero, 1); /* the indicator byte, already read */
  uint64_t size = 1, count, n = 0, su = 0, sc = 0, h = 0;
  if (read_vli(s, &crc, &count, &size) < 0) return -1;
  if (count != d->nblocks) return arc_fail(s, "xz index does not match the blocks");
  for (uint64_t i = 0; i < count; i++) {
    uint64_t unp, unc;
    if (read_vli(s, &crc, &unp, &size) < 0 || read_vli(s, &crc, &unc, &size) < 0) return -1;
    if (unp == 0) return arc_fail(s, "corrupt xz index");
    rec_add(&n, &su, &sc, &h, unp, unc);
  }
  if (su != d->sum_unpadded || sc != d->sum_unc || h != d->rec_hash) return arc_fail(s, "xz index does not match the blocks");
  while (size & 3) {
    int c = in_byte(&s->in);
    if (c < 0) return trunc_err(s);
    if (c) return arc_fail(s, "corrupt xz index (padding)");
    uint8_t b = 0;
    crc = arc_crc32(crc, &b, 1);
    size++;
  }
  uint8_t t[4];
  if (in_read(&s->in, t, 4) != 4) return trunc_err(s);
  if (rd32le(t) != crc) return arc_fail(s, "xz index is corrupt (CRC32)");
  d->index_size = size + 4;
  return 0;
}

static int xz_fill(arc_stream *s) {
  struct xzd *d = s->dec;
  for (;;) {
    switch (d->phase) {
    case X_HEAD:
      if (stream_header(s, d) < 0) return -1;
      d->phase = X_BLOCK;
      break;
    case X_BLOCK: {
      int c = in_byte(&s->in);
      if (c < 0) return trunc_err(s);
      if (c == 0) { d->phase = X_INDEX; break; }
      if (block_header(s, d, c) < 0) return -1;
      d->blk_start_in = arc_total_in(s);
      d->blk_out = 0;
      d->crc32 = 0;
      d->crc64 = 0;
      arc_sha256_init(&d->sha);
      d->l2 = L_CTRL;
      d->need_dict = 1;
      d->need_props = 1;
      d->dpos = 0;
      win_reset(&s->w, d->dict, UNIT);
      d->phase = X_DATA;
      break;
    }
    case X_DATA: {
      if (win_need(&s->w, UNIT) < 0) return arc_fail(s, "out of memory");
      size_t start = s->w.pos, limit = start + CHUNK;
      int r = 1;
      while (s->w.pos < limit && (r = lzma2_step(s, d, limit)) == 1) {}
      if (r < 0) return -1;
      size_t n = s->w.pos - start;
      uint8_t *p = s->w.buf + start;
      if (d->delta) {
        /* the filter's output must not touch the LZMA dictionary: write it elsewhere */
        s->ow = &d->dout;
        if (d->dout.cap < n) {
          uint8_t *nb = realloc(d->dout.buf, UNIT);
          if (!nb) return arc_fail(s, "out of memory");
          d->dout.buf = nb;
          d->dout.cap = UNIT;
        }
        uint8_t *q = d->dout.buf;
        unsigned dist = d->delta_dist;
        uint8_t pos = d->delta_pos;
        for (size_t i = 0; i < n; i++) {
          q[i] = p[i] + d->delta_hist[(uint8_t)(pos + dist)];
          d->delta_hist[pos--] = q[i];
        }
        d->delta_pos = pos;
        d->dout.rd = 0;
        d->dout.pos = n;
        s->w.rd = s->w.pos;
        p = q;
      } else s->ow = 0;
      if (d->check == 1) d->crc32 = arc_crc32(d->crc32, p, n);
      else if (d->check == 4) d->crc64 = arc_crc64(d->crc64, p, n);
      else if (d->check == 10) arc_sha256_update(&d->sha, p, n);
      d->blk_out += n;
      if (d->has_unc && d->blk_out > d->decl_unc) return arc_fail(s, "xz block is larger than its header says");
      if (r == 0) d->phase = X_BEND;
      if (n) return 1;
      break;
    }
    case X_BEND: {
      uint64_t comp = arc_total_in(s) - d->blk_start_in; /* compressed data, before padding */
      if (d->has_comp && comp != d->decl_comp) return arc_fail(s, "xz block compressed size does not match its header");
      if (d->has_unc && d->blk_out != d->decl_unc) return arc_fail(s, "xz block size does not match its header");
      for (uint64_t k = comp; k & 3; k++) {
        int c = in_byte(&s->in);
        if (c < 0) return trunc_err(s);
        if (c) return arc_fail(s, "corrupt xz data (block padding)");
      }
      uint8_t ck[32], mine[32];
      if (in_read(&s->in, ck, d->check_size) != d->check_size) return trunc_err(s);
      if (d->check == 1) wr32le(mine, d->crc32);
      else if (d->check == 4) { wr32le(mine, (uint32_t)d->crc64); wr32le(mine + 4, (uint32_t)(d->crc64 >> 32)); }
      else if (d->check == 10) arc_sha256_final(&d->sha, mine);
      if (memcmp(ck, mine, d->check_size)) return arc_fail(s, "xz block check failed: the data is corrupt");
      record(d, d->hsize + comp + d->check_size, d->blk_out);
      d->phase = X_BLOCK;
      break;
    }
    case X_INDEX:
      if (read_index(s, d) < 0) return -1;
      d->phase = X_FOOT;
      break;
    case X_FOOT: {
      uint8_t f[12];
      if (in_read(&s->in, f, 12) != 12) return trunc_err(s);
      if (memcmp(f + 10, "YZ", 2)) return arc_fail(s, "xz stream footer is corrupt");
      if (arc_crc32(0, f + 4, 6) != rd32le(f)) return arc_fail(s, "xz stream footer is corrupt");
      if (((uint64_t)rd32le(f + 4) + 1) * 4 != d->index_size) return arc_fail(s, "xz footer does not match the index");
      if (f[8] != 0 || f[9] != d->flags) return arc_fail(s, "xz footer flags do not match the header");
      d->phase = X_PAD;
      break;
    }
    case X_PAD: {
      /* stream padding (zeros, a multiple of four bytes), then the end or another stream */
      size_t n = in_avail(&s->in, 4);
      if (s->in.err) return arc_fail(s, "read error");
      if (n == 0) { d->phase = X_DONE; return 0; }
      if (s->in.buf[s->in.pos] != 0) { d->phase = X_HEAD; break; }
      if (n < 4 || rd32le(s->in.buf + s->in.pos)) return arc_fail(s, "corrupt xz stream padding");
      s->in.pos += 4;
      break;
    }
    default:
      return 0;
    }
  }
}

static void xz_free(void *p) {
  struct xzd *d = p;
  if (d) free(d->dout.buf);
  free(d);
}

int xz_start(arc_stream *s) {
  struct xzd *d = calloc(1, sizeof *d);
  if (!d) return arc_fail(s, "out of memory");
  s->dec = d;
  s->fill = xz_fill;
  s->dec_free = xz_free;
  d->phase = X_HEAD;
  return 0;
}
