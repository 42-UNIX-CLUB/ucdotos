// SPDX-License-Identifier: GPL-2.0-or-later
/* Binary packages, deb(5): an ar archive whose members are, in this order,
 * "debian-binary" (the text "2.0\n"), "control.tar[.gz|.xz|.zst]" and
 * "data.tar[.gz|.xz|.zst|.bz2|.lzma]". Members whose names start with "_" may
 * sit between them and are skipped. Anything after data.tar is ignored.
 * The control archive is small: it is read into memory. The data archive is
 * streamed. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONTROL_FILE_MAX (64u << 20)

static int fmt_for(const char *member, const char *base, char **err) {
  size_t bl = strlen(base);
  const char *suf = member + bl;
  if (!*suf) return ARC_RAW;
  if (!strcmp(suf, ".gz")) return ARC_GZIP;
  if (!strcmp(suf, ".xz")) return ARC_XZ;
  if (!strcmp(suf, ".zst")) return ARC_ZSTD;
  if (err) *err = xasprintf("member %s uses a compression this system does not read", member);
  return -1;
}
static int drain(deb *d) {
  char buf[4096];
  while (d->rd.left) {
    long r = fd_reader_src(&d->rd, buf, sizeof buf);
    if (r <= 0) return -1;
  }
  return 0;
}
static int skip_member(deb *d, const ar_member *m) {
  d->rd.limited = 1; d->rd.left = m->size;
  int r = drain(d);
  d->rd.limited = 0;
  return r < 0 ? -1 : ar_skip_pad(fd_reader_src, &d->rd, m);
}

int deb_norm_path(const char *in, char *out, size_t cap) {
  size_t n = 0;
  out[n++] = '/';
  const char *p = in;
  while (*p) {
    while (*p == '/') p++;
    const char *e = p;
    while (*e && *e != '/') e++;
    size_t len = e - p;
    if (len == 0) break;
    if (len == 1 && p[0] == '.') { p = e; continue; }
    if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
    if (n > 1) { if (n + 1 >= cap) return 0; out[n++] = '/'; }
    if (n + len >= cap) return 0;
    memcpy(out + n, p, len);
    n += len;
    p = e;
  }
  out[n] = 0;
  return 1;
}

static void add_cfile(deb *d, const char *name, unsigned char *data, size_t size, unsigned mode) {
  d->cf = xrealloc(d->cf, (d->ncf + 1) * sizeof *d->cf);
  deb_cfile *c = &d->cf[d->ncf++];
  memset(c, 0, sizeof *c);
  snprintf(c->name, sizeof c->name, "%s", name);
  c->data = data; c->size = size; c->mode = mode;
}

deb *deb_open(const char *path, char **err) {
  *err = 0;
  deb *d = xcalloc(1, sizeof *d);
  d->fd = open(path, O_RDONLY);
  if (d->fd < 0) { *err = xasprintf("cannot open %s", path); free(d); return 0; }
  struct stat st;
  if (fstat(d->fd, &st) == 0) d->file_size = st.st_size;
  d->rd.fd = d->fd;
  ar_member m;
  if (ar_begin(fd_reader_src, &d->rd) < 0) { *err = xasprintf("%s is not a Debian binary package (no ar header)", path); goto fail; }
  if (ar_next(fd_reader_src, &d->rd, &m) != 1 || strcmp(m.name, "debian-binary")) {
    *err = xasprintf("%s: first member is not debian-binary", path); goto fail;
  }
  if (m.size > 64 || m.size < 3) { *err = xasprintf("%s: bad debian-binary member", path); goto fail; }
  {
    char v[65] = {0};
    d->rd.limited = 1; d->rd.left = m.size;
    unsigned long got = 0;
    while (got < m.size) { long r = fd_reader_src(&d->rd, v + got, m.size - got); if (r <= 0) break; got += r; }
    d->rd.limited = 0;
    if (got != m.size) { *err = xasprintf("%s: truncated", path); goto fail; }
    ar_skip_pad(fd_reader_src, &d->rd, &m);
    char *nl = strchr(v, '\n');
    if (nl) *nl = 0;
    snprintf(d->fmtver, sizeof d->fmtver, "%s", v);
    if (strncmp(v, "2.", 2)) { *err = xasprintf("%s: archive format version %s is not supported (want 2.x)", path, v); goto fail; }
    if (strcmp(v, "2.0")) pkg_warn("%s: format version %s has a newer minor version than 2.0", path, v);
  }
  for (;;) {
    int r = ar_next(fd_reader_src, &d->rd, &m);
    if (r != 1) { *err = xasprintf("%s: no control member", path); goto fail; }
    if (m.name[0] == '_') { if (skip_member(d, &m) < 0) goto trunc; continue; }
    if (strncmp(m.name, "control.tar", 11)) { *err = xasprintf("%s: unexpected member '%s' where control.tar was expected", path, m.name); goto fail; }
    break;
  }
  {
    int fmt = fmt_for(m.name, "control.tar", err);
    if (fmt < 0) goto fail;
    snprintf(d->control_member, sizeof d->control_member, "%s", m.name);
    d->control_size = m.size;
    d->rd.limited = 1; d->rd.left = m.size;
    arc_stream *s = arc_open(fmt, fd_reader_src, &d->rd);
    tar_entry *e = xmalloc(sizeof *e);
    int k;
    while ((k = tar_next(s, e)) == 1) {
      char np[4096];
      if (!deb_norm_path(e->path, np, sizeof np)) { *err = xasprintf("%s: unsafe path in control archive", path); k = -2; break; }
      if (!strcmp(np, "/")) continue;
      if (e->type != '0' && e->type != '7') continue;
      if (strchr(np + 1, '/')) continue;  /* control files are flat */
      if (e->size > CONTROL_FILE_MAX) { *err = xasprintf("%s: control file %s too big", path, np); k = -2; break; }
      unsigned char *buf = xmalloc(e->size + 1);
      uint64_t got = 0;
      while (got < e->size) { long r = tar_read(s, buf + got, e->size - got); if (r <= 0) break; got += r; }
      if (got != e->size) { free(buf); k = -1; break; }
      buf[e->size] = 0;
      add_cfile(d, np + 1, buf, e->size, e->mode);
    }
    if (k < 0 && !*err) *err = xasprintf("%s: corrupt control archive (%s)", path, arc_error(s));
    arc_close(s);
    free(e);
    if (k < 0) goto fail;
    if (drain(d) < 0) goto trunc;
    d->rd.limited = 0;
    ar_skip_pad(fd_reader_src, &d->rd, &m);
  }
  {
    const deb_cfile *c = deb_cfile_get(d, "control");
    if (!c) { *err = xasprintf("%s: the control archive has no control file", path); goto fail; }
    ctl_reader *r = ctl_open_mem((const char *)c->data, c->size);
    int k = ctl_next(r, &d->control);
    if (k != 1) { *err = xasprintf("%s: control file: %s", path, k < 0 ? ctl_errmsg(r) : "empty"); ctl_close(r); goto fail; }
    ctl_close(r);
    if (!ctl_get(&d->control, "Package")) { *err = xasprintf("%s: control file has no Package field", path); goto fail; }
  }
  return d;
trunc:
  *err = xasprintf("%s: truncated archive", path);
fail:
  deb_close(d);
  return 0;
}

const deb_cfile *deb_cfile_get(const deb *d, const char *name) {
  for (int i = 0; i < d->ncf; i++) if (!strcmp(d->cf[i].name, name)) return &d->cf[i];
  return 0;
}

int deb_data_begin(deb *d, int raw, char **err) {
  (void)raw;
  ar_member m;
  for (;;) {
    int r = ar_next(fd_reader_src, &d->rd, &m);
    if (r != 1) { *err = xstrdup("no data member"); return -1; }
    if (m.name[0] == '_') { if (skip_member(d, &m) < 0) { *err = xstrdup("truncated"); return -1; } continue; }
    if (strncmp(m.name, "data.tar", 8)) { *err = xasprintf("unexpected member '%s' where data.tar was expected", m.name); return -1; }
    break;
  }
  int fmt = fmt_for(m.name, "data.tar", err);
  if (fmt < 0) return -1;
  snprintf(d->data_member, sizeof d->data_member, "%s", m.name);
  d->data_size = m.size;
  d->rd.limited = 1; d->rd.left = m.size;
  d->data = arc_open(fmt, fd_reader_src, &d->rd);
  if (!d->data) { *err = xstrdup("cannot open the data archive"); return -1; }
  return 0;
}

void deb_close(deb *d) {
  if (!d) return;
  if (d->data) arc_close(d->data);
  for (int i = 0; i < d->ncf; i++) free(d->cf[i].data);
  free(d->cf);
  ctl_free(&d->control);
  if (d->fd >= 0) close(d->fd);
  free(d);
}
