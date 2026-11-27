// SPDX-License-Identifier: GPL-2.0-or-later
/* Deflate decoder (RFC 1951) with the gzip (RFC 1952) and zlib (RFC 1950)
 * wrappers. Huffman codes are decoded with a first-level table indexed by the
 * next 10 (or 9) bits and small second-level tables for longer codes. */
#include "arc_int.h"

#define LBITS 10   /* first-level bits, literal/length table */
#define DBITS 9    /* first-level bits, distance table */
#define LSIZE ((1 << LBITS) + 288 * (1 << (15 - LBITS)))
#define DSIZE ((1 << DBITS) + 32 * (1 << (15 - DBITS)))
#define CHUNK 65536 /* output produced per fill */
#define BAD 0xFFFFu

/* Table entry: bits 0-7 bits to drop, bits 8-15 0 for a symbol or the index
 * width of a second-level table, bits 16-31 symbol or second-level offset. */
typedef uint32_t hent;

enum { G_HEAD, G_BODY, G_TRAIL, G_DONE };
enum { B_HEAD, B_STORED, B_HUFF, B_END };

struct infl {
  uint64_t bb;          /* bit buffer, next bit in bit 0 */
  int bc;               /* bits in bb */
  int over;             /* zero bytes invented past the end of input */
  int phase, bstate, final;
  unsigned stored_left;
  uint32_t crc, adler;
  uint64_t size;        /* bytes of this member */
  int members;
  hent lit[LSIZE], dist[DSIZE];
};

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Builds a decoding table from code lengths. Returns 0, or -1 if the lengths
 * describe more codes than fit (over-subscribed). Incomplete codes are
 * accepted; their unused entries decode as BAD and stop the stream. */
static int build(hent *t, int fb, const uint8_t *lens, int n) {
  int count[16] = {0}, next[16];
  for (int i = 0; i < n; i++) count[lens[i]]++;
  count[0] = 0;
  int left = 1;
  for (int b = 1; b < 16; b++) { left <<= 1; left -= count[b]; if (left < 0) return -1; }
  int code = 0;
  next[1] = 0;
  for (int b = 1; b < 16; b++) { code = (code + count[b - 1]) << 1; next[b] = code; }
  next[1] = 0; /* count[0] was cleared, so this is already 0; kept explicit */
  for (int i = 0; i < (1 << fb); i++) t[i] = BAD << 16;
  /* first pass: widths of second-level tables, per first-level slot */
  uint8_t subw[1 << LBITS];
  memset(subw, 0, sizeof subw);
  int nx[16];
  memcpy(nx, next, sizeof nx);
  uint16_t rev[288];
  for (int i = 0; i < n; i++) {
    int l = lens[i];
    if (!l) continue;
    unsigned c = nx[l]++, r = 0;
    for (int k = 0; k < l; k++) { r = r << 1 | (c & 1); c >>= 1; }
    rev[i] = r;
    if (l > fb) { unsigned s = r & ((1u << fb) - 1); if (l - fb > subw[s]) subw[s] = l - fb; }
  }
  int used = 1 << fb;
  for (int s = 0; s < (1 << fb); s++) {
    if (!subw[s]) continue;
    t[s] = (hent)used << 16 | (hent)subw[s] << 8 | fb;
    for (int k = 0; k < (1 << subw[s]); k++) t[used + k] = BAD << 16;
    used += 1 << subw[s];
  }
  for (int i = 0; i < n; i++) {
    int l = lens[i];
    if (!l) continue;
    unsigned r = rev[i];
    if (l <= fb) {
      for (unsigned k = r; k < (1u << fb); k += 1u << l) t[k] = (hent)i << 16 | l;
    } else {
      hent root = t[r & ((1u << fb) - 1)];
      unsigned base = root >> 16, w = (root >> 8) & 0xff, sl = l - fb;
      for (unsigned k = r >> fb; k < (1u << w); k += 1u << sl) t[base + k] = (hent)i << 16 | sl;
    }
  }
  return 0;
}

/* ---- bits ---- */

static void refill_slow(arc_stream *s, struct infl *d) {
  while (d->bc <= 56) {
    int c = in_byte(&s->in);
    if (c < 0) { c = 0; d->over++; }
    d->bb |= (uint64_t)c << d->bc;
    d->bc += 8;
  }
}

static inline void refill(arc_stream *s, struct infl *d) {
  struct ain *in = &s->in;
  if (in->end - in->pos >= 8) {
    d->bb |= ld64(in->buf + in->pos) << d->bc;
    in->pos += (63 - d->bc) >> 3;
    d->bc |= 56;
  } else refill_slow(s, d);
}

static inline unsigned bits(struct infl *d, int n) { /* n <= 32, caller refilled */
  unsigned v = (unsigned)(d->bb & ((1ull << n) - 1));
  d->bb >>= n;
  d->bc -= n;
  return v;
}

/* Gives whole unused bytes in the bit buffer back to the input. */
static int give_back(arc_stream *s, struct infl *d) {
  d->bb >>= d->bc & 7;
  d->bc &= ~7;
  int back = d->bc >> 3;
  if (d->over > back) return arc_fail(s, "unexpected end of compressed data");
  s->in.pos -= back - d->over;
  d->bb = 0;
  d->bc = 0;
  d->over = 0;
  return 0;
}

static inline unsigned decode(struct infl *d, const hent *t, int fb) {
  hent e = t[d->bb & ((1u << fb) - 1)];
  if (e & 0xff00) {
    unsigned w = (e >> 8) & 0xff;
    d->bb >>= fb;
    d->bc -= fb;
    e = t[(e >> 16) + (d->bb & ((1u << w) - 1))];
  }
  d->bb >>= e & 0xff;
  d->bc -= e & 0xff;
  return e >> 16;
}

/* ---- blocks ---- */

static int fixed_tables(struct infl *d) {
  uint8_t l[320];
  int i = 0;
  for (; i < 144; i++) l[i] = 8;
  for (; i < 256; i++) l[i] = 9;
  for (; i < 280; i++) l[i] = 7;
  for (; i < 288; i++) l[i] = 8;
  for (; i < 320; i++) l[i] = 5;
  if (build(d->lit, LBITS, l, 288) < 0 || build(d->dist, DBITS, l + 288, 30) < 0) return -1;
  return 0;
}

static int dynamic_tables(arc_stream *s, struct infl *d) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  refill(s, d);
  int hlit = bits(d, 5) + 257, hdist = bits(d, 5) + 1, hclen = bits(d, 4) + 4;
  if (hlit > 286 || hdist > 30) return arc_fail(s, "corrupt deflate data (bad table sizes)");
  uint8_t cl[19] = {0};
  for (int i = 0; i < hclen; i++) { refill(s, d); cl[order[i]] = bits(d, 3); }
  hent ct[1 << 7];
  if (build(ct, 7, cl, 19) < 0) return arc_fail(s, "corrupt deflate data (bad code lengths)");
  uint8_t lens[286 + 30];
  int n = 0, total = hlit + hdist;
  while (n < total) {
    refill(s, d);
    unsigned sym = decode(d, ct, 7);
    if (sym == BAD) return arc_fail(s, "corrupt deflate data (bad code length code)");
    if (sym < 16) { lens[n++] = sym; continue; }
    int rep, val = 0;
    if (sym == 16) {
      if (n == 0) return arc_fail(s, "corrupt deflate data (repeat with no previous length)");
      val = lens[n - 1];
      rep = 3 + bits(d, 2);
    } else if (sym == 17) rep = 3 + bits(d, 3);
    else rep = 11 + bits(d, 7);
    if (n + rep > total) return arc_fail(s, "corrupt deflate data (lengths overflow)");
    while (rep--) lens[n++] = val;
  }
  if (d->over > 8) return arc_fail(s, "unexpected end of compressed data");
  if (lens[256] == 0) return arc_fail(s, "corrupt deflate data (no end-of-block code)");
  if (build(d->lit, LBITS, lens, hlit) < 0 || build(d->dist, DBITS, lens + hlit, hdist) < 0)
    return arc_fail(s, "corrupt deflate data (over-subscribed code)");
  return 0;
}

/* Decodes until about CHUNK bytes are out or the deflate stream ends.
 * Returns 1 if the stream goes on, 0 at its end, -1 on error. */
static int inflate_some(arc_stream *s, struct infl *d) {
  struct awin *w = &s->w; /* the caller made room for CHUNK bytes */
  uint8_t *base = w->buf;
  size_t pos = w->pos, limit = pos + CHUNK - 258 - WILD;
  for (;;) {
    if (d->bstate == B_HEAD) {
      if (d->final) { d->bstate = B_END; break; }
      refill(s, d);
      d->final = bits(d, 1);
      int type = bits(d, 2);
      if (type == 0) {
        d->bb >>= d->bc & 7;
        d->bc &= ~7;
        refill(s, d);
        unsigned len = bits(d, 16), nlen = bits(d, 16);
        if ((len ^ 0xffff) != nlen) return arc_fail(s, "corrupt deflate data (stored block length)");
        if (give_back(s, d) < 0) return -1;
        d->stored_left = len;
        d->bstate = B_STORED;
      } else if (type == 1) {
        if (fixed_tables(d) < 0) return arc_fail(s, "internal error");
        d->bstate = B_HUFF;
      } else if (type == 2) {
        if (dynamic_tables(s, d) < 0) return -1;
        d->bstate = B_HUFF;
      } else return arc_fail(s, "corrupt deflate data (block type 3)");
      if (d->over > 8) return arc_fail(s, "unexpected end of compressed data");
    }
    if (d->bstate == B_STORED) {
      while (d->stored_left && pos < limit) {
        size_t k = d->stored_left;
        if (k > limit - pos) k = limit - pos;
        size_t got = in_read(&s->in, base + pos, k);
        pos += got;
        d->stored_left -= got;
        if (got < k) { w->pos = pos; return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data"); }
      }
      if (d->stored_left) break;
      d->bstate = B_HEAD;
      continue;
    }
    /* B_HUFF. The bit buffer and input position live in locals here: the
     * byte stores into the window could otherwise alias them and force
     * reloads after every byte. */
    {
      uint64_t bb = d->bb;
      int bc = d->bc;
      struct ain *in = &s->in;
      const uint8_t *ib = in->buf;
      size_t ipos = in->pos, iend = in->end;
      const hent *lt = d->lit, *dt = d->dist;
      const char *bad = 0;
#define SYNC_OUT() do { d->bb = bb; d->bc = bc; in->pos = ipos; } while (0)
#define SYNC_IN() do { bb = d->bb; bc = d->bc; ib = in->buf; ipos = in->pos; iend = in->end; } while (0)
#define LREFILL() do { \
        if (iend - ipos >= 8) { bb |= ld64(ib + ipos) << bc; ipos += (63 - bc) >> 3; bc |= 56; } \
        else { SYNC_OUT(); refill_slow(s, d); SYNC_IN(); } } while (0)
#define LDECODE(t, fb, out) do { \
        hent e_ = (t)[bb & ((1u << (fb)) - 1)]; \
        if (e_ & 0xff00) { unsigned w_ = (e_ >> 8) & 0xff; bb >>= (fb); bc -= (fb); e_ = (t)[(e_ >> 16) + (bb & ((1u << w_) - 1))]; } \
        bb >>= e_ & 0xff; bc -= e_ & 0xff; (out) = e_ >> 16; } while (0)
#define LBITS_(n, out) do { (out) = (unsigned)(bb & ((1ull << (n)) - 1)); bb >>= (n); bc -= (n); } while (0)
      while (pos < limit) {
        unsigned sym;
        LREFILL();
        LDECODE(lt, LBITS, sym);
        if (sym < 256) {
          base[pos++] = sym;
          LDECODE(lt, LBITS, sym);  /* the refill left at least 41 bits: room for one more code */
          if (sym < 256) { base[pos++] = sym; continue; }
        }
        if (sym == 256) { d->bstate = B_HEAD; break; }
        if (sym > 285) { bad = "corrupt deflate data (bad literal/length code)"; break; }
        LREFILL();
        sym -= 257;
        unsigned len, dc, dist, extra;
        LBITS_(len_extra[sym], extra);
        len = len_base[sym] + extra;
        LDECODE(dt, DBITS, dc);
        if (dc >= 30) { bad = "corrupt deflate data (bad distance code)"; break; }
        LBITS_(dist_extra[dc], extra);
        dist = dist_base[dc] + extra;
        if (dist > pos) { bad = "corrupt deflate data (distance too far back)"; break; }
        copy_match_wild(base + pos, dist, len);
        pos += len;
      }
      SYNC_OUT();
#undef SYNC_OUT
#undef SYNC_IN
#undef LREFILL
#undef LDECODE
#undef LBITS_
      if (bad) { w->pos = pos; return arc_fail(s, bad); }
    }
    if (d->over > 8) { w->pos = pos; return arc_fail(s, "unexpected end of compressed data"); }
    if (d->bstate != B_HEAD || pos >= limit) break;
  }
  w->pos = pos;
  if (d->bstate == B_END) return give_back(s, d) < 0 ? -1 : 0;
  return 1;
}

/* ---- wrappers ---- */

static int need_byte(arc_stream *s, int *c) {
  *c = in_byte(&s->in);
  if (*c < 0) return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data");
  return 0;
}

static int gzip_header(arc_stream *s, struct infl *d) {
  uint8_t h[10];
  if (in_read(&s->in, h, 10) != 10) return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data");
  if (h[0] != 0x1f || h[1] != 0x8b) return arc_fail(s, "not in gzip format");
  if (h[2] != 8) return arc_fail(s, "unknown gzip compression method");
  if (h[3] & 0xe0) return arc_fail(s, "gzip header has reserved flags set");
  uint32_t hcrc = arc_crc32(0, h, 10);
  int c;
  if (h[3] & 4) { /* FEXTRA */
    uint8_t x[2];
    if (in_read(&s->in, x, 2) != 2) return arc_fail(s, "unexpected end of compressed data");
    hcrc = arc_crc32(hcrc, x, 2);
    unsigned xlen = rd16le(x);
    while (xlen--) { if (need_byte(s, &c) < 0) return -1; uint8_t b = c; hcrc = arc_crc32(hcrc, &b, 1); }
  }
  for (int f = 8; f <= 16; f <<= 1) /* FNAME, then FCOMMENT: zero-terminated, any length */
    if (h[3] & f) do { if (need_byte(s, &c) < 0) return -1; uint8_t b = c; hcrc = arc_crc32(hcrc, &b, 1); } while (c);
  if (h[3] & 2) { /* FHCRC */
    uint8_t x[2];
    if (in_read(&s->in, x, 2) != 2) return arc_fail(s, "unexpected end of compressed data");
    if (rd16le(x) != (hcrc & 0xffff)) return arc_fail(s, "gzip header checksum mismatch");
  }
  (void)d;
  return 0;
}

static void member_reset(arc_stream *s, struct infl *d) {
  win_reset(&s->w, 32768, CHUNK);
  d->bb = 0; d->bc = 0; d->over = 0;
  d->bstate = B_HEAD; d->final = 0;
  d->crc = 0; d->adler = 1; d->size = 0;
}

static int inflate_fill(arc_stream *s) {
  struct infl *d = s->dec;
  for (;;) {
    switch (d->phase) {
    case G_HEAD:
      member_reset(s, d);
      if (s->fmt == ARC_GZIP) {
        if (gzip_header(s, d) < 0) return -1;
      } else if (s->fmt == ARC_ZLIB) {
        uint8_t h[2];
        if (in_read(&s->in, h, 2) != 2) return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data");
        if ((h[0] & 15) != 8 || (h[0] >> 4) > 7 || ((unsigned)h[0] << 8 | h[1]) % 31)
          return arc_fail(s, "not in zlib format");
        if (h[1] & 0x20) return arc_fail(s, "zlib stream needs a preset dictionary (not supported)");
      }
      d->phase = G_BODY;
      d->members++;
      break;
    case G_BODY: {
      if (win_need(&s->w, CHUNK) < 0) return arc_fail(s, "out of memory");
      size_t start = s->w.pos;
      int r = inflate_some(s, d);
      size_t n = s->w.pos - start;
      if (s->fmt == ARC_GZIP) d->crc = arc_crc32(d->crc, s->w.buf + start, n);
      else if (s->fmt == ARC_ZLIB) d->adler = arc_adler32(d->adler, s->w.buf + start, n);
      d->size += n;
      if (r < 0) return -1;
      if (r == 0) d->phase = G_TRAIL;
      if (n) return 1;
      break;
    }
    case G_TRAIL:
      if (s->fmt == ARC_GZIP) {
        uint8_t t[8];
        if (in_read(&s->in, t, 8) != 8) return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data");
        if (rd32le(t) != d->crc) return arc_fail(s, "gzip CRC32 mismatch: the data is corrupt");
        if (rd32le(t + 4) != (uint32_t)d->size) return arc_fail(s, "gzip length mismatch: the data is corrupt");
        /* another member may follow (concatenated .gz files) */
        size_t n = in_avail(&s->in, 2);
        if (s->in.err) return arc_fail(s, "read error");
        if (n == 0) { d->phase = G_DONE; return 0; }
        if (n >= 2 && s->in.buf[s->in.pos] == 0x1f && s->in.buf[s->in.pos + 1] == 0x8b) { d->phase = G_HEAD; break; }
        return arc_fail(s, "trailing garbage after gzip data");
      }
      if (s->fmt == ARC_ZLIB) {
        uint8_t t[4];
        if (in_read(&s->in, t, 4) != 4) return arc_fail(s, s->in.err ? "read error" : "unexpected end of compressed data");
        if (rd32be(t) != d->adler) return arc_fail(s, "zlib Adler-32 mismatch: the data is corrupt");
      }
      d->phase = G_DONE;
      return 0;
    default:
      return 0;
    }
  }
}

int inflate_start(arc_stream *s) {
  struct infl *d = calloc(1, sizeof *d);
  if (!d) return arc_fail(s, "out of memory");
  s->dec = d;
  s->fill = inflate_fill;
  d->phase = G_HEAD;
  return 0;
}
