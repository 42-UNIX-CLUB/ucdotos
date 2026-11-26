// SPDX-License-Identifier: GPL-2.0-or-later
/* tar reader and writer.
 *
 * Reader: v7, ustar (with the prefix field), GNU ('L' long names, 'K' long
 * links, base-256 numbers) and pax ('x' records for path, linkpath, size,
 * uid, gid, uname, gname, mtime). Global pax headers ('g') are skipped. Sparse
 * files (GNU 'S' and pax GNU.sparse) and multi-volume archives are refused.
 * Every header checksum is verified. A value that does not fit tar_entry is
 * an error, never truncated, so a long name cannot silently change meaning.
 *
 * Writer: the "gnu" format GNU tar writes by default. */
#include "arc_int.h"

#define PAX_MAX (1u << 20)   /* largest extended header accepted */

/* ---- reading ---- */

static long read_full(arc_stream *s, void *buf, unsigned long n) {
  unsigned long got = 0;
  while (got < n) {
    long r = arc_read(s, (uint8_t *)buf + got, n - got);
    if (r < 0) return -1;
    if (r == 0) break;
    got += r;
  }
  return (long)got;
}

static int discard(arc_stream *s, uint64_t n) {
  uint8_t tmp[4096];
  while (n) {
    unsigned long k = n < sizeof tmp ? (unsigned long)n : sizeof tmp;
    long r = read_full(s, tmp, k);
    if (r < 0) return -1;
    if (r != (long)k) return arc_fail(s, "unexpected end of tar archive");
    n -= k;
  }
  return 0;
}

/* Numeric header field: octal, or GNU base-256 when the top bit is set. */
static int num(const uint8_t *f, int len, uint64_t *v, int64_t *sv) {
  if (f[0] & 0x80) {
    uint64_t r = f[0] == 0xff ? ~0ull : 0;
    if (f[0] != 0x80 && f[0] != 0xff) return -1;
    for (int i = 1; i < len; i++) {
      if (f[0] == 0x80 && (r >> 56)) return -1;
      r = r << 8 | f[i];
    }
    if (v) { if (f[0] == 0xff) return -1; *v = r; }
    if (sv) *sv = (int64_t)r;
    return 0;
  }
  int i = 0;
  uint64_t r = 0;
  while (i < len && f[i] == ' ') i++;
  for (; i < len && f[i] >= '0' && f[i] <= '7'; i++) {
    if (r >> 60) return -1;
    r = r << 3 | (f[i] - '0');
  }
  for (; i < len; i++) if (f[i] != ' ' && f[i] != 0) return -1;
  if (v) *v = r;
  if (sv) *sv = (int64_t)r;
  return 0;
}

static int checksum_ok(const uint8_t *h) {
  uint64_t want;
  if (num(h + 148, 8, &want, 0) < 0) return 0;
  unsigned u = 0;
  int sgn = 0;
  for (int i = 0; i < 512; i++) {
    int c = (i >= 148 && i < 156) ? ' ' : h[i];
    u += (unsigned)c;
    sgn += (i >= 148 && i < 156) ? ' ' : (signed char)h[i];
  }
  return want == u || (int64_t)want == sgn; /* some old writers summed signed bytes */
}

/* Copies a NUL-terminated or full-width field into dst (size cap). */
static int str_field(char *dst, size_t cap, const uint8_t *f, size_t len) {
  size_t n = strnlen((const char *)f, len);
  if (n >= cap) return -1;
  memcpy(dst, f, n);
  dst[n] = 0;
  return 0;
}

struct pax {
  int has_path, has_link, has_size, has_uid, has_gid, has_mtime, has_uname, has_gname, sparse;
  uint64_t size, uid, gid;
  int64_t mtime;
};

static int dec(const char *p, size_t n, uint64_t *v) {
  uint64_t r = 0;
  if (n == 0) return -1;
  for (size_t i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9' || r > (~0ull - 9) / 10) return -1;
    r = r * 10 + (p[i] - '0');
  }
  *v = r;
  return 0;
}

static int parse_pax(arc_stream *s, char *b, size_t n, struct pax *x, tar_entry *e) {
  size_t off = 0;
  while (off < n) {
    size_t i = off, len = 0;
    while (i < n && b[i] >= '0' && b[i] <= '9') { len = len * 10 + (b[i] - '0'); if (len > n) break; i++; }
    if (i >= n || b[i] != ' ' || len < 5 || len > n - off || b[off + len - 1] != '\n')
      return arc_fail(s, "corrupt pax extended header");
    char *k = b + i + 1, *end = b + off + len - 1;
    char *eq = memchr(k, '=', end - k);
    if (!eq) return arc_fail(s, "corrupt pax extended header");
    size_t kl = eq - k, vl = end - eq - 1;
    const char *v = eq + 1;
#define KEY(name) (kl == sizeof name - 1 && !memcmp(k, name, kl))
    if (KEY("path") || KEY("linkpath")) {
      char *dst = KEY("path") ? e->path : e->link;
      if (vl >= sizeof e->path || memchr(v, 0, vl)) return arc_fail(s, "pax path is too long or invalid");
      memcpy(dst, v, vl);
      dst[vl] = 0;
      if (KEY("path")) x->has_path = 1; else x->has_link = 1;
    } else if (KEY("size")) {
      if (dec(v, vl, &x->size) < 0) return arc_fail(s, "corrupt pax size");
      x->has_size = 1;
    } else if (KEY("uid")) {
      if (dec(v, vl, &x->uid) < 0) return arc_fail(s, "corrupt pax uid");
      x->has_uid = 1;
    } else if (KEY("gid")) {
      if (dec(v, vl, &x->gid) < 0) return arc_fail(s, "corrupt pax gid");
      x->has_gid = 1;
    } else if (KEY("uname") || KEY("gname")) {
      char *dst = KEY("uname") ? e->uname : e->gname;
      if (vl >= sizeof e->uname) vl = sizeof e->uname - 1; /* user names only label files; shortening one is harmless */
      memcpy(dst, v, vl);
      dst[vl] = 0;
      if (KEY("uname")) x->has_uname = 1; else x->has_gname = 1;
    } else if (KEY("mtime")) {
      /* seconds, maybe negative, maybe with a fraction we drop */
      size_t j = 0;
      int neg = 0;
      if (vl && v[0] == '-') { neg = 1; j = 1; }
      size_t d = j;
      while (d < vl && v[d] != '.') d++;
      uint64_t t;
      if (dec(v + j, d - j, &t) < 0 || t > (1ull << 62)) return arc_fail(s, "corrupt pax mtime");
      x->mtime = neg ? -(int64_t)t : (int64_t)t;
      x->has_mtime = 1;
    } else if (kl > 11 && !memcmp(k, "GNU.sparse.", 11)) {
      x->sparse = 1;
    }
#undef KEY
    off += len;
  }
  return 0;
}

int tar_next(arc_stream *s, tar_entry *e) {
  if (s->tar_left + s->tar_pad && discard(s, s->tar_left + s->tar_pad) < 0) return -1;
  s->tar_left = s->tar_pad = 0;
  struct pax x;
  memset(&x, 0, sizeof x);
  memset(e, 0, sizeof *e);
  int long_name = 0, long_link = 0;
  for (;;) {
    uint8_t h[512];
    long r = read_full(s, h, 512);
    if (r < 0) return -1;
    if (r == 0) {
      if (long_name || long_link || x.has_path || x.has_size) return arc_fail(s, "unexpected end of tar archive");
      return 0;
    }
    if (r != 512) return arc_fail(s, "unexpected end of tar archive");
    int zero = 1;
    for (int i = 0; i < 512 && zero; i++) zero = !h[i];
    if (zero) return 0;
    if (!checksum_ok(h)) return arc_fail(s, "tar header checksum mismatch (not a tar archive, or corrupt)");
    uint64_t size;
    if (num(h + 124, 12, &size, 0) < 0) return arc_fail(s, "corrupt tar header (size)");
    char type = h[156];
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
      if (size > PAX_MAX) return arc_fail(s, "tar extended header is too large");
      uint64_t padded = (size + 511) & ~511ull;
      if (type == 'g') { if (discard(s, padded) < 0) return -1; continue; }
      char *b = malloc(padded + 1);
      if (!b) return arc_fail(s, "out of memory");
      long got = read_full(s, b, padded);
      if (got != (long)padded) { free(b); return got < 0 ? -1 : arc_fail(s, "unexpected end of tar archive"); }
      b[size] = 0;
      int bad = 0;
      if (type == 'x') bad = parse_pax(s, b, size, &x, e) < 0;
      else {
        size_t n = strnlen(b, size);
        char *dst = type == 'L' ? e->path : e->link;
        if (n >= sizeof e->path) bad = arc_fail(s, "tar long name is too long");
        else { memcpy(dst, b, n); dst[n] = 0; if (type == 'L') long_name = 1; else long_link = 1; }
      }
      free(b);
      if (bad) return -1;
      continue;
    }
    if (type == 'V') { if (discard(s, (size + 511) & ~511ull) < 0) return -1; continue; } /* volume label */
    if (type == 'S' || x.sparse) return arc_fail(s, "sparse files in tar archives are not supported");
    if (type == 'M') return arc_fail(s, "multi-volume tar archives are not supported");

    int ustar = !memcmp(h + 257, "ustar\0", 6), gnu = !memcmp(h + 257, "ustar  \0", 8);
    if (!x.has_path && !long_name) {
      char name[101];
      str_field(name, sizeof name, h, 100);
      if (ustar && h[345]) {
        char prefix[156];
        str_field(prefix, sizeof prefix, h + 345, 155);
        snprintf(e->path, sizeof e->path, "%s/%s", prefix, name);
      } else memcpy(e->path, name, strlen(name) + 1);
    }
    if (!x.has_link && !long_link) str_field(e->link, sizeof e->link, h + 157, 100);
    uint64_t mode, uid, gid, maj = 0, mnr = 0;
    int64_t mtime;
    if (num(h + 100, 8, &mode, 0) < 0 || num(h + 108, 8, &uid, 0) < 0 || num(h + 116, 8, &gid, 0) < 0 ||
        num(h + 136, 12, 0, &mtime) < 0)
      return arc_fail(s, "corrupt tar header");
    if ((ustar || gnu) && (num(h + 329, 8, &maj, 0) < 0 || num(h + 337, 8, &mnr, 0) < 0)) maj = mnr = 0;
    e->mode = mode & 07777;
    e->uid = x.has_uid ? (unsigned)x.uid : (unsigned)uid;
    e->gid = x.has_gid ? (unsigned)x.gid : (unsigned)gid;
    e->size = x.has_size ? x.size : size;
    e->mtime = x.has_mtime ? x.mtime : mtime;
    e->devmajor = (unsigned)maj;
    e->devminor = (unsigned)mnr;
    if ((ustar || gnu) && !x.has_uname) str_field(e->uname, sizeof e->uname, h + 265, 31);
    if ((ustar || gnu) && !x.has_gname) str_field(e->gname, sizeof e->gname, h + 297, 31);
    if (type == 0 || type == '7') type = '0';
    if (type == '0' && !ustar && !gnu) {
      size_t n = strlen(e->path);
      if (n && e->path[n - 1] == '/') type = '5'; /* old archives mark directories with a slash */
    }
    e->type = type;
    if (!e->path[0]) return arc_fail(s, "tar entry has an empty name");
    s->tar_left = e->size;
    s->tar_pad = (unsigned)((512 - (e->size & 511)) & 511);
    return 1;
  }
}

long tar_read(arc_stream *s, void *buf, unsigned long n) {
  if (s->tar_left == 0) return 0;
  if (n > s->tar_left) n = (unsigned long)s->tar_left;
  long r = arc_read(s, buf, n);
  if (r < 0) return -1;
  if (r == 0) return arc_fail(s, "unexpected end of tar archive");
  s->tar_left -= r;
  return r;
}

/* ---- writing ---- */

static int wzeros(arc_writer *w, size_t n) {
  static const uint8_t z[512];
  while (n) {
    size_t k = n < 512 ? n : 512;
    if (arc_write(w, z, k) != (long)k) return -1;
    n -= k;
  }
  return 0;
}

/* Writes value into an octal field of len bytes (len - 1 digits and a NUL),
 * or in base-256 if it does not fit. */
static void put_num(uint8_t *f, int len, int64_t v) {
  uint64_t lim = 1ull << (3 * (len - 1));
  if (v >= 0 && (uint64_t)v < lim) {
    for (int i = len - 2; i >= 0; i--) { f[i] = '0' + (v & 7); v >>= 3; }
    f[len - 1] = 0;
    return;
  }
  uint64_t u = (uint64_t)v;
  for (int i = len - 1; i >= 1; i--) { f[i] = u & 0xff; u >>= 8; }
  f[0] = v < 0 ? 0xff : 0x80;
}

static int header_block(arc_writer *w, const char *name, const char *link, const tar_entry *e, char type, uint64_t size) {
  uint8_t h[512];
  memset(h, 0, sizeof h);
  memcpy(h, name, strnlen(name, 100));
  put_num(h + 100, 8, e->mode & 07777);
  put_num(h + 108, 8, e->uid);
  put_num(h + 116, 8, e->gid);
  put_num(h + 124, 12, (int64_t)size);
  put_num(h + 136, 12, e->mtime);
  h[156] = type;
  memcpy(h + 157, link, strnlen(link, 100));
  memcpy(h + 257, "ustar  ", 8);  /* GNU magic and version, ending in a NUL */
  memcpy(h + 265, e->uname, strnlen(e->uname, 31));
  memcpy(h + 297, e->gname, strnlen(e->gname, 31));
  if (type == '3' || type == '4') { put_num(h + 329, 8, e->devmajor); put_num(h + 337, 8, e->devminor); }
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum & 0777777);
  h[155] = ' ';
  return arc_write(w, h, 512) == 512 ? 0 : -1;
}

static int long_record(arc_writer *w, const char *s, char type) {
  tar_entry z;
  memset(&z, 0, sizeof z);
  strcpy(z.uname, "root");
  strcpy(z.gname, "root");
  size_t n = strlen(s) + 1;
  if (header_block(w, "././@LongLink", "", &z, type, n) < 0) return -1;
  if (arc_write(w, s, n) != (long)n) return -1;
  return wzeros(w, (512 - (n & 511)) & 511);
}

uint64_t *arc__tar_left(arc_writer *w);
unsigned *arc__tar_pad(arc_writer *w);

int tar_write_header(arc_writer *w, const tar_entry *e) {
  uint64_t *left = arc__tar_left(w);
  unsigned *pad = arc__tar_pad(w);
  if (*left) return -1;  /* the previous entry got fewer bytes than its size */
  if (*pad && wzeros(w, *pad) < 0) return -1;
  *pad = 0;
  size_t pl = strnlen(e->path, sizeof e->path), ll = strnlen(e->link, sizeof e->link);
  if (pl == 0 || pl >= sizeof e->path || ll >= sizeof e->link) return -1;
  if (ll > 100 && long_record(w, e->link, 'K') < 0) return -1;
  if (pl > 100 && long_record(w, e->path, 'L') < 0) return -1;
  int data = e->type == '0' || e->type == 0 || e->type == '7';
  uint64_t size = data ? e->size : 0;
  if (header_block(w, e->path, e->link, e, e->type ? e->type : '0', size) < 0) return -1;
  *left = size;
  *pad = (unsigned)((512 - (size & 511)) & 511);
  return 0;
}

long tar_write(arc_writer *w, const void *buf, unsigned long n) {
  uint64_t *left = arc__tar_left(w);
  if (n > *left) return -1;
  long r = arc_write(w, buf, n);
  if (r > 0) *left -= r;
  return r;
}

int tar_write_end(arc_writer *w) {
  uint64_t *left = arc__tar_left(w);
  unsigned *pad = arc__tar_pad(w);
  if (*left) return -1;
  if (*pad && wzeros(w, *pad) < 0) return -1;
  *pad = 0;
  if (wzeros(w, 1024) < 0) return -1;
  /* GNU tar and bsdtar fill the last record: archives are a multiple of 10240 bytes */
  uint64_t t = arc_wtotal_in(w) % 10240;
  return t ? wzeros(w, 10240 - t) : 0;
}
