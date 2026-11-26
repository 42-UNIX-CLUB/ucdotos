// SPDX-License-Identifier: GPL-2.0-or-later
// A small printf for the translator's builds (the standalone wasm build has no libc).
// Supports %d %i %u %x %c %s %f %%, the l and ll sizes, a width, '-' and '0', a
// precision for %f and %s, and %.*s.
#include "port.h"
#include <stdarg.h>

typedef struct { char *buf; size_t cap, n; } sink;
static void put(sink *s, char c) { if (s->n + 1 < s->cap) s->buf[s->n] = c; s->n++; }
static void puts_(sink *s, const char *p, size_t n, int width, int left, char pad) {
  int fill = width > (int)n ? width - (int)n : 0;
  if (!left) while (fill-- > 0) put(s, pad);
  for (size_t i = 0; i < n; i++) put(s, p[i]);
  if (left) while (fill-- > 0) put(s, ' ');
}

static void vfmt(sink *s, const char *f, va_list ap) {
  for (; *f; f++) {
    if (*f != '%') { put(s, *f); continue; }
    f++;
    int left = 0, width = 0, prec = -1, lng = 0;
    char pad = ' ';
    for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') pad = '0'; else break; }
    if (*f == '*') { width = va_arg(ap, int); f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      f++; prec = 0;
      if (*f == '*') { prec = va_arg(ap, int); f++; }
      while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
    }
    while (*f == 'l') { lng++; f++; }
    if (*f == 'z') { lng = sizeof(size_t) == 8 ? 2 : 0; f++; }
    char tmp[64];
    int n = 0;
    switch (*f) {
    case 'd': case 'i': {
      long long v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
      unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
      do { tmp[63 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
      if (v < 0) tmp[63 - n++] = '-';
      puts_(s, tmp + 64 - n, (size_t)n, width, left, pad);
      break;
    }
    case 'u': case 'x': {
      unsigned long long u = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
      unsigned base = *f == 'x' ? 16 : 10;
      do { tmp[63 - n++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
      puts_(s, tmp + 64 - n, (size_t)n, width, left, pad);
      break;
    }
    case 'c': tmp[0] = (char)va_arg(ap, int); puts_(s, tmp, 1, width, left, ' '); break;
    case 's': {
      const char *p = va_arg(ap, const char *);
      if (!p) p = "(null)";
      size_t len = 0;
      while (p[len] && (prec < 0 || (int)len < prec)) len++;
      puts_(s, p, len, width, left, ' ');
      break;
    }
    case 'f': {
      double v = va_arg(ap, double);
      if (prec < 0) prec = 6;
      if (v != v) { puts_(s, "nan", 3, width, left, ' '); break; }
      int neg = v < 0;
      if (neg) v = -v;
      double scale = 1;
      for (int i = 0; i < prec; i++) scale *= 10;
      if (v > 1e18) { puts_(s, "big", 3, width, left, ' '); break; }
      unsigned long long whole = (unsigned long long)(v * scale + 0.5);
      unsigned long long ip = whole / (unsigned long long)scale, fp = whole % (unsigned long long)scale;
      for (int i = 0; i < prec; i++) { tmp[63 - n++] = (char)('0' + fp % 10); fp /= 10; }
      if (prec) tmp[63 - n++] = '.';
      do { tmp[63 - n++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
      if (neg) tmp[63 - n++] = '-';
      puts_(s, tmp + 64 - n, (size_t)n, width, left, pad);
      break;
    }
    case '%': put(s, '%'); break;
    default: put(s, '%'); put(s, *f); break;
    }
  }
}

int jit_snprintf(char *buf, size_t cap, const char *fmt, ...) {
  sink s = { buf, cap, 0 };
  va_list ap;
  va_start(ap, fmt);
  vfmt(&s, fmt, ap);
  va_end(ap);
  if (cap) buf[s.n < cap ? s.n : cap - 1] = 0;
  return (int)s.n;
}

int jit_printf(const char *fmt, ...) {
  char buf[1024];
  sink s = { buf, sizeof buf, 0 };
  va_list ap;
  va_start(ap, fmt);
  vfmt(&s, fmt, ap);
  va_end(ap);
  size_t n = s.n < sizeof buf ? s.n : sizeof buf - 1;
  jit_print(buf, n);
  return (int)s.n;
}
