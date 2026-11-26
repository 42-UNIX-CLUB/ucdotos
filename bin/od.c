// SPDX-License-Identifier: GPL-2.0-or-later
/* od [-A d|o|x|n] [-t TYPE]... [-abcdilosvx] [-j SKIP] [-N COUNT] [-w[N]] [file...]
 * Dump bytes. TYPE: a (named characters), c (characters and escapes),
 * d, o, u, x followed by a size 1 2 4 8 or C S I L. Several types print one
 * line each, aligned as GNU od aligns them. A z after a type adds the
 * printable characters. f4 and f8 print floating point numbers with the
 * fewest digits that read back exactly. Repeated lines become "*" unless -v. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct spec { int kind, size, width, pad, text; }; /* text: the z suffix (a >printable< column) */
static struct spec specs[32];
static int nspecs, addr = 'o', verbose;
static long long skip, limit = -1;
static int line_bytes = 16;

static int width_of(int kind, int size) {
  static const int d[] = {0, 5, 7, 0, 12, 0, 0, 0, 21}, u[] = {0, 4, 6, 0, 11, 0, 0, 0, 21},
                   o[] = {0, 4, 7, 0, 12, 0, 0, 0, 23}, x[] = {0, 3, 5, 0, 9, 0, 0, 0, 17};
  switch (kind) {
  case 'a': case 'c': return 4;
  case 'f': return size == 4 ? 16 : 25;
  case 'd': return d[size];
  case 'u': return u[size];
  case 'o': return o[size];
  default: return x[size];
  }
}
static void add_spec(int kind, int size) {
  if (nspecs == 32) { fprintf(stderr, "od: too many types\n"); exit(1); }
  specs[nspecs++] = (struct spec){kind, size, width_of(kind, size), 0};
}
static void parse_type(const char *t) {
  while (*t) {
    int k = *t++;
    if (k == 'a' || k == 'c') { add_spec(k, 1); if (*t == 'z') specs[nspecs - 1].text = 1, t++; continue; }
    if (k == 'f') {
      int size = 8;
      if (*t == 'F') size = 4, t++;
      else if (*t == 'D') size = 8, t++;
      else if (*t == 'L') { fprintf(stderr, "od: long double is not supported\n"); exit(1); }
      else if (*t >= '0' && *t <= '9') {
        size = strtol(t, (char **)&t, 10);
        if (size != 4 && size != 8) { fprintf(stderr, "od: invalid type string: %d-byte floating point type not supported\n", size); exit(1); }
      }
      add_spec('f', size);
      if (*t == 'z') specs[nspecs - 1].text = 1, t++;
      continue;
    }
    if (!strchr("doux", k)) { fprintf(stderr, "od: invalid type string '%s'\n", t - 1); exit(1); }
    int size = 4;
    if (*t == 'C') size = 1, t++;
    else if (*t == 'S') size = 2, t++;
    else if (*t == 'I') size = 4, t++;
    else if (*t == 'L') size = sizeof(long), t++;
    else if (*t >= '0' && *t <= '9') {
      size = strtol(t, (char **)&t, 10);
      if (size != 1 && size != 2 && size != 4 && size != 8) { fprintf(stderr, "od: invalid type string: %d-byte type not supported\n", size); exit(1); }
    }
    add_spec(k, size);
    if (*t == 'z') specs[nspecs - 1].text = 1, t++;
  }
}
static long long parse_num(const char *s) {
  char *e;
  long long v = strtoll(s, &e, 0); /* 0x hex and leading-0 octal, as od allows */
  if (*e == 'b') v *= 512, e++;
  else if (*e == 'k' || *e == 'K') v *= 1024, e++;
  else if (*e == 'm' || *e == 'M') v *= 1024 * 1024, e++;
  if (*e || v < 0) { fprintf(stderr, "od: invalid number '%s'\n", s); exit(1); }
  return v;
}

static const char *names[] = {"nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "nl", "vt", "ff", "cr", "so", "si",
                              "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "esc", "fs", "gs", "rs", "us", "sp"};
static void field(const struct spec *sp, const unsigned char *p, int w) {
  unsigned long long v = 0;
  for (int k = sp->size - 1; k >= 0; k--) v = v << 8 | p[k]; /* little endian */
  char tmp[32];
  switch (sp->kind) {
  case 'a': {
    int c = p[0] & 0x7f;
    printf("%*s", w, c <= 32 ? names[c] : c == 127 ? "del" : (tmp[0] = c, tmp[1] = 0, tmp));
    return;
  }
  case 'c': {
    int c = p[0];
    const char *e = c == 0 ? "\\0" : c == 7 ? "\\a" : c == 8 ? "\\b" : c == 12 ? "\\f" : c == 10 ? "\\n" : c == 13 ? "\\r" : c == 9 ? "\\t" : c == 11 ? "\\v" : NULL;
    if (e) printf("%*s", w, e);
    else if (c >= 32 && c < 127) { tmp[0] = c; tmp[1] = 0; printf("%*s", w, tmp); }
    else printf("%*s", w, (snprintf(tmp, sizeof tmp, "%03o", c), tmp));
    return;
  }
  case 'd': {
    long long s = sp->size == 1 ? (signed char)v : sp->size == 2 ? (short)v : sp->size == 4 ? (int)v : (long long)v;
    printf("%*lld", w, s);
    return;
  }
  case 'u': printf("%*llu", w, v); return;
  case 'o': snprintf(tmp, sizeof tmp, "%0*llo", sp->width - 1, v); printf("%*s", w, tmp); return;
  case 'f': {
    /* the shortest %g that reads back to the same value */
    double d;
    if (sp->size == 4) { float fl; memcpy(&fl, p, 4); d = fl; } else memcpy(&d, p, 8);
    int maxp = sp->size == 4 ? 9 : 17;
    for (int prec = 1; prec <= maxp; prec++) {
      snprintf(tmp, sizeof tmp, "%.*g", prec, d);
      if (sp->size == 4 ? strtof(tmp, NULL) == (float)d : strtod(tmp, NULL) == d) break;
    }
    printf("%*s", w, tmp);
    return;
  }
  default: printf("%*.*llx", w, sp->size * 2, v); return;
  }
}
static void print_addr(long long off) {
  if (addr == 'o') printf("%07llo", off);
  else if (addr == 'd') printf("%07lld", off);
  else if (addr == 'x') printf("%06llx", off);
}
static void print_line(long long off, const unsigned char *buf, int n, int block) {
  for (int s = 0; s < nspecs; s++) {
    const struct spec *sp = &specs[s];
    if (s == 0) print_addr(off);
    else if (addr != 'n') printf("%*s", addr == 'x' ? 6 : 7, "");
    unsigned char tmp[8];
    int fields = block / sp->size;
    for (int b = 0; b < n; b += block) {
      /* GNU spreads the block's padding over its fields, more on the left */
      int pad_left = sp->pad;
      for (int i = fields; i-- > 0;) {
        int pos = b + (fields - 1 - i) * sp->size;
        if (pos >= n) break;
        int next = sp->pad * i / fields;
        const unsigned char *p = buf + pos;
        if (pos + sp->size > n) { memset(tmp, 0, sizeof tmp); memcpy(tmp, buf + pos, n - pos); p = tmp; }
        field(sp, p, sp->width + pad_left - next);
        pad_left = next;
      }
    }
    if (sp->text) {
      /* pad a short last line to the full width, then the characters */
      int missing = (line_bytes - n) / sp->size;
      int blocks_missing = (line_bytes - n) / block;
      printf("%*s  >", missing * sp->width + blocks_missing * sp->pad, "");
      for (int k = 0; k < n; k++) putchar(buf[k] >= 32 && buf[k] < 127 ? buf[k] : '.');
      putchar('<');
    }
    putchar('\n');
  }
}

int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (a[1] == '-') {
      if (!strncmp(a, "--address-radix=", 16)) a = a + 16, addr = *a;
      else if (!strncmp(a, "--format=", 9)) parse_type(a + 9);
      else if (!strncmp(a, "--skip-bytes=", 13)) skip = parse_num(a + 13);
      else if (!strncmp(a, "--read-bytes=", 13)) limit = parse_num(a + 13);
      else if (!strcmp(a, "--output-duplicates")) verbose = 1;
      else if (!strcmp(a, "--width")) line_bytes = 32;
      else if (!strncmp(a, "--width=", 8)) line_bytes = parse_num(a + 8);
      else { fprintf(stderr, "od: unrecognized option '%s'\n", a); return 1; }
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      switch (*f) {
      case 'b': add_spec('o', 1); continue;
      case 'c': add_spec('c', 1); continue;
      case 'd': add_spec('u', 2); continue;
      case 'o': add_spec('o', 2); continue;
      case 's': add_spec('d', 2); continue;
      case 'x': add_spec('x', 2); continue;
      case 'i': add_spec('d', 4); continue;
      case 'l': add_spec('d', sizeof(long)); continue;
      case 'a': add_spec('a', 1); continue;
      case 'v': verbose = 1; continue;
      case 'w': line_bytes = f[1] ? parse_num(f + 1) : 32; break;
      case 'A': case 't': case 'j': case 'N': {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "od: option requires an argument -- '%c'\n", *f); return 1; }
        if (*f == 'A') {
          if (strlen(v) != 1 || !strchr("doxn", v[0])) { fprintf(stderr, "od: invalid output address radix '%s'; it must be one character from [doxn]\n", v); return 1; }
          addr = v[0];
        } else if (*f == 't') parse_type(v);
        else if (*f == 'j') skip = parse_num(v);
        else limit = parse_num(v);
        break;
      }
      default: fprintf(stderr, "od: invalid option -- '%c'\nusage: od [-A radix] [-t type] [-j skip] [-N count] [-v] [-w width] [file...]\n", *f); return 1;
      }
      break;
    }
  }
  if (!strchr("doxn", addr)) { fprintf(stderr, "od: invalid output address radix\n"); return 1; }
  if (!nspecs) add_spec('o', 2);
  /* a block holds a whole number of fields of every type */
  int block = 1;
  for (int s = 0; s < nspecs; s++) if (specs[s].size > block) block = specs[s].size; /* sizes are powers of two */
  if (line_bytes <= 0) { fprintf(stderr, "od: invalid line width %d\n", line_bytes); return 1; }
  if (line_bytes % block) {
    int w = line_bytes / block * block;
    if (w == 0) w = block;
    fprintf(stderr, "od: warning: invalid width %d; using %d instead\n", line_bytes, w);
    line_bytes = w;
  }
  int wpb = 0;
  for (int s = 0; s < nspecs; s++) { int w = specs[s].width * (block / specs[s].size); if (w > wpb) wpb = w; }
  for (int s = 0; s < nspecs; s++) specs[s].pad = wpb - specs[s].width * (block / specs[s].size);

  setvbuf(stdout, NULL, _IOFBF, 65536);
  static char *dash[] = {"-"};
  char **files = i < argc ? argv + i : dash;
  int nf = i < argc ? argc - i : 1, fi = 0;
  FILE *f = NULL;
  unsigned char *buf = malloc(line_bytes), *prev = malloc(line_bytes);
  long long off = 0, left = limit;
  int have_prev = 0, starred = 0;
  /* the next byte of the concatenated input, or EOF */
#define NEXT_FILE() do { \
    if (f && f != stdin) fclose(f); f = NULL; \
    while (fi < nf && !f) { \
      const char *nm = files[fi++]; \
      f = strcmp(nm, "-") ? fopen(nm, "rb") : stdin; \
      if (!f) { fprintf(stderr, "od: %s: %s\n", nm, strerror(errno)); rc = 1; } } \
  } while (0)
  NEXT_FILE();
  while (skip > 0 && f) {
    int c = getc(f);
    if (c == EOF) { NEXT_FILE(); continue; }
    skip--;
    off++;
  }
  if (skip > 0) { fprintf(stderr, "od: cannot skip past end of combined input\n"); return 1; }
  for (;;) {
    int n = 0;
    while (n < line_bytes && f && left != 0) {
      int c = getc(f);
      if (c == EOF) { NEXT_FILE(); continue; }
      buf[n++] = c;
      if (left > 0) left--;
    }
    if (n == 0) break;
    if (!verbose && have_prev && n == line_bytes && !memcmp(buf, prev, n)) {
      if (!starred) { puts("*"); starred = 1; }
    } else {
      print_line(off, buf, n, block);
      starred = 0;
    }
    memcpy(prev, buf, n);
    have_prev = n == line_bytes;
    off += n;
    if (n < line_bytes) break;
  }
  if (addr != 'n') { print_addr(off); putchar('\n'); }
  if (fflush(stdout)) return 1;
  return rc;
}
