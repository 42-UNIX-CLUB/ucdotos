// SPDX-License-Identifier: GPL-2.0-or-later
/* scanf family: one core (vscan) reading from a string or a FILE.
 * Supports %d %i %u %o %x %X %p %c %s %[...] %f %e %g %a (any case) %n %%,
 * assignment suppression (*), field widths and the length modifiers hh h l ll j z t L. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

struct in { const char *s; FILE *f; int n; };
static int get(struct in *in) {
  int c = in->f ? fgetc(in->f) : (*in->s ? (unsigned char)*in->s++ : EOF);
  if (c != EOF) in->n++;
  return c;
}
static void unget(struct in *in, int c) {
  if (c == EOF) return;
  in->n--;
  if (in->f) ungetc(c, in->f); else in->s--;
}
static int digit_ok(int c, int base) {
  if (base == 8) return c >= '0' && c <= '7';
  if (base == 16) return isxdigit(c);
  return isdigit(c);
}
long double __double_to_ld(double);

static int vscan(struct in *in, const char *fmt, va_list ap) {
  int count = 0, c;
  for (; *fmt; fmt++) {
    if (isspace((unsigned char)*fmt)) {
      while (isspace(c = get(in))) {}
      unget(in, c);
      continue;
    }
    if (*fmt != '%' || fmt[1] == '%') {
      if (*fmt == '%') {
        fmt++;
        while (isspace(c = get(in))) {}
        unget(in, c);
      }
      c = get(in);
      if (c != (unsigned char)*fmt) { unget(in, c); return c == EOF && !count ? EOF : count; }
      continue;
    }
    fmt++;
    int suppress = 0, width = 0, size = 0; /* size: -2 hh, -1 h, 1 l, 2 ll, 3 L */
    if (*fmt == '*') suppress = 1, fmt++;
    while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
    for (;; fmt++) {
      if (*fmt == 'h') size = size < 0 ? -2 : -1;
      else if (*fmt == 'l') size = size == 1 ? 2 : 1;
      else if (*fmt == 'j' || *fmt == 'q') size = 2;
      else if (*fmt == 'L') size = 3;
      else if (*fmt == 'z' || *fmt == 't') size = 1;
      else break;
    }
    int conv = *fmt;
    if (!conv) break;
    if (conv == 'n') {
      if (!suppress) *va_arg(ap, int *) = in->n;
      continue;
    }
    if (conv != 'c' && conv != '[') {
      while (isspace(c = get(in))) {}
      unget(in, c);
    }
    if (conv == 'c' || conv == 's' || conv == '[') {
      unsigned char set[256] = {0};
      if (conv == '[') {
        int neg = 0;
        fmt++;
        if (*fmt == '^') neg = 1, fmt++;
        if (*fmt == ']') set[']'] = 1, fmt++;
        for (; *fmt && *fmt != ']'; fmt++) {
          if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
            for (int ch = (unsigned char)fmt[0]; ch <= (unsigned char)fmt[2]; ch++) set[ch] = 1;
            fmt += 2;
          } else set[(unsigned char)*fmt] = 1;
        }
        if (!*fmt) return count;
        if (neg) for (int i = 0; i < 256; i++) set[i] = !set[i];
      }
      if (!width) width = conv == 'c' ? 1 : -1;
      char *out = suppress ? 0 : va_arg(ap, char *);
      int got = 0;
      while (width < 0 || got < width) {
        c = get(in);
        if (c == EOF) break;
        if ((conv == 's' && isspace(c)) || (conv == '[' && !set[c])) { unget(in, c); break; }
        if (out) out[got] = c;
        got++;
      }
      if (!got || (conv == 'c' && got < width)) return c == EOF && !count ? EOF : count;
      if (out && conv != 'c') out[got] = 0;
      if (out) count++;
      continue;
    }
    char buf[128];
    int len = 0, max = width && width < (int)sizeof buf - 1 ? width : (int)sizeof buf - 1;
#define TAKE() (buf[len++] = c, c = len < max ? get(in) : EOF)
    c = get(in);
    if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X' || conv == 'p') {
      int base = conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'i' ? 0 : 10, nd = 0;
      if (c == '+' || c == '-') TAKE();
      if ((base == 0 || base == 16) && c == '0') {
        TAKE(); nd++;
        if (c == 'x' || c == 'X') { TAKE(); base = 16; nd = 0; }
        else if (base == 0) base = 8;
      }
      if (!base) base = 10;
      while (len < max && digit_ok(c, base)) TAKE(), nd++;
      unget(in, c);
      if (!nd) return c == EOF && !count && !len ? EOF : count;
      buf[len] = 0;
      if (suppress) continue;
      int sgn = conv == 'd' || conv == 'i';
      unsigned long long v = sgn ? (unsigned long long)strtoll(buf, 0, base) : strtoull(buf, 0, base);
      if (conv == 'p') *va_arg(ap, void **) = (void *)(unsigned long)v;
      else if (size == -2) *va_arg(ap, char *) = (char)v;
      else if (size == -1) *va_arg(ap, short *) = (short)v;
      else if (size == 1) *va_arg(ap, long *) = (long)v;
      else if (size >= 2) *va_arg(ap, long long *) = (long long)v;
      else *va_arg(ap, int *) = (int)v;
      count++;
      continue;
    }
    if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'a' || conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A') {
      int nd = 0, hex = 0;
      if (c == '+' || c == '-') TAKE();
      if (c == 'i' || c == 'I' || c == 'n' || c == 'N') { /* inf, infinity, nan */
        const char *w = (c == 'i' || c == 'I') ? "infinity" : "nan";
        int k = 0;
        while (w[k] && len < max && tolower(c) == w[k]) TAKE(), k++;
        if (k != 3 && k != 8) { unget(in, c); return count; }
        nd = 1;
      } else {
        if (c == '0') {
          TAKE(); nd++;
          if (c == 'x' || c == 'X') TAKE(), hex = 1, nd = 0;
        }
        while (len < max && (hex ? isxdigit(c) : isdigit(c))) TAKE(), nd++;
        if (len < max && c == '.') {
          TAKE();
          while (len < max && (hex ? isxdigit(c) : isdigit(c))) TAKE(), nd++;
        }
        if (nd && len < max && (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) {
          TAKE();
          if (len < max && (c == '+' || c == '-')) TAKE();
          while (len < max && isdigit(c)) TAKE();
        }
      }
      unget(in, c);
      if (!nd) return c == EOF && !count && !len ? EOF : count;
      buf[len] = 0;
      if (suppress) continue;
      double v = strtod(buf, 0);
      if (size == 3) *va_arg(ap, long double *) = __double_to_ld(v);
      else if (size == 1) *va_arg(ap, double *) = v;
      else *va_arg(ap, float *) = (float)v;
      count++;
      continue;
    }
#undef TAKE
    unget(in, c);
    return count; /* unknown conversion */
  }
  return count;
}

int vsscanf(const char *s, const char *fmt, va_list ap) { struct in in = {s, 0, 0}; return vscan(&in, fmt, ap); }
int vfscanf(FILE *f, const char *fmt, va_list ap) { struct in in = {0, f, 0}; return vscan(&in, fmt, ap); }
int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int sscanf(const char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }
int fscanf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r; }
int scanf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(stdin, fmt, ap); va_end(ap); return r; }
