// SPDX-License-Identifier: GPL-2.0-or-later
/* vsnprintf shared by the kernel and the user libc.
 * Supports: %d %i %u %x %X %o %c %s %p %f %F %e %E %g %G %%, flags - 0 + space #,
 * width, .precision (both may be *), length hh h l ll z j t L(ignored, double). */
typedef unsigned long usize;
typedef unsigned long long u64;
typedef __builtin_va_list va_list;
#define va_arg __builtin_va_arg
usize strlen(const char *);

struct out { char *buf; usize cap, len; };
static void put(struct out *o, char c) { if (o->len + 1 < o->cap) o->buf[o->len] = c; o->len++; }
static void putn(struct out *o, char c, int n) { while (n-- > 0) put(o, c); }

/* Emit prefix (sign, 0x) and body s[0..n) padded to width. Zero padding goes
 * between the prefix and the body. */
static void emit(struct out *o, const char *pre, const char *s, int n, int width, int left, int zero) {
  int np = (int)strlen(pre), pad = width > n + np ? width - n - np : 0;
  if (!left && !zero) putn(o, ' ', pad);
  for (int i = 0; i < np; i++) put(o, pre[i]);
  if (!left && zero) putn(o, '0', pad);
  for (int i = 0; i < n; i++) put(o, s[i]);
  if (left) putn(o, ' ', pad);
}

static int utoa(u64 v, unsigned base, int upper, char *end) {
  const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char *p = end;
  do { *--p = digs[v % base]; v /= base; } while (v);
  return end - p;
}

/* ---- floating point: exact conversion ----
 * A finite double is M * 2^E exactly. For E < 0 that equals M * 5^-E / 10^-E,
 * so the big integer M * 5^-E (or M << E) holds every decimal digit of v.
 * Rounding is then done on the decimal string, ties to even, like glibc. */
typedef unsigned int u32;
/* All significant digits of v > 0 into d (no leading/trailing zeros).
 * v = 0.d[0]d[1]... * 10^(*dexp). Returns the digit count (at most ~770). */
static int exact_digits(double v, char *d, int *dexp) {
  union { double d; u64 u; } b = {v};
  int be = (int)((b.u >> 52) & 0x7ff), pt = 0, nw = 2, nd = 0;
  u64 M = b.u & 0xfffffffffffffull;
  if (be) M |= 1ull << 52; else be = 1;
  int E = be - 1075;
  u32 w[84] = {(u32)M, (u32)(M >> 32)};
  if (E >= 0) { /* shift left by E */
    int ws = E >> 5, bs = E & 31;
    for (int i = nw - 1; i >= 0; i--) { w[i + ws] = w[i]; if (ws) w[i] = 0; }
    nw += ws;
    if (bs) {
      u32 carry = 0;
      for (int i = 0; i < nw; i++) { u32 x = w[i]; w[i] = (x << bs) | carry; carry = x >> (32 - bs); }
      if (carry) w[nw++] = carry;
    }
  } else { /* multiply by 5^-E, 13 factors of 5 at a time */
    pt = -E;
    for (int k = -E; k > 0; k -= 13) {
      u32 f = 1;
      for (int i = 0; i < (k < 13 ? k : 13); i++) f *= 5;
      u64 carry = 0;
      for (int i = 0; i < nw; i++) { u64 t = (u64)w[i] * f + carry; w[i] = (u32)t; carry = t >> 32; }
      if (carry) w[nw++] = (u32)carry;
    }
  }
  while (nw && !w[nw - 1]) nw--;
  char tmp[800]; /* digits, least significant first */
  while (nw) {
    u64 rem = 0;
    for (int i = nw - 1; i >= 0; i--) { u64 cur = (rem << 32) | w[i]; w[i] = (u32)(cur / 1000000000u); rem = cur % 1000000000u; }
    while (nw && !w[nw - 1]) nw--;
    for (int i = 0; i < 9; i++) { tmp[nd++] = '0' + rem % 10; rem /= 10; }
  }
  while (nd && tmp[nd - 1] == '0') nd--;
  int lo = 0;
  while (tmp[lo] == '0') lo++;
  *dexp = nd - pt;
  for (int i = nd - 1; i >= lo; i--) *d++ = tmp[i];
  return nd - lo;
}
/* Round the digit string to keep digits (ties to even). Updates n and dexp. */
static void round_digits(char *d, int *n, int *dexp, int keep) {
  if (keep >= *n) return;
  if (keep < 0) { *n = 0; return; }
  int up = d[keep] > '5';
  if (d[keep] == '5') {
    up = keep + 1 < *n || (keep > 0 && (d[keep - 1] - '0') & 1); /* digits are trimmed, so more digits mean > half */
  }
  *n = keep;
  if (!up) { while (*n && d[*n - 1] == '0') (*n)--; return; }
  int i = keep - 1;
  while (i >= 0 && d[i] == '9') i--;
  if (i < 0) { d[0] = '1'; *n = 1; (*dexp)++; return; }
  d[i]++;
  *n = i + 1;
}
/* Formats |v| into buf (no sign). Returns the length. conv is one of f e g (lowercase). */
static int fmt_float(char *buf, double v, int prec, char conv, int alt, int upper) {
  char d[800], *p = buf;
  int n = 0, dexp = 1;
  if (v != 0) n = exact_digits(v, d, &dexp);
  if (conv == 'g') {
    int P = prec == 0 ? 1 : prec;
    round_digits(d, &n, &dexp, P);
    int X = n ? dexp - 1 : 0;
    if (X >= -4 && X < P) { conv = 'f'; prec = P - 1 - X; } else { conv = 'e'; prec = P - 1; }
    if (!alt) { /* drop trailing zeros: no more precision than digits */
      int sig = n ? n : 1, need = conv == 'f' ? sig - 1 - X : sig - 1;
      if (need < prec) prec = need < 0 ? 0 : need;
    }
  }
  if (conv == 'e') {
    round_digits(d, &n, &dexp, prec + 1);
    int ex = n ? dexp - 1 : 0;
    *p++ = n ? d[0] : '0';
    if (prec || alt) *p++ = '.';
    for (int i = 1; i <= prec; i++) *p++ = i < n ? d[i] : '0';
    *p++ = upper ? 'E' : 'e';
    *p++ = ex < 0 ? '-' : '+';
    if (ex < 0) ex = -ex;
    char eb[8];
    int en = utoa(ex, 10, 0, eb + 8);
    if (en < 2) *p++ = '0';
    for (int i = 0; i < en; i++) *p++ = eb[8 - en + i];
    return p - buf;
  }
  /* 'f': digit i of the string has weight 10^(dexp-1-i) */
  if (dexp + prec < 0) n = 0;
  else round_digits(d, &n, &dexp, dexp + prec);
  if (!n) dexp = 1;
  if (dexp <= 0) *p++ = '0';
  for (int i = 0; i < dexp; i++) *p++ = i < n ? d[i] : '0';
  if (prec || alt) *p++ = '.';
  for (int i = 0; i < prec; i++) { int j = dexp + i; *p++ = j >= 0 && j < n ? d[j] : '0'; }
  return p - buf;
}

int vsnprintf(char *buf, usize cap, const char *fmt, va_list ap) {
  struct out o = {buf, cap, 0};
  for (; *fmt; fmt++) {
    if (*fmt != '%') { put(&o, *fmt); continue; }
    fmt++;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0, sht = 0;
    for (;; fmt++) {
      if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+') plus = 1;
      else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else break;
    }
    if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
    else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      fmt++; prec = 0;
      if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
      else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    for (;; fmt++) {
      if (*fmt == 'l') lng++; else if (*fmt == 'h') sht++; else if (*fmt == 'j' || *fmt == 'q') lng = 2;
      else if (*fmt == 'z' || *fmt == 't' || *fmt == 'L') {} else break;
    }
    if (left) zero = 0;
    char tmp[640], *end = tmp + sizeof tmp;
    switch (*fmt) {
    case 'd': case 'i': {
      long long v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
      if (sht == 1) v = (short)v; else if (sht >= 2) v = (signed char)v;
      u64 u = v < 0 ? -(u64)v : (u64)v;
      int n = (prec == 0 && u == 0) ? 0 : utoa(u, 10, 0, end);
      while (n < prec) *(end - ++n) = '0';
      emit(&o, v < 0 ? "-" : plus ? "+" : space ? " " : "", end - n, n, width, left, zero && prec < 0);
      break;
    }
    case 'u': case 'x': case 'X': case 'o': {
      u64 v = lng >= 2 ? va_arg(ap, u64) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
      if (sht == 1) v = (unsigned short)v; else if (sht >= 2) v = (unsigned char)v;
      int n = (prec == 0 && v == 0) ? 0 : utoa(v, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16, *fmt == 'X', end);
      while (n < prec) *(end - ++n) = '0';
      const char *pre = "";
      if (alt && v && *fmt == 'x') pre = "0x"; else if (alt && v && *fmt == 'X') pre = "0X";
      else if (alt && *fmt == 'o' && (n == 0 || end[-n] != '0')) pre = "0";
      emit(&o, pre, end - n, n, width, left, zero && prec < 0);
      break;
    }
    case 'p': {
      int n = utoa((unsigned long)va_arg(ap, void *), 16, 0, end);
      emit(&o, "0x", end - n, n, width, left, 0);
      break;
    }
    case 'c': { char c = (char)va_arg(ap, int); emit(&o, "", &c, 1, width, left, 0); break; }
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      int n = 0;
      while (s[n] && (prec < 0 || n < prec)) n++;
      emit(&o, "", s, n, width, left, 0);
      break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
      double v = va_arg(ap, double);
      int upper = *fmt == 'F' || *fmt == 'E' || *fmt == 'G';
      union { double d; u64 u; } b = {v};
      const char *sign = (b.u >> 63) ? "-" : plus ? "+" : space ? " " : "";
      if (v < 0) v = -v;
      if (v != v || v - v != 0) { /* nan or inf */
        const char *s = v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit(&o, sign, s, 3, width, left, 0);
        break;
      }
      if (prec < 0) prec = 6;
      if (prec > 300) prec = 300;
      char c = *fmt | 32;
      int n = fmt_float(tmp, v, prec, c, alt, upper);
      emit(&o, sign, tmp, n, width, left, zero);
      break;
    }
    case '%': put(&o, '%'); break;
    case 0: fmt--; break;
    default: put(&o, '%'); put(&o, *fmt);
    }
  }
  if (cap) o.buf[o.len < cap ? o.len : cap - 1] = 0;
  return (int)o.len;
}
int snprintf(char *buf, usize cap, const char *fmt, ...) {
  va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(buf, cap, fmt, ap);
  __builtin_va_end(ap);
  return n;
}
