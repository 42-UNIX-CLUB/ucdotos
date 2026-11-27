// SPDX-License-Identifier: GPL-2.0-or-later
/* The decompression stream: input buffering, the output window, format
 * detection and the public arc_* calls. */
#include "arc_int.h"
#include <unistd.h>

/* ---- input ---- */

int in_fill(struct ain *in) {
  if (in->err) return -1;
  if (in->eof) return 0;
  size_t keep = in->pos < IN_KEEP ? in->pos : IN_KEEP;
  size_t live = in->end - in->pos;
  if (in->pos - keep > 0) memmove(in->buf, in->buf + in->pos - keep, keep + live);
  in->pos = keep;
  in->end = keep + live;
  if (in->end >= IN_SIZE) return 0;   /* full already: caller asked for more than fits */
  long r = in->src(in->ctx, in->buf + in->end, IN_SIZE - in->end);
  if (r < 0) { in->err = 1; return -1; }
  if (r == 0) { in->eof = 1; return 0; }
  if ((unsigned long)r > IN_SIZE - in->end) { in->err = 1; return -1; } /* broken callback */
  in->end += r;
  in->got += r;
  return (int)r;
}

int in_byte(struct ain *in) {
  if (in->pos == in->end && in_fill(in) <= 0) return -1;
  return in->buf[in->pos++];
}

size_t in_read(struct ain *in, void *dst, size_t n) {
  size_t done = 0;
  while (done < n) {
    if (in->pos == in->end && in_fill(in) <= 0) break;
    size_t k = in->end - in->pos;
    if (k > n - done) k = n - done;
    memcpy((uint8_t *)dst + done, in->buf + in->pos, k);
    in->pos += k;
    done += k;
  }
  return done;
}

size_t in_avail(struct ain *in, size_t n) {
  while (in->end - in->pos < n && in_fill(in) > 0) {}
  return in->end - in->pos;
}

/* ---- output window ---- */

void win_reset(struct awin *w, size_t dict, size_t unit) {
  w->dict = dict;
  size_t chunk = dict / 4;
  if (chunk < 65536) chunk = 65536;
  w->max = dict + chunk + unit;
  w->pos = w->rd = 0;
}

int win_need(struct awin *w, size_t need) {
  if (w->cap - w->pos >= need) return 0;
  if (w->cap < w->max) {
    size_t ncap = w->cap ? w->cap * 2 : 65536;
    if (ncap < w->pos + need) ncap = w->pos + need;
    if (ncap > w->max) ncap = w->max;
    if (ncap >= w->pos + need) {
      uint8_t *nb = realloc(w->buf, ncap);
      if (!nb) return -1;
      w->buf = nb;
      w->cap = ncap;
      return 0;
    }
  }
  size_t keep = w->pos < w->dict ? w->pos : w->dict;
  memmove(w->buf, w->buf + w->pos - keep, keep);
  w->pos = w->rd = keep;
  if (w->cap - w->pos < need) { /* only if max was lowered below what is kept; grow to fit */
    uint8_t *nb = realloc(w->buf, w->pos + need);
    if (!nb) return -1;
    w->buf = nb;
    w->cap = w->pos + need;
  }
  return 0;
}

/* ---- the stream ---- */

int arc_fail(arc_stream *s, const char *msg) {
  if (!s->err[0]) {
    size_t n = strlen(msg);
    if (n >= sizeof s->err) n = sizeof s->err - 1;
    memcpy(s->err, msg, n);
    s->err[n] = 0;
  }
  s->failed = 1;
  return -1;
}

static int raw_fill(arc_stream *s) { (void)s; return 0; } /* raw streams bypass the window */

arc_stream *arc_open(int format, arc_src src, void *ctx) {
  arc_stream *s = calloc(1, sizeof *s);
  if (!s) return 0;
  s->in.buf = malloc(IN_SIZE);
  if (!s->in.buf) { free(s); return 0; }
  s->in.src = src;
  s->in.ctx = ctx;
  s->want = format;
  s->fmt = ARC_AUTO;
  s->memlimit = ARC_MEMLIMIT_DEFAULT;
  return s;
}

void arc_set_memlimit(arc_stream *s, unsigned long long bytes) { s->memlimit = bytes; }
int arc_format(arc_stream *s) { return s->fmt; }
unsigned long long arc_total_in(arc_stream *s) { return s->in.got - (s->in.end - s->in.pos); }
unsigned long long arc_total_out(arc_stream *s) { return s->out_total; }
const char *arc_error(arc_stream *s) { return s ? s->err : "out of memory"; }

static int detect(arc_stream *s) {
  int f = s->want;
  if (f == ARC_AUTO) {
    size_t n = in_avail(&s->in, 10);
    if (s->in.err) return arc_fail(s, "read error");
    const uint8_t *p = s->in.buf + s->in.pos;
    f = ARC_RAW;
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) f = ARC_GZIP;
    else if (n >= 6 && !memcmp(p, "\xfd" "7zXZ\0", 6)) f = ARC_XZ;
    else if (n >= 4 && rd32le(p) == 0xFD2FB528u) f = ARC_ZSTD;
    else if (n >= 4 && (rd32le(p) & 0xFFFFFFF0u) == 0x184D2A50u) f = ARC_ZSTD; /* skippable frame first */
    else if (n >= 10 && p[0] == 'B' && p[1] == 'Z' && p[2] == 'h' && p[3] >= '1' && p[3] <= '9' &&
             !memcmp(p + 4, "\x31\x41\x59\x26\x53\x59", 6))
      return arc_fail(s, "bzip2 data is not supported");
  }
  s->fmt = f;
  switch (f) {
  case ARC_RAW: s->fill = raw_fill; return 0;
  case ARC_GZIP: case ARC_ZLIB: case ARC_DEFLATE: return inflate_start(s);
  case ARC_XZ: return xz_start(s);
  case ARC_ZSTD: return zstd_start(s);
  default: return arc_fail(s, "unknown format");
  }
}

long arc_read(arc_stream *s, void *buf, unsigned long n) {
  if (!s->fill) {
    if (s->failed) return -1;
    if (detect(s) < 0) return -1;
  }
  if (n == 0) return 0;
  if (s->fmt == ARC_RAW) {
    if (s->failed) return -1;
    size_t k = in_read(&s->in, buf, n);
    if (k == 0 && s->in.err) return arc_fail(s, "read error");
    s->out_total += k;
    return (long)k;
  }
  for (;;) {
    struct awin *w = s->ow ? s->ow : &s->w;
    if (w->rd < w->pos) {
      size_t k = w->pos - w->rd;
      if (k > n) k = n;
      memcpy(buf, w->buf + w->rd, k);
      w->rd += k;
      s->out_total += k;
      return (long)k;
    }
    if (s->failed) return -1;
    if (s->done) return 0;
    int r = s->fill(s);
    if (r < 0) { if (!s->err[0]) arc_fail(s, "corrupt data"); s->failed = 1; }
    else if (r == 0) s->done = 1;
  }
}

void arc_close(arc_stream *s) {
  if (!s) return;
  if (s->dec_free) s->dec_free(s->dec);
  else free(s->dec);
  free(s->w.buf);
  free(s->in.buf);
  free(s);
}

static long fd_src(void *ctx, void *buf, unsigned long n) {
  long r = read((int)(intptr_t)ctx, buf, n);
  return r < 0 ? -1 : r;
}

arc_stream *arc_open_fd(int format, int fd) { return arc_open(format, fd_src, (void *)(intptr_t)fd); }
