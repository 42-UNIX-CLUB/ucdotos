// SPDX-License-Identifier: GPL-2.0-or-later
/* Memory, buffers, hash maps, interned identifiers and diagnostics. */
#include "cc.h"

/* A bump allocator. The compiler exits after one translation unit, so nothing is
 * freed; this is faster than malloc and needs no bookkeeping. */
static char *arena, *arena_end;
void *xalloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (n > (size_t)(arena_end - arena)) {
    size_t sz = n > (1 << 20) ? n : (1 << 20);
    arena = calloc(1, sz);
    if (!arena) fatal("out of memory");
    arena_end = arena + sz;
  }
  void *p = arena;
  arena += n;
  return p;
}
void *xrealloc(void *p, size_t n) {
  p = realloc(p, n);
  if (!p) fatal("out of memory");
  return p;
}
char *xstrndup(const char *s, size_t n) {
  char *p = xalloc(n + 1);
  memcpy(p, s, n);
  return p;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xfmt(const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) return xstrdup(tmp);
  char *p = xalloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(p, n + 1, fmt, ap);
  va_end(ap);
  return p;
}

char *read_file(const char *path, size_t *len) {
  FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
  if (!f) return NULL;
  size_t cap = 4096, n = 0;
  char *p = malloc(cap);
  for (;;) {
    if (cap - n < 4096) p = xrealloc(p, cap *= 2);
    size_t r = fread(p + n, 1, cap - n - 1, f);
    if (r == 0) break;
    n += r;
  }
  if (f != stdin) fclose(f);
  p[n] = 0;
  *len = n;
  return p;
}
int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

/* ---- byte buffers and the LEB128 encodings WebAssembly uses ---- */
static void buf_grow(Buf *b, int n) {
  if (b->len + n <= b->cap) return;
  int cap = b->cap ? b->cap * 2 : 64;
  while (cap < b->len + n) cap *= 2;
  b->p = xrealloc(b->p, cap);
  b->cap = cap;
}
void buf_byte(Buf *b, int c) {
  buf_grow(b, 1);
  b->p[b->len++] = (u8)c;
}
void buf_mem(Buf *b, const void *p, int n) {
  if (n <= 0) return;
  buf_grow(b, n);
  memcpy(b->p + b->len, p, n);
  b->len += n;
}
void buf_str(Buf *b, const char *s) { buf_mem(b, s, (int)strlen(s)); }
void buf_uleb(Buf *b, u64 v) {
  do {
    u8 c = v & 0x7f;
    v >>= 7;
    buf_byte(b, v ? c | 0x80 : c);
  } while (v);
}
void buf_sleb(Buf *b, i64 v) {
  for (;;) {
    u8 c = v & 0x7f;
    v >>= 7; /* arithmetic shift on every compiler we use */
    if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { buf_byte(b, c); return; }
    buf_byte(b, c | 0x80);
  }
}
void put_uleb5(u8 *p, u32 v) {
  for (int i = 0; i < 4; i++) { p[i] = (v & 0x7f) | 0x80; v >>= 7; }
  p[4] = v & 0x7f;
}
void put_sleb5(u8 *p, int32_t v) {
  for (int i = 0; i < 4; i++) { p[i] = (v & 0x7f) | 0x80; v >>= 7; }
  p[4] = v & 0x7f;
}
void put_u32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void buf_uleb5(Buf *b, u32 v) { buf_grow(b, 5); put_uleb5(b->p + b->len, v); b->len += 5; }
void buf_sleb5(Buf *b, int32_t v) { buf_grow(b, 5); put_sleb5(b->p + b->len, v); b->len += 5; }
void buf_u32(Buf *b, u32 v) { buf_grow(b, 4); put_u32(b->p + b->len, v); b->len += 4; }
void buf_name(Buf *b, const char *s) {
  int n = (int)strlen(s);
  buf_uleb(b, n);
  buf_mem(b, s, n);
}

void vec_push(Vec *v, void *x) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->p = xrealloc(v->p, v->cap * sizeof *v->p);
  }
  v->p[v->len++] = x;
}

/* ---- string-keyed hash map with open addressing ---- */
static unsigned hash_str(const char *s, int n) {
  unsigned h = 2166136261u;
  for (int i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 16777619u;
  return h;
}
void *map_get(Map *m, const char *k) {
  if (!m->cap) return NULL;
  unsigned i = hash_str(k, (int)strlen(k)) & (m->cap - 1);
  for (; m->keys[i]; i = (i + 1) & (m->cap - 1))
    if (!strcmp(m->keys[i], k)) return m->vals[i];
  return NULL;
}
void map_put(Map *m, const char *k, void *v) {
  if ((m->used + 1) * 2 > m->cap) {
    Map n = {0};
    n.cap = m->cap ? m->cap * 2 : 64;
    n.keys = calloc(n.cap, sizeof *n.keys);
    n.vals = calloc(n.cap, sizeof *n.vals);
    for (int i = 0; i < m->cap; i++)
      if (m->keys[i]) map_put(&n, m->keys[i], m->vals[i]);
    free(m->keys);
    free(m->vals);
    *m = n;
  }
  unsigned i = hash_str(k, (int)strlen(k)) & (m->cap - 1);
  for (; m->keys[i]; i = (i + 1) & (m->cap - 1))
    if (!strcmp(m->keys[i], k)) { m->vals[i] = v; return; }
  m->keys[i] = k;
  m->vals[i] = v;
  m->used++;
}

/* ---- interned identifiers ---- */
#define NBUCKET 8192
static Ident *idtab[NBUCKET];
Ident *intern(const char *s, int len) {
  unsigned h = hash_str(s, len);
  Ident **pp = &idtab[h & (NBUCKET - 1)];
  for (Ident *id = *pp; id; id = id->next)
    if (id->hash == h && id->len == len && !memcmp(id->name, s, len)) return id;
  Ident *id = xalloc(sizeof *id);
  id->name = xstrndup(s, len);
  id->len = len;
  id->hash = h;
  id->next = *pp;
  *pp = id;
  return id;
}

/* UC.OS runs programs on one thread and takes the CPU back only at system calls. A
 * long compile with no I/O would freeze every other program, so the compiler makes a
 * cheap system call every few microseconds of work (per statement, per 64 tokens); the
 * kernel checks its time slice on those. */
#ifdef __wasm__
#include <unistd.h>
#endif
void tick(void) {
#ifdef __wasm__
  getppid(); /* about 15 ns; the kernel reads its clock on every 256th call */
#endif
}

/* ---- diagnostics: file:line:col, the source line, a caret ---- */
int nerrors, nwarnings, opt_werror, opt_wall, opt_w;

char *tok_str(Tok *t) {
  if (t->kind == T_EOF) return "end of file";
  if (t->kind == T_IDENT) return (char *)t->id->name;
  return xstrndup(t->s, t->len);
}

static void show(Tok *t, const char *kind, const char *fmt, va_list ap) {
  while (t && t->origin) t = t->origin;
  if (t && t->file) {
    fprintf(stderr, "%s:%d:%d: %s: ", t->file->name, t->line, t->col, kind);
  } else {
    fprintf(stderr, "cc: %s: ", kind);
  }
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  if (t && t->file && t->s >= t->file->data && t->s <= t->file->data + t->file->len) {
    const char *b = t->s, *e = t->s;
    while (b > t->file->data && b[-1] != '\n') b--;
    while (*e && *e != '\n') e++;
    fprintf(stderr, "  %.*s\n  ", (int)(e - b), b);
    for (const char *p = b; p < t->s; p++) fputc(*p == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
  }
}
void error_at(Tok *t, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  show(t, "error", fmt, ap);
  va_end(ap);
  exit(1);
}
void errorc_at(Tok *t, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  show(t, "error", fmt, ap);
  va_end(ap);
  if (++nerrors >= 20) { fputs("cc: too many errors\n", stderr); exit(1); }
}
void warn_at(Tok *t, const char *fmt, ...) {
  if (opt_w) return;
  va_list ap;
  va_start(ap, fmt);
  show(t, opt_werror ? "error" : "warning", fmt, ap);
  va_end(ap);
  if (opt_werror) nerrors++;
  nwarnings++;
}
void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("cc: error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}
