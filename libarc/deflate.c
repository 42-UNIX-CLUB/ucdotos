// SPDX-License-Identifier: GPL-2.0-or-later
/* Deflate encoder (RFC 1951) and the push-stream writer with the gzip and
 * zlib wrappers.
 *
 * Matching: a hash of the next three bytes indexes chains of earlier
 * positions in a 32 KiB window. Levels 1 to 3 take the first good match
 * (greedy); levels 4 to 9 also try the next position before committing (lazy
 * matching), and search longer chains as the level rises.
 * Blocks: symbols are collected until a buffer fills, then the block is
 * written in whichever form is smallest: stored, fixed Huffman or dynamic
 * Huffman with length-limited codes built from the block's own counts. */
#include "arc_int.h"
#include <unistd.h>

#define WSIZE 32768
#define WMASK (WSIZE - 1)
#define HBITS 15
#define HSIZE (1 << HBITS)
#define MINLOOK 262         /* longest match plus the three bytes the hash needs, plus one */
#define NSYM 16384          /* symbols per block */
#define OBUF 65536
#define NIL (-1)

struct level { uint16_t chain, nice, lazy, good; };
static const struct level levels[10] = {
  {0, 0, 0, 0},                                    /* 0: stored */
  {4, 8, 0, 4}, {8, 16, 0, 8}, {24, 32, 0, 16},     /* 1-3 greedy */
  {16, 32, 8, 8}, {32, 64, 16, 16}, {128, 128, 32, 16}, /* 4-6 lazy */
  {256, 192, 128, 32}, {1024, 258, 258, 32}, {4096, 258, 258, 32}}; /* 7-9 */

struct arc_writer {
  int fmt, level, failed, finished, started;
  arc_sink sink;
  void *ctx;
  char err[96];
  uint64_t tin, tout;
  uint32_t crc, adler;
  char *gz_name;
  unsigned gz_mtime;
  /* output */
  uint8_t obuf[OBUF];
  size_t on;
  uint64_t bb;
  int bc;
  /* matcher */
  uint8_t *win;            /* 2 * WSIZE */
  int32_t *head, *prev;    /* HSIZE, WSIZE */
  int wpos, wend;
  int have_prev, prev_len, prev_dist;
  /* current block */
  uint16_t *sdist;         /* 0 for a literal */
  uint16_t *slen;          /* literal byte, or match length */
  int nsym;
  int bstart;              /* window index where the block's input starts; < 0 once slid out */
  int bbytes;
  int epos;                /* window index of the next byte to be emitted as a symbol */
  uint32_t lfreq[286], dfreq[30];
  /* tar writer state */
  uint64_t tar_left;
  unsigned tar_pad;
};

static int wfail(arc_writer *w, const char *m) {
  if (!w->err[0]) { strncpy(w->err, m, sizeof w->err - 1); }
  w->failed = 1;
  return -1;
}

/* ---- output ---- */

static int oflush(arc_writer *w) {
  if (w->failed) return -1;
  size_t off = 0;
  while (off < w->on) {
    long r = w->sink(w->ctx, w->obuf + off, w->on - off);
    if (r <= 0) return wfail(w, "write error");
    off += r;
  }
  w->tout += w->on;
  w->on = 0;
  return 0;
}

static void obytes(arc_writer *w, const void *p, size_t n) {
  const uint8_t *s = p;
  while (n) {
    if (w->on == OBUF && oflush(w) < 0) return;
    size_t k = OBUF - w->on;
    if (k > n) k = n;
    memcpy(w->obuf + w->on, s, k);
    w->on += k; s += k; n -= k;
  }
}

static inline void put(arc_writer *w, uint32_t v, int n) { /* n <= 32 */
  w->bb |= (uint64_t)v << w->bc;
  w->bc += n;
  if (w->bc >= 32) {
    if (w->on + 4 > OBUF && oflush(w) < 0) { w->bc -= 32; w->bb >>= 32; return; }
    wr32le(w->obuf + w->on, (uint32_t)w->bb);
    w->on += 4;
    w->bb >>= 32;
    w->bc -= 32;
  }
}

static void align(arc_writer *w) {
  while (w->bc > 0) {
    if (w->on == OBUF && oflush(w) < 0) return;
    w->obuf[w->on++] = (uint8_t)w->bb;
    w->bb >>= 8;
    w->bc = w->bc > 8 ? w->bc - 8 : 0;
  }
  w->bb = 0;
}

/* ---- Huffman code lengths ---- */

/* Code lengths for n symbols with the given counts, no longer than limit.
 * Optimal Huffman lengths are computed first, then if needed reshaped to
 * respect the limit while keeping the code complete, then handed out again
 * so the most frequent symbols get the shortest codes. */
static void make_lengths(const uint32_t *freq, int n, int limit, uint8_t *len) {
  int sym[320], m = 0;
  memset(len, 0, n);
  for (int i = 0; i < n; i++) if (freq[i]) sym[m++] = i;
  if (m == 0) { len[0] = len[1] = 1; return; }
  if (m == 1) { len[sym[0]] = 1; len[sym[0] ? 0 : 1] = 1; return; }
  /* sort by count, ascending (insertion sort: n is at most 286) */
  for (int i = 1; i < m; i++) {
    int s = sym[i], j = i;
    while (j > 0 && freq[sym[j - 1]] > freq[s]) { sym[j] = sym[j - 1]; j--; }
    sym[j] = s;
  }
  /* two queues: leaves in order, then internal nodes as they are made */
  uint64_t w[640];
  int parent[640];
  for (int i = 0; i < m; i++) w[i] = freq[sym[i]];
  int li = 0, ni = m, nn = m;
  for (int k = 0; k < m - 1; k++) {
    int a, b;
    a = (li < m && (ni >= nn || w[li] <= w[ni])) ? li++ : ni++;
    b = (li < m && (ni >= nn || w[li] <= w[ni])) ? li++ : ni++;
    w[nn] = w[a] + w[b];
    parent[a] = parent[b] = nn;
    nn++;
  }
  int depth[640], count[320] = {0}, maxd = 0;
  depth[nn - 1] = 0;
  for (int i = nn - 2; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
  for (int i = 0; i < m; i++) { count[depth[i]]++; if (depth[i] > maxd) maxd = depth[i]; }
  /* enforce the limit: move pairs of leaves up from the deepest level */
  for (int i = maxd; i > limit; i--) {
    while (count[i] > 0) {
      int j = i - 2;
      while (count[j] == 0) j--;
      count[i] -= 2;
      count[i - 1] += 1;
      count[j + 1] += 2;
      count[j] -= 1;
    }
  }
  /* most frequent symbols get the shortest lengths */
  int d = 1, k = m - 1;
  for (; d <= limit && k >= 0; d++)
    for (int c = count[d]; c > 0; c--) len[sym[k--]] = d;
}

static void make_codes(const uint8_t *len, int n, uint16_t *code) {
  int count[16] = {0}, next[16];
  for (int i = 0; i < n; i++) count[len[i]]++;
  count[0] = 0;
  int c = 0;
  for (int b = 1; b < 16; b++) { c = (c + count[b - 1]) << 1; next[b] = c; }
  for (int i = 0; i < n; i++) {
    if (!len[i]) continue;
    unsigned v = next[len[i]]++, r = 0;
    for (int k = 0; k < len[i]; k++) { r = r << 1 | (v & 1); v >>= 1; }
    code[i] = r;
  }
}

/* ---- blocks ---- */

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static uint8_t len_code[259];   /* match length -> code - 257 */
static uint8_t dist_code[512];  /* see dcode() */
static int tables_ready;

static void init_tables(void) {
  for (int c = 0; c < 29; c++)
    for (int l = len_base[c]; l < len_base[c] + (1 << len_extra[c]) && l <= 258; l++) len_code[l] = c;
  len_code[258] = 28;
  for (int c = 0; c < 30; c++)
    for (int dd = dist_base[c]; dd < dist_base[c] + (1 << dist_extra[c]); dd++) {
      if (dd <= 256) dist_code[dd - 1] = c;
      else dist_code[256 + ((dd - 1) >> 7)] = c;
    }
  tables_ready = 1;
}
static inline int dcode(int dist) { return dist <= 256 ? dist_code[dist - 1] : dist_code[256 + ((dist - 1) >> 7)]; }

static void write_symbols(arc_writer *w, const uint8_t *ll, const uint16_t *lc, const uint8_t *dl, const uint16_t *dc) {
  for (int i = 0; i < w->nsym; i++) {
    int dist = w->sdist[i], v = w->slen[i];
    if (!dist) { put(w, lc[v], ll[v]); continue; }
    int c = len_code[v];
    put(w, lc[257 + c], ll[257 + c]);
    if (len_extra[c]) put(w, v - len_base[c], len_extra[c]);
    int d = dcode(dist);
    put(w, dc[d], dl[d]);
    if (dist_extra[d]) put(w, dist - dist_base[d], dist_extra[d]);
  }
  put(w, lc[256], ll[256]);
}

static void flush_block(arc_writer *w, int final) {
  w->lfreq[256] = 1;
  /* cost of each form, in bits */
  uint64_t extra = 0;
  for (int c = 0; c < 29; c++) extra += (uint64_t)w->lfreq[257 + c] * len_extra[c];
  for (int c = 0; c < 30; c++) extra += (uint64_t)w->dfreq[c] * dist_extra[c];

  uint8_t fl[288], fd[30];
  for (int i = 0; i < 288; i++) fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
  for (int i = 0; i < 30; i++) fd[i] = 5;
  uint64_t fixed = 3 + extra;
  for (int i = 0; i < 286; i++) fixed += (uint64_t)w->lfreq[i] * fl[i];
  for (int i = 0; i < 30; i++) fixed += (uint64_t)w->dfreq[i] * fd[i];

  uint8_t dl_lit[286], dl_dist[30];
  make_lengths(w->lfreq, 286, 15, dl_lit);
  make_lengths(w->dfreq, 30, 15, dl_dist);
  int hlit = 286, hdist = 30;
  while (hlit > 257 && !dl_lit[hlit - 1]) hlit--;
  while (hdist > 1 && !dl_dist[hdist - 1]) hdist--;
  /* run-length code the lengths with symbols 16, 17, 18 */
  uint8_t all[316], rs[316], rx[316];
  int na = 0, nr = 0;
  for (int i = 0; i < hlit; i++) all[na++] = dl_lit[i];
  for (int i = 0; i < hdist; i++) all[na++] = dl_dist[i];
  uint32_t cfreq[19] = {0};
  for (int i = 0; i < na;) {
    int v = all[i], run = 1;
    while (i + run < na && all[i + run] == v) run++;
    if (v == 0 && run >= 3) {
      int r = run > 138 ? 138 : run;
      if (r >= 11) { rs[nr] = 18; rx[nr++] = r - 11; } else { rs[nr] = 17; rx[nr++] = r - 3; }
      i += r;
    } else if (v != 0 && run >= 4) {
      rs[nr] = v; rx[nr++] = 0;
      int r = run - 1 > 6 ? 6 : run - 1;
      rs[nr] = 16; rx[nr++] = r - 3;
      i += 1 + r;
    } else { rs[nr] = v; rx[nr++] = 0; i++; }
  }
  for (int i = 0; i < nr; i++) cfreq[rs[i]]++;
  uint8_t cl[19];
  make_lengths(cfreq, 19, 7, cl);
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  int hclen = 19;
  while (hclen > 4 && !cl[order[hclen - 1]]) hclen--;
  uint64_t dyn = 3 + 14 + 3 * hclen + extra;
  for (int i = 0; i < nr; i++) dyn += cl[rs[i]] + (rs[i] == 16 ? 2 : rs[i] == 17 ? 3 : rs[i] == 18 ? 7 : 0);
  for (int i = 0; i < 286; i++) dyn += (uint64_t)w->lfreq[i] * dl_lit[i];
  for (int i = 0; i < 30; i++) dyn += (uint64_t)w->dfreq[i] * dl_dist[i];

  uint64_t stored = ~0ull;
  if (w->bstart >= 0) {
    int pieces = w->bbytes ? (w->bbytes + 65534) / 65535 : 1;
    stored = (uint64_t)w->bbytes * 8 + pieces * 40 + 7;
  }
  if (w->level == 0 && w->bstart >= 0) fixed = dyn = ~0ull;

  if (stored <= fixed && stored <= dyn) {
    int left = w->bbytes, p = w->bstart;
    do {
      int k = left > 65535 ? 65535 : left;
      left -= k;
      put(w, final && left == 0, 1);
      put(w, 0, 2);
      align(w);
      uint8_t h[4] = {(uint8_t)k, (uint8_t)(k >> 8), (uint8_t)~k, (uint8_t)(~k >> 8)};
      obytes(w, h, 4);
      obytes(w, w->win + p, k);
      p += k;
    } while (left > 0);
  } else if (fixed <= dyn) {
    uint16_t lc[288], dc[30];
    make_codes(fl, 288, lc);
    make_codes(fd, 30, dc);
    put(w, final, 1);
    put(w, 1, 2);
    write_symbols(w, fl, lc, fd, dc);
  } else {
    uint16_t lc[286], dc[30], cc[19];
    make_codes(dl_lit, 286, lc);
    make_codes(dl_dist, 30, dc);
    make_codes(cl, 19, cc);
    put(w, final, 1);
    put(w, 2, 2);
    put(w, hlit - 257, 5);
    put(w, hdist - 1, 5);
    put(w, hclen - 4, 4);
    for (int i = 0; i < hclen; i++) put(w, cl[order[i]], 3);
    for (int i = 0; i < nr; i++) {
      put(w, cc[rs[i]], cl[rs[i]]);
      if (rs[i] == 16) put(w, rx[i], 2);
      else if (rs[i] == 17) put(w, rx[i], 3);
      else if (rs[i] == 18) put(w, rx[i], 7);
    }
    write_symbols(w, dl_lit, lc, dl_dist, dc);
  }
  w->nsym = 0;
  w->bstart = w->epos;
  w->bbytes = 0;
  memset(w->lfreq, 0, sizeof w->lfreq);
  memset(w->dfreq, 0, sizeof w->dfreq);
}

static inline void emit_lit(arc_writer *w, int c) {
  w->sdist[w->nsym] = 0;
  w->slen[w->nsym++] = c;
  w->lfreq[c]++;
  w->bbytes++;
  w->epos++;
  if (w->nsym == NSYM) flush_block(w, 0);
}

static inline void emit_match(arc_writer *w, int len, int dist) {
  w->sdist[w->nsym] = dist;
  w->slen[w->nsym++] = len;
  w->lfreq[257 + len_code[len]]++;
  w->dfreq[dcode(dist)]++;
  w->bbytes += len;
  w->epos += len;
  if (w->nsym == NSYM) flush_block(w, 0);
}

/* ---- matcher ---- */

static inline uint32_t hash3(const uint8_t *p) { return (rd24le(p) * 2654435761u) >> (32 - HBITS); }

static inline void insert(arc_writer *w, int pos) {
  uint32_t h = hash3(w->win + pos);
  w->prev[pos & WMASK] = w->head[h];
  w->head[h] = pos;
}

/* Longest match at pos better than `best`; returns its length, sets *dist. */
static int longest(arc_writer *w, int pos, int best, int maxlen, int *dist) {
  const struct level *lv = &levels[w->level];
  int chain = lv->chain, nice = lv->nice < maxlen ? lv->nice : maxlen;
  if (best >= lv->good) chain >>= 2;
  const uint8_t *cur = w->win + pos;
  int limit = pos > WSIZE ? pos - WSIZE : 0, found = 0;
  int cand = w->prev[pos & WMASK];
  while (cand >= limit && cand != NIL && chain-- > 0) {
    const uint8_t *m = w->win + cand;
    if (best < maxlen && m[best] == cur[best] && m[0] == cur[0] && m[1] == cur[1]) {
      int l = 2;
      while (l < maxlen && m[l] == cur[l]) l++;
      if (l > best) { best = l; *dist = pos - cand; found = l; if (l >= nice) break; }
    }
    int nx = w->prev[cand & WMASK];
    if (nx >= cand) break; /* stale entry from before a slide */
    cand = nx;
  }
  return found >= 3 ? found : 0;
}

static void slide(arc_writer *w) {
  if (w->level == 0 && w->nsym) flush_block(w, 0); /* keep stored blocks possible */
  memmove(w->win, w->win + WSIZE, WSIZE);
  w->wpos -= WSIZE;
  w->wend -= WSIZE;
  w->bstart = w->bstart >= WSIZE ? w->bstart - WSIZE : -1;
  w->epos -= WSIZE;
  for (int i = 0; i < HSIZE; i++) w->head[i] = w->head[i] >= WSIZE ? w->head[i] - WSIZE : NIL;
  for (int i = 0; i < WSIZE; i++) w->prev[i] = w->prev[i] >= WSIZE ? w->prev[i] - WSIZE : NIL;
}

static void process(arc_writer *w, int flush) {
  const struct level *lv = &levels[w->level];
  for (;;) {
    int avail = w->wend - w->wpos;
    if (avail == 0 || (!flush && avail < MINLOOK)) break;
    int pos = w->wpos, maxlen = avail < 258 ? avail : 258;
    if (w->level == 0) { emit_lit(w, w->win[pos]); w->wpos++; continue; }
    int len = 0, dist = 0;
    if (avail >= 3) {
      insert(w, pos);
      if (!lv->lazy || w->prev_len < lv->lazy) len = longest(w, pos, w->have_prev ? w->prev_len : 2, maxlen, &dist);
    }
    if (!lv->lazy) {
      if (len >= 3) {
        emit_match(w, len, dist);
        int end = pos + len;
        if (len <= lv->nice || w->level > 1)
          for (int p = pos + 1; p < end && p + 2 < w->wend; p++) insert(w, p);
        w->wpos = end;
      } else { emit_lit(w, w->win[pos]); w->wpos++; }
      continue;
    }
    if (w->have_prev && w->prev_len >= 3 && len <= w->prev_len) {
      /* the match found one byte earlier is at least as good: take it */
      int start = pos - 1, end = start + w->prev_len;
      emit_match(w, w->prev_len, w->prev_dist);
      for (int p = pos + 1; p < end && p + 2 < w->wend; p++) insert(w, p);
      w->wpos = end;
      w->have_prev = 0;
      w->prev_len = 0;
    } else {
      if (w->have_prev) emit_lit(w, w->win[pos - 1]);
      w->have_prev = 1;
      w->prev_len = len;
      w->prev_dist = dist;
      w->wpos = pos + 1;
    }
  }
}

/* ---- writer ---- */

arc_writer *arc_wopen(int format, int level, arc_sink sink, void *ctx) {
  if (format != ARC_GZIP && format != ARC_ZLIB && format != ARC_DEFLATE && format != ARC_RAW) return 0;
  if (level < 0) level = 6;
  if (level > 9) level = 9;
  arc_writer *w = calloc(1, sizeof *w);
  if (!w) return 0;
  w->fmt = format;
  w->level = level;
  w->sink = sink;
  w->ctx = ctx;
  w->adler = 1;
  if (format != ARC_RAW) {
    w->win = malloc(2 * WSIZE);
    w->head = malloc(HSIZE * sizeof *w->head);
    w->prev = malloc(WSIZE * sizeof *w->prev);
    w->sdist = malloc(NSYM * sizeof *w->sdist);
    w->slen = malloc(NSYM * sizeof *w->slen);
    if (!w->win || !w->head || !w->prev || !w->sdist || !w->slen) { w->finished = 1; arc_wclose(w); return 0; }
    for (int i = 0; i < HSIZE; i++) w->head[i] = NIL;
    for (int i = 0; i < WSIZE; i++) w->prev[i] = NIL;
    if (!tables_ready) init_tables();
  }
  return w;
}

static long fd_sink(void *ctx, const void *buf, unsigned long n) {
  long r = write((int)(intptr_t)ctx, buf, n);
  return r < 0 ? -1 : r;
}

arc_writer *arc_wopen_fd(int format, int level, int fd) { return arc_wopen(format, level, fd_sink, (void *)(intptr_t)fd); }

void arc_wgzip_info(arc_writer *w, const char *name, unsigned mtime) {
  free(w->gz_name);
  w->gz_name = name ? strdup(name) : 0;
  w->gz_mtime = mtime;
}

static void header(arc_writer *w) {
  w->started = 1;
  if (w->fmt == ARC_GZIP) {
    uint8_t h[10] = {0x1f, 0x8b, 8, w->gz_name ? 8 : 0, 0, 0, 0, 0, w->level == 9 ? 2 : w->level == 1 ? 4 : 0, 3};
    wr32le(h + 4, w->gz_mtime);
    obytes(w, h, 10);
    if (w->gz_name) obytes(w, w->gz_name, strlen(w->gz_name) + 1);
  } else if (w->fmt == ARC_ZLIB) {
    int fl = w->level < 2 ? 0 : w->level < 6 ? 1 : w->level == 6 ? 2 : 3;
    uint8_t h[2] = {0x78, (uint8_t)(fl << 6)};
    h[1] += 31 - ((0x78 << 8 | h[1]) % 31);
    obytes(w, h, 2);
  }
}

long arc_write(arc_writer *w, const void *buf, unsigned long n) {
  if (w->failed) return -1;
  if (w->finished) return wfail(w, "write after finish");
  if (!w->started) header(w);
  const uint8_t *p = buf;
  w->tin += n;
  if (w->fmt == ARC_RAW) { obytes(w, p, n); return w->failed ? -1 : (long)n; }
  if (w->fmt == ARC_GZIP) w->crc = arc_crc32(w->crc, p, n);
  if (w->fmt == ARC_ZLIB) w->adler = arc_adler32(w->adler, p, n);
  unsigned long left = n;
  while (left) {
    if (w->wend == 2 * WSIZE) slide(w);
    size_t k = 2 * WSIZE - w->wend;
    if (k > left) k = left;
    memcpy(w->win + w->wend, p, k);
    w->wend += k; p += k; left -= k;
    process(w, 0);
    if (w->failed) return -1;
  }
  return (long)n;
}

int arc_wfinish(arc_writer *w) {
  if (w->finished) return w->failed ? -1 : 0;
  if (!w->started) header(w);
  if (w->fmt != ARC_RAW) {
    process(w, 1);
    if (w->have_prev) { emit_lit(w, w->win[w->wend - 1]); w->have_prev = 0; }
    flush_block(w, 1);
    align(w);
    uint8_t t[8];
    if (w->fmt == ARC_GZIP) { wr32le(t, w->crc); wr32le(t + 4, (uint32_t)w->tin); obytes(w, t, 8); }
    if (w->fmt == ARC_ZLIB) { wr32be(t, w->adler); obytes(w, t, 4); }
  }
  w->finished = 1;
  if (w->on) oflush(w);
  return w->failed ? -1 : 0;
}

int arc_wclose(arc_writer *w) {
  if (!w) return -1;
  int r = arc_wfinish(w);
  free(w->win); free(w->head); free(w->prev); free(w->sdist); free(w->slen); free(w->gz_name);
  free(w);
  return r;
}

const char *arc_werror(arc_writer *w) { return w ? w->err : "out of memory"; }
unsigned long long arc_wtotal_in(arc_writer *w) { return w->tin; }
unsigned long long arc_wtotal_out(arc_writer *w) { return w->tout + w->on; }

/* for tar.c: the tar writer keeps its state in the writer */
uint64_t *arc__tar_left(arc_writer *w) { return &w->tar_left; }
unsigned *arc__tar_pad(arc_writer *w) { return &w->tar_pad; }
