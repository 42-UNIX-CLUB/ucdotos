// SPDX-License-Identifier: GPL-2.0-or-later
/* Control paragraphs (deb822(5), Debian Policy 5.1).
 *  - A paragraph is a run of fields; paragraphs are separated by lines that
 *    are empty or hold only spaces and tabs.
 *  - "Name: value". A name is printable ASCII without space or colon and does
 *    not start with '#' or '-'. Names compare without regard to case.
 *  - A line that starts with a space or tab continues the previous field.
 *  - A line that starts with '#' is a comment, also between continuation lines.
 *  - Leading and trailing spaces of the first line of a value are dropped,
 *    trailing spaces of continuation lines too; continuation lines otherwise
 *    stay as they are, so writing a paragraph back gives the same text.
 *  - A field may appear only once in a paragraph. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ---- paragraph storage ---- */
static uint32_t put(ctl_para *p, const char *s, size_t n) {
  if (p->len + n + 1 > p->cap) {
    size_t c = p->cap ? p->cap : 512;
    while (c < p->len + n + 1) c *= 2;
    p->buf = xrealloc(p->buf, c);
    p->cap = c;
  }
  uint32_t at = p->len;
  memcpy(p->buf + at, s, n);
  p->buf[at + n] = 0;
  p->len += n + 1;
  return at;
}
static int find(const ctl_para *p, const char *name) {
  for (int i = 0; i < p->n; i++) if (!strcasecmp(p->buf + p->f[i].name, name)) return i;
  return -1;
}
const char *ctl_get(const ctl_para *p, const char *name) { int i = find(p, name); return i < 0 ? 0 : p->buf + p->f[i].value; }
const char *ctl_name(const ctl_para *p, int i) { return p->buf + p->f[i].name; }
const char *ctl_value(const ctl_para *p, int i) { return p->buf + p->f[i].value; }
static void add_field(ctl_para *p, uint32_t name, uint32_t value) {
  if (p->n == p->fcap) { p->fcap = p->fcap ? p->fcap * 2 : 16; p->f = xrealloc(p->f, p->fcap * sizeof *p->f); }
  p->f[p->n].name = name;
  p->f[p->n].value = value;
  p->n++;
}
void ctl_set(ctl_para *p, const char *name, const char *value) {
  int i = find(p, name);
  /* compute both before storing offsets: put() may move the buffer */
  uint32_t v = put(p, value, strlen(value));
  if (i >= 0) { p->f[i].value = v; return; }
  uint32_t nm = put(p, name, strlen(name));
  add_field(p, nm, v);
}
void ctl_del(ctl_para *p, const char *name) {
  int i = find(p, name);
  if (i < 0) return;
  memmove(p->f + i, p->f + i + 1, (p->n - i - 1) * sizeof *p->f);
  p->n--;
}
void ctl_reset(ctl_para *p) { p->len = 0; p->n = 0; p->line = 0; }
void ctl_free(ctl_para *p) { free(p->buf); free(p->f); memset(p, 0, sizeof *p); }
void ctl_copy(ctl_para *dst, const ctl_para *src) {
  ctl_reset(dst);
  for (int i = 0; i < src->n; i++) ctl_set(dst, ctl_name(src, i), ctl_value(src, i));
  dst->line = src->line;
}
void ctl_write(sbuf *out, const ctl_para *p) {
  for (int i = 0; i < p->n; i++) {
    const char *v = ctl_value(p, i);
    sb_puts(out, ctl_name(p, i));
    sb_putc(out, ':');
    if (*v && *v != '\n') sb_putc(out, ' ');
    sb_puts(out, v);
    sb_putc(out, '\n');
  }
}
int ctl_field_name_ok(const char *s, size_t n) {
  if (!n || s[0] == '#' || s[0] == '-') return 0;
  for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] <= 32 || (unsigned char)s[i] >= 127 || s[i] == ':') return 0;
  return 1;
}

/* ---- reader ---- */
struct ctl_reader {
  arc_src src; void *ctx;
  const char *mem; size_t memlen;   /* in-memory input (no copy) */
  char *buf; size_t pos, end, cap;
  uint64_t base;                    /* input offset of buf[0] */
  uint64_t para_off;
  int line, eof, err;
  char msg[160];
};
ctl_reader *ctl_open(arc_src src, void *ctx) {
  ctl_reader *r = xcalloc(1, sizeof *r);
  r->src = src; r->ctx = ctx;
  return r;
}
ctl_reader *ctl_open_mem(const char *data, size_t n) {
  ctl_reader *r = xcalloc(1, sizeof *r);
  r->mem = data; r->memlen = n; r->eof = 1;
  r->buf = (char *)data; r->end = n;
  return r;
}
void ctl_close(ctl_reader *r) { if (r && !r->mem) free(r->buf); free(r); }
const char *ctl_errmsg(ctl_reader *r) { return r->msg; }
uint64_t ctl_offset(ctl_reader *r) { return r->para_off; }

/* Next line without its newline; NULL at end. *len excludes the newline. */
static const char *next_line(ctl_reader *r, size_t *len, uint64_t *off) {
  for (;;) {
    char *nl = r->end > r->pos ? memchr(r->buf + r->pos, '\n', r->end - r->pos) : 0;
    if (nl || (r->eof && r->pos < r->end)) {
      const char *s = r->buf + r->pos;
      size_t n = nl ? (size_t)(nl - s) : r->end - r->pos;
      *off = r->base + r->pos;
      r->pos += n + (nl ? 1 : 0);
      r->line++;
      *len = n;
      return s;
    }
    if (r->eof) return 0;
    /* shift and refill */
    if (r->pos) {
      memmove(r->buf, r->buf + r->pos, r->end - r->pos);
      r->base += r->pos; r->end -= r->pos; r->pos = 0;
    }
    if (r->end + 65536 > r->cap) { r->cap = r->cap ? r->cap * 2 : 131072; r->buf = xrealloc(r->buf, r->cap); }
    long k = r->src(r->ctx, r->buf + r->end, r->cap - r->end);
    if (k < 0) { r->err = 1; snprintf(r->msg, sizeof r->msg, "read error"); return 0; }
    if (k == 0) r->eof = 1;
    r->end += k;
  }
}
static int blank(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return 0;
  return 1;
}
static size_t rtrim(const char *s, size_t n) {
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
  return n;
}
int ctl_next(ctl_reader *r, ctl_para *p) {
  ctl_reset(p);
  if (r->err) return -1;
  size_t n;
  uint64_t off;
  const char *s;
  int have_field = 0;
  for (;;) {
    s = next_line(r, &n, &off);
    if (!s) {
      if (r->err) return -1;
      return p->n ? 1 : 0;
    }
    if (blank(s, n)) { if (p->n) return 1; continue; }
    if (s[0] == '#') continue;
    if (s[0] == ' ' || s[0] == '\t') {
      if (!have_field) { snprintf(r->msg, sizeof r->msg, "line %d: continuation line without a field", r->line); r->err = 1; return -1; }
      /* append "\n" + line to the last value, which is the last string in buf */
      size_t k = rtrim(s, n);
      p->len--;                      /* drop the NUL, then add "\n" and the line */
      put(p, "\n", 1);
      p->len--;
      put(p, s, k);
      continue;
    }
    const char *colon = memchr(s, ':', n);
    if (!colon || !ctl_field_name_ok(s, colon - s)) {
      snprintf(r->msg, sizeof r->msg, "line %d: %s", r->line, colon ? "bad field name" : "missing ':' after the field name");
      r->err = 1;
      return -1;
    }
    char name[128];
    size_t nl = colon - s;
    if (nl >= sizeof name) { snprintf(r->msg, sizeof r->msg, "line %d: field name too long", r->line); r->err = 1; return -1; }
    memcpy(name, s, nl); name[nl] = 0;
    if (find(p, name) >= 0) {
      snprintf(r->msg, sizeof r->msg, "line %d: duplicate field '%.60s'", r->line, name);
      r->err = 1;
      return -1;
    }
    if (!p->n) { r->para_off = off; p->line = r->line; }
    const char *v = colon + 1, *e = s + n;
    while (v < e && (*v == ' ' || *v == '\t')) v++;
    size_t vl = rtrim(v, e - v);
    uint32_t nm = put(p, name, nl);
    uint32_t vv = put(p, v, vl);
    add_field(p, nm, vv);
    have_field = 1;
  }
}

int ctl_parse_file(const char *path, int (*fn)(ctl_para *p, void *arg), void *arg) {
  fd_reader fr = {0};
  fr.fd = open(path, O_RDONLY);
  if (fr.fd < 0) return -2;
  arc_stream *s = arc_open(ARC_AUTO, fd_reader_src, &fr);
  ctl_reader *r = ctl_open(arc_stream_src, s);
  ctl_para p = {0};
  int rc = 0, k;
  while ((k = ctl_next(r, &p)) == 1) if ((rc = fn(&p, arg)) != 0) break;
  if (k < 0) { pkg_error("parsing %s: %s", path, ctl_errmsg(r)); rc = -1; }
  ctl_free(&p);
  ctl_close(r);
  arc_close(s);
  close(fr.fd);
  return rc;
}
