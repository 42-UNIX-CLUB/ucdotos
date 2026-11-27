// SPDX-License-Identifier: GPL-2.0-or-later
/* Small helpers: memory, strings, lists, a hash map, an arena, messages, files. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

const char *pkg_progname = "dpkg";
int pkg_verbose, pkg_quiet;

static void oom(void) { fprintf(stderr, "%s: out of memory\n", pkg_progname); exit(2); }
void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xcalloc(size_t a, size_t b) { void *p = calloc(a ? a : 1, b ? b : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) oom(); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
char *xasprintf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt); va_copy(ap2, ap);
  int n = vsnprintf(0, 0, fmt, ap);
  va_end(ap);
  char *p = xmalloc(n + 1);
  vsnprintf(p, n + 1, fmt, ap2);
  va_end(ap2);
  return p;
}

/* ---- sbuf ---- */
static void sb_grow(sbuf *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  size_t c = b->cap ? b->cap : 64;
  while (c < b->n + extra + 1) c *= 2;
  b->s = xrealloc(b->s, c);
  b->cap = c;
}
void sb_add(sbuf *b, const void *p, size_t n) { sb_grow(b, n); memcpy(b->s + b->n, p, n); b->n += n; b->s[b->n] = 0; }
void sb_puts(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }
void sb_putc(sbuf *b, int c) { char ch = c; sb_add(b, &ch, 1); }
void sb_printf(sbuf *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt); va_copy(ap2, ap);
  int n = vsnprintf(0, 0, fmt, ap);
  va_end(ap);
  sb_grow(b, n);
  vsnprintf(b->s + b->n, n + 1, fmt, ap2);
  va_end(ap2);
  b->n += n;
}
void sb_free(sbuf *b) { free(b->s); b->s = 0; b->n = b->cap = 0; }

/* ---- strlist ---- */
void sl_add(strlist *l, const char *s) {
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = xrealloc(l->v, l->cap * sizeof *l->v); }
  l->v[l->n++] = xstrdup(s);
}
void sl_free(strlist *l) { for (int i = 0; i < l->n; i++) free(l->v[i]); free(l->v); l->v = 0; l->n = l->cap = 0; }
int sl_find(const strlist *l, const char *s) { for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i], s)) return i; return -1; }
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
void sl_sort(strlist *l) { if (l->n > 1) qsort(l->v, l->n, sizeof *l->v, cmp_str); }

/* ---- strmap: open addressing ---- */
uint32_t hash_str(const char *s, size_t n) {
  uint32_t h = 2166136261u;  /* FNV-1a */
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
  return h;
}
void *sm_get(const strmap *m, const char *key) {
  if (!m->cap) return 0;
  uint32_t i = hash_str(key, strlen(key)) & (m->cap - 1);
  while (m->k[i]) { if (!strcmp(m->k[i], key)) return m->v[i]; i = (i + 1) & (m->cap - 1); }
  return 0;
}
void **sm_slot(strmap *m, const char *key) {
  if ((m->n + 1) * 4 >= m->cap * 3) {
    uint32_t nc = m->cap ? m->cap * 2 : 64;
    char **nk = xcalloc(nc, sizeof *nk);
    void **nv = xcalloc(nc, sizeof *nv);
    for (uint32_t i = 0; i < m->cap; i++) if (m->k[i]) {
      uint32_t j = hash_str(m->k[i], strlen(m->k[i])) & (nc - 1);
      while (nk[j]) j = (j + 1) & (nc - 1);
      nk[j] = m->k[i]; nv[j] = m->v[i];
    }
    free(m->k); free(m->v);
    m->k = nk; m->v = nv; m->cap = nc;
  }
  uint32_t i = hash_str(key, strlen(key)) & (m->cap - 1);
  while (m->k[i]) { if (!strcmp(m->k[i], key)) return &m->v[i]; i = (i + 1) & (m->cap - 1); }
  m->k[i] = xstrdup(key);
  m->v[i] = 0;
  m->n++;
  return &m->v[i];
}
void sm_free(strmap *m) {
  for (uint32_t i = 0; i < m->cap; i++) free(m->k[i]);
  free(m->k); free(m->v);
  memset(m, 0, sizeof *m);
}

/* ---- arena ---- */
struct arena_blk { arena_blk *next; size_t used, cap; char data[]; };
void *ar_alloc(arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (!a->head || a->head->used + n > a->head->cap) {
    size_t cap = n > (1u << 20) ? n : (1u << 20);
    arena_blk *b = xmalloc(sizeof *b + cap);
    b->next = a->head; b->used = 0; b->cap = cap;
    a->head = b;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += n;
  a->used_total += n;
  return p;
}
char *ar_strndup(arena *a, const char *s, size_t n) { char *p = ar_alloc(a, n + 1); memcpy(p, s, n); p[n] = 0; return p; }
void ar_free(arena *a) { while (a->head) { arena_blk *n = a->head->next; free(a->head); a->head = n; } a->used_total = 0; }

/* ---- messages ---- */
void pkg_msg(const char *fmt, ...) {
  if (pkg_quiet > 1) return;
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void pkg_warn(const char *fmt, ...) {
  fflush(stdout);
  fprintf(stderr, "%s: warning: ", pkg_progname);
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}
void pkg_error(const char *fmt, ...) {
  fflush(stdout);
  fprintf(stderr, "%s: error: ", pkg_progname);
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}
static strlist unsup;
static int unsup_count;
void pkg_unsupported(const char *what, const char *path) {
  unsup_count++;
  if (sl_find(&unsup, what) >= 0) return;
  sl_add(&unsup, what);
  pkg_warn("this system cannot %s yet (first seen on %s); carrying on without it", what, path ? path : "?");
}
int pkg_unsupported_count(void) { return unsup_count; }
void pkg_unsupported_report(void) {
  if (!unsup.n) return;
  sbuf b = {0};
  for (int i = 0; i < unsup.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", unsup.v[i]);
  pkg_warn("%d file operations were skipped because the system does not support them: %s", unsup_count, b.s);
  sb_free(&b);
}

/* ---- files ---- */
int file_read(const char *path, char **data, size_t *n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  sbuf b = {0};
  char buf[65536];
  long r;
  while ((r = read(fd, buf, sizeof buf)) > 0) sb_add(&b, buf, r);
  int e = errno;
  close(fd);
  if (r < 0) { sb_free(&b); errno = e; return -1; }
  if (!b.s) sb_add(&b, "", 0);
  *data = b.s;
  if (n) *n = b.n;
  return 0;
}
void fsync_fd(int fd, const char *what) {
  if (pkg_cfg.force & F_UNSAFE_IO) return;
  if (fsync(fd) < 0 && (errno == ENOSYS || errno == EINVAL)) pkg_unsupported("fsync files", what);
}
void fsync_path(const char *path) {
  if (pkg_cfg.force & F_UNSAFE_IO) return;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  fsync_fd(fd, path);
  close(fd);
}
/* Write to PATH.new, fsync, rename over PATH, fsync the directory. A crash leaves
 * either the old or the new file, never a mix. */
int file_write_atomic(const char *path, const void *data, size_t n, unsigned mode) {
  char *tmp = xasprintf("%s.new", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) { free(tmp); return -1; }
  if (write(fd, data, n) != (ssize_t)n) { int e = errno; close(fd); unlink(tmp); free(tmp); errno = e; return -1; }
  fsync_fd(fd, path);
  if (close(fd) < 0 || rename(tmp, path) < 0) { int e = errno; unlink(tmp); free(tmp); errno = e; return -1; }
  free(tmp);
  char *dir = path_dirname(path);
  fsync_path(dir);
  free(dir);
  return 0;
}
/* The UC.OS kernel refuses file names longer than 63 bytes (kernel/kernel.h,
 * struct inode name[64]); apt's index file names are often longer. Say so
 * plainly instead of a bare "File name too long". Update when the kernel's
 * limit changes. */
#ifdef __wasm__
#define PKG_NAME_LIMIT 63
#else
#define PKG_NAME_LIMIT 255
#endif
const char *pkg_file_error(int err, const char *path) {
  static char msg[256];
  if (err != ENAMETOOLONG || !path) return strerror(err);
  size_t longest = 0;
  for (const char *p = path; *p;) {
    size_t n = strcspn(p, "/");
    if (n > longest) longest = n;
    p += n;
    while (*p == '/') p++;
  }
  snprintf(msg, sizeof msg, "file name too long: a name in this path is %zu bytes, and this system accepts at most %d bytes per name", longest, PKG_NAME_LIMIT);
  return msg;
}
int file_copy(const char *from, const char *to, unsigned mode) {
  int in = open(from, O_RDONLY);
  if (in < 0) return -1;
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (out < 0) { close(in); return -1; }
  char buf[65536];
  long r;
  int rc = 0;
  while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, r) != r) { rc = -1; break; }
  if (r < 0) rc = -1;
  close(in);
  if (close(out) < 0) rc = -1;
  return rc;
}
int mkdir_p(const char *path, unsigned mode) {
  char *p = xstrdup(path);
  for (char *s = p + 1; ; s++) {
    if (*s == '/' || !*s) {
      char c = *s;
      *s = 0;
      if (mkdir(p, mode) < 0 && errno != EEXIST) {
        if (!path_is_dir(p)) { free(p); return -1; }
      }
      *s = c;
      if (!c) break;
    }
  }
  free(p);
  return path_is_dir(path) ? 0 : -1;
}
int path_exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
int path_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
char *path_join(const char *a, const char *b) {
  size_t la = strlen(a);
  while (la > 1 && a[la - 1] == '/') la--;
  while (*b == '/') b++;
  if (la == 1 && a[0] == '/') return xasprintf("/%s", b);
  if (!*b) return xstrndup(a, la);
  return xasprintf("%.*s/%s", (int)la, a, b);
}
char *path_dirname(const char *p) {
  const char *s = strrchr(p, '/');
  if (!s) return xstrdup(".");
  if (s == p) return xstrdup("/");
  return xstrndup(p, s - p);
}
void hex_encode(const unsigned char *in, int n, char *out) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) { out[2 * i] = hx[in[i] >> 4]; out[2 * i + 1] = hx[in[i] & 15]; }
  out[2 * n] = 0;
}
int hex_digest_file(const char *path, int md5, char *out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  arc_md5 m; arc_sha256 s;
  if (md5) arc_md5_init(&m); else arc_sha256_init(&s);
  char buf[65536];
  long r;
  while ((r = read(fd, buf, sizeof buf)) > 0) { if (md5) arc_md5_update(&m, buf, r); else arc_sha256_update(&s, buf, r); }
  close(fd);
  if (r < 0) return -1;
  unsigned char d[32];
  if (md5) { arc_md5_final(&m, d); hex_encode(d, 16, out); }
  else { arc_sha256_final(&s, d); hex_encode(d, 32, out); }
  return 0;
}
long fd_reader_src(void *ctx, void *buf, unsigned long n) {
  fd_reader *r = ctx;
  if (r->limited) { if (!r->left) return 0; if (n > r->left) n = r->left; }
  long k = read(r->fd, buf, n);
  if (k > 0 && r->limited) r->left -= k;
  return k;
}
long arc_stream_src(void *ctx, void *buf, unsigned long n) { return arc_read(ctx, buf, n); }
void xqsort(void *b, size_t n, size_t sz, int (*cmp)(const void *, const void *)) { if (n > 1) qsort(b, n, sz, cmp); }
