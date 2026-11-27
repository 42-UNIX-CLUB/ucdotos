// SPDX-License-Identifier: GPL-2.0-or-later
/* ar archives: the common format with GNU long names ("//" table and "/N")
 * and BSD long names ("#1/N"), as written by GNU binutils, BSD ar and
 * dpkg-deb. Symbol tables ("/" and "/SYM64/") are skipped.
 *
 * The functions take the caller's source directly and never read ahead, so
 * the caller reads member data from the same source. State that must live
 * between calls (the GNU name table, the pad of a BSD-named member) is kept
 * for one archive at a time, keyed by the ctx pointer; ar_begin resets it. */
#include "arc_int.h"

static struct {
  void *ctx;
  char *names;       /* GNU "//" table */
  size_t nnames;
  int pad;           /* pad byte owed after the current member's data */
} st;

static long read_full(arc_src src, void *ctx, void *buf, unsigned long n) {
  unsigned long got = 0;
  while (got < n) {
    long r = src(ctx, (uint8_t *)buf + got, n - got);
    if (r < 0) return -1;
    if (r == 0) break;
    got += r;
  }
  return (long)got;
}

static int skip(arc_src src, void *ctx, uint64_t n) {
  uint8_t tmp[4096];
  while (n) {
    unsigned long k = n < sizeof tmp ? (unsigned long)n : sizeof tmp;
    if (read_full(src, ctx, tmp, k) != (long)k) return -1;
    n -= k;
  }
  return 0;
}

int ar_begin(arc_src src, void *ctx) {
  free(st.names);
  st.names = 0;
  st.nnames = 0;
  st.ctx = ctx;
  st.pad = 0;
  char m[8];
  if (read_full(src, ctx, m, 8) != 8 || memcmp(m, "!<arch>\n", 8)) return -1;
  return 0;
}

/* Decimal or octal number in a space-padded field. -1 if malformed. */
static int field(const char *f, int len, int base, uint64_t *v) {
  int i = 0;
  uint64_t r = 0;
  while (i < len && f[i] == ' ') i++;
  if (i == len) { *v = 0; return 0; }
  for (; i < len && f[i] != ' '; i++) {
    int d = f[i] - '0';
    if (d < 0 || d >= base) return -1;
    if (r > (~0ull - d) / base) return -1;
    r = r * base + d;
  }
  for (; i < len; i++) if (f[i] != ' ') return -1;
  *v = r;
  return 0;
}

int ar_next(arc_src src, void *ctx, ar_member *m) {
  for (;;) {
    char h[60];
    long r = read_full(src, ctx, h, 60);
    if (r == 0) return 0;
    if (r != 60) {
      /* a stray newline after the last member is tolerated */
      if (r == 1 && h[0] == '\n') return 0;
      return -1;
    }
    if (h[58] != '`' || h[59] != '\n') return -1;
    uint64_t size, mtime, uid, gid, mode;
    if (field(h + 48, 10, 10, &size) || field(h + 16, 12, 10, &mtime) || field(h + 28, 6, 10, &uid) ||
        field(h + 34, 6, 10, &gid) || field(h + 40, 8, 8, &mode))
      return -1;
    int nl = 16;
    while (nl > 0 && h[nl - 1] == ' ') nl--;
    int pad = size & 1;
    memset(m, 0, sizeof *m);
    if ((nl == 1 && h[0] == '/') || (nl == 7 && !memcmp(h, "/SYM64/", 7))) { /* symbol table */
      if (skip(src, ctx, size + pad) < 0) return -1;
      continue;
    }
    if (nl == 2 && h[0] == '/' && h[1] == '/') { /* GNU long name table */
      if (size > (1u << 20)) return -1;
      free(st.names);
      st.names = malloc(size + 1);
      if (!st.names) return -1;
      if (read_full(src, ctx, st.names, size) != (long)size) return -1;
      st.names[size] = 0;
      st.nnames = size;
      st.ctx = ctx;
      if (pad && skip(src, ctx, 1) < 0) return -1;
      continue;
    }
    size_t n = 0;
    if (h[0] == '/' && nl > 1 && h[1] >= '0' && h[1] <= '9') { /* GNU long name: offset into the table */
      uint64_t off;
      if (field(h + 1, nl - 1, 10, &off) || st.ctx != ctx || !st.names || off >= st.nnames) return -1;
      const char *p = st.names + off;
      while (off + n < st.nnames && p[n] != '\n' && p[n] != 0) n++;
      if (n && p[n - 1] == '/') n--;
      if (n == 0 || n >= sizeof m->name) return -1;
      memcpy(m->name, p, n);
    } else if (nl > 3 && !memcmp(h, "#1/", 3)) { /* BSD: the name follows the header */
      uint64_t len;
      if (field(h + 3, nl - 3, 10, &len) || len == 0 || len > size || len >= sizeof m->name) return -1;
      if (read_full(src, ctx, m->name, len) != (long)len) return -1;
      m->name[len] = 0;
      size -= len;
      n = strlen(m->name);
      if (n == 0) return -1;
    } else {
      n = nl;
      if (n && h[n - 1] == '/') n--;  /* GNU terminator */
      if (n == 0) return -1;
      memcpy(m->name, h, n);
    }
    m->name[n] = 0;
    if (memchr(m->name, '/', n)) return -1; /* a member name never holds a path */
    m->size64 = size;
    if (size > (unsigned long)-1) return -1;  /* only on 32-bit hosts */
    m->size = (unsigned long)size;
    m->mtime = (long long)mtime;
    m->uid = (unsigned)uid;
    m->gid = (unsigned)gid;
    m->mode = (unsigned)mode;
    st.ctx = ctx;
    st.pad = pad;
    return 1;
  }
}

int ar_skip_pad(arc_src src, void *ctx, const ar_member *m) {
  int pad = st.ctx == ctx ? st.pad : (int)(m->size64 & 1);
  st.pad = 0;
  if (!pad) return 0;
  char c;
  long r = read_full(src, ctx, &c, 1);
  if (r == 0) return 0;  /* some writers leave out the last pad byte */
  return r == 1 ? 0 : -1;
}

/* ---- writer ---- */

static int put(arc_sink sink, void *ctx, const void *p, unsigned long n) {
  unsigned long off = 0;
  while (off < n) {
    long r = sink(ctx, (const uint8_t *)p + off, n - off);
    if (r <= 0) return -1;
    off += r;
  }
  return 0;
}

int ar_write_begin(arc_sink sink, void *ctx) { return put(sink, ctx, "!<arch>\n", 8); }

int ar_write_header(arc_sink sink, void *ctx, const ar_member *m) {
  char h[61];
  uint64_t size = m->size64 ? m->size64 : m->size;
  size_t n = strnlen(m->name, sizeof m->name);
  if (n == 0 || n >= sizeof m->name || memchr(m->name, '/', n)) return -1;
  int bsd = n > 15 || memchr(m->name, ' ', n);
  if (bsd) size += n;
  char name[17];
  if (bsd) snprintf(name, sizeof name, "#1/%u", (unsigned)n);
  else memcpy(name, m->name, n), name[n] = 0;
  long long mt = m->mtime < 0 ? 0 : m->mtime;
  if (size > 9999999999ull || mt > 999999999999ll) return -1;
  snprintf(h, sizeof h, "%-16s%-12lld%-6u%-6u%-8o%-10llu`\n", name, mt, m->uid % 1000000, m->gid % 1000000,
           m->mode & 07777777, (unsigned long long)size);
  if (put(sink, ctx, h, 60) < 0) return -1;
  if (bsd && put(sink, ctx, m->name, n) < 0) return -1;
  return 0;
}

int ar_write_pad(arc_sink sink, void *ctx, const ar_member *m) {
  uint64_t size = m->size64 ? m->size64 : m->size;
  size_t n = strnlen(m->name, sizeof m->name);
  if (n > 15 || memchr(m->name, ' ', n)) size += n;
  return size & 1 ? put(sink, ctx, "\n", 1) : 0;
}
