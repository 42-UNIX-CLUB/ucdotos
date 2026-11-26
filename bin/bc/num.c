// SPDX-License-Identifier: GPL-2.0-or-later
/* Decimal arbitrary precision arithmetic for bc. Digits are stored one per
 * byte, least significant first; schoolbook algorithms throughout. */
#include "bc.h"

static num *alloc(int n, int scale) {
  num *a = malloc(sizeof *a);
  if (n < scale) n = scale;
  a->sign = 1;
  a->n = n;
  a->scale = scale;
  a->d = calloc(n + 1, 1);
  return a;
}
void num_free(num *a) { if (a) { free(a->d); free(a); } }
/* drop leading zero digits of the integer part */
static num *norm(num *a) {
  while (a->n > a->scale && a->d[a->n - 1] == 0) a->n--;
  if (num_is_zero(a)) a->sign = 1;
  return a;
}
int num_is_zero(const num *a) {
  for (int i = 0; i < a->n; i++) if (a->d[i]) return 0;
  return 1;
}
num *num_zero(void) { return alloc(0, 0); }
num *num_int(long v) {
  num *a = alloc(20, 0);
  unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
  for (int i = 0; i < 20; i++) { a->d[i] = u % 10; u /= 10; }
  a->sign = v < 0 ? -1 : 1;
  return norm(a);
}
num *num_dup(const num *a) {
  num *r = alloc(a->n, a->scale);
  memcpy(r->d, a->d, a->n);
  r->sign = a->sign;
  return r;
}
/* digit for the power 10^k */
static int dig(const num *a, int k) {
  int i = k + a->scale;
  return i >= 0 && i < a->n ? a->d[i] : 0;
}
static int ipart(const num *a) { return a->n - a->scale; }

static int cmp_abs(const num *a, const num *b) {
  int hi = ipart(a) > ipart(b) ? ipart(a) : ipart(b);
  int lo = a->scale > b->scale ? a->scale : b->scale;
  for (int k = hi - 1; k >= -lo; k--) {
    int x = dig(a, k), y = dig(b, k);
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}
int num_cmp(const num *a, const num *b) {
  int za = num_is_zero(a), zb = num_is_zero(b);
  int sa = za ? 0 : a->sign, sb = zb ? 0 : b->sign;
  if (sa != sb) return sa < sb ? -1 : 1;
  if (!sa) return 0;
  int c = cmp_abs(a, b);
  return sa < 0 ? -c : c;
}
static num *add_abs(const num *a, const num *b) {
  int s = a->scale > b->scale ? a->scale : b->scale;
  int hi = (ipart(a) > ipart(b) ? ipart(a) : ipart(b)) + 1;
  num *r = alloc(s + hi, s);
  int carry = 0;
  for (int k = -s; k < hi; k++) {
    int v = dig(a, k) + dig(b, k) + carry;
    r->d[k + s] = v % 10;
    carry = v / 10;
  }
  return r;
}
static num *sub_abs(const num *a, const num *b) { /* |a| >= |b| */
  int s = a->scale > b->scale ? a->scale : b->scale;
  int hi = ipart(a) > ipart(b) ? ipart(a) : ipart(b);
  num *r = alloc(s + hi, s);
  int borrow = 0;
  for (int k = -s; k < hi; k++) {
    int v = dig(a, k) - dig(b, k) - borrow;
    borrow = v < 0;
    r->d[k + s] = v + (borrow ? 10 : 0);
  }
  return r;
}
static num *addsub(const num *a, const num *b, int bsign) {
  num *r;
  if (a->sign == bsign) { r = add_abs(a, b); r->sign = a->sign; }
  else if (cmp_abs(a, b) >= 0) { r = sub_abs(a, b); r->sign = a->sign; }
  else { r = sub_abs(b, a); r->sign = bsign; }
  return norm(r);
}
num *num_add(const num *a, const num *b) { return addsub(a, b, b->sign); }
num *num_sub(const num *a, const num *b) { return addsub(a, b, -b->sign); }

num *num_trunc(const num *a, int scale) {
  if (scale >= a->scale) {
    num *r = alloc(a->n + (scale - a->scale), scale);
    memcpy(r->d + (scale - a->scale), a->d, a->n);
    r->sign = a->sign;
    return norm(r);
  }
  int drop = a->scale - scale;
  num *r = alloc(a->n - drop, scale);
  memcpy(r->d, a->d + drop, a->n - drop);
  r->sign = a->sign;
  return norm(r);
}

num *num_mul(const num *a, const num *b, int scale) {
  int n = a->n + b->n;
  int *acc = calloc(n + 1, sizeof *acc);
  for (int i = 0; i < a->n; i++) {
    if (!a->d[i]) continue;
    for (int j = 0; j < b->n; j++) acc[i + j] += a->d[i] * b->d[j];
    if ((i & 31) == 31) for (int k = 0; k < n; k++) if (acc[k] >= 10) { acc[k + 1] += acc[k] / 10; acc[k] %= 10; }
  }
  num *r = alloc(n, a->scale + b->scale);
  int carry = 0;
  for (int k = 0; k < n; k++) { int v = acc[k] + carry; r->d[k] = v % 10; carry = v / 10; }
  free(acc);
  r->sign = a->sign * b->sign;
  int keep = a->scale + b->scale, want = scale;
  if (a->scale > want) want = a->scale;
  if (b->scale > want) want = b->scale;
  if (want < keep) { num *t = num_trunc(r, want); num_free(r); r = t; }
  return norm(r);
}

/* integer magnitudes as digit arrays (least significant first) */
static int icmp(const unsigned char *a, int an, const unsigned char *b, int bn) {
  while (an > 0 && !a[an - 1]) an--;
  while (bn > 0 && !b[bn - 1]) bn--;
  if (an != bn) return an < bn ? -1 : 1;
  for (int i = an - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
static void isub(unsigned char *a, int an, const unsigned char *b, int bn) {
  int borrow = 0;
  for (int i = 0; i < an; i++) {
    int v = a[i] - (i < bn ? b[i] : 0) - borrow;
    borrow = v < 0;
    a[i] = v + (borrow ? 10 : 0);
  }
}
/* q = floor(N / D); N has nn digits, D dn digits; q gets nn digits */
static void idiv(const unsigned char *N, int nn, const unsigned char *D, int dn, unsigned char *q) {
  unsigned char *r = calloc(dn + 2, 1);
  int rn = dn + 1;
  for (int i = nn - 1; i >= 0; i--) {
    memmove(r + 1, r, rn - 1); /* r = r * 10 + N[i] */
    r[0] = N[i];
    int c = 0;
    while (icmp(r, rn, D, dn) >= 0) { isub(r, rn, D, dn); c++; }
    q[i] = c;
  }
  free(r);
}

num *num_div(const num *a, const num *b, int scale) {
  if (num_is_zero(b)) return NULL;
  /* q = floor(A * 10^(b.scale + scale) / (B * 10^a.scale)) with A, B the digit integers */
  int shift_n = b->scale + scale, shift_d = a->scale;
  int nn = a->n + shift_n, dn = b->n + shift_d;
  unsigned char *N = calloc(nn + 1, 1), *D = calloc(dn + 1, 1);
  memcpy(N + shift_n, a->d, a->n);
  memcpy(D + shift_d, b->d, b->n);
  while (dn > 1 && !D[dn - 1]) dn--;
  num *r = alloc(nn, scale);
  idiv(N, nn, D, dn, r->d);
  free(N); free(D);
  r->sign = a->sign * b->sign;
  return norm(r);
}

num *num_mod(const num *a, const num *b, int scale) {
  num *q = num_div(a, b, scale);
  if (!q) return NULL;
  num *t = num_mul(q, b, q->scale + b->scale);
  num *r = num_sub(a, t);
  num_free(q); num_free(t);
  return r;
}

num *num_pow(const num *a, long e, int scale) {
  int neg = e < 0;
  unsigned long u = neg ? -(unsigned long)e : (unsigned long)e;
  long want = (long)a->scale * (long)u;
  int rs = scale > a->scale ? scale : a->scale;
  if (!neg && want < rs) rs = want;
  num *r = num_int(1), *base = num_dup(a);
  int work = (neg ? scale : rs) + 2;
  if (work < a->scale) work = a->scale;
  while (u) {
    if (u & 1) { num *t = num_mul(r, base, work); num_free(r); r = t; }
    u >>= 1;
    if (u) { num *t = num_mul(base, base, work); num_free(base); base = t; }
  }
  num_free(base);
  if (neg) {
    num *one = num_int(1), *t = num_div(one, r, scale);
    num_free(one); num_free(r);
    return t ? t : num_zero();
  }
  num *t = num_trunc(r, rs);
  num_free(r);
  return t;
}

num *num_sqrt(const num *a, int scale) {
  if (a->sign < 0 && !num_is_zero(a)) return NULL;
  int rs = scale > a->scale ? scale : a->scale;
  if (num_is_zero(a)) return alloc(0, rs > 0 ? 0 : 0);
  int work = rs + 2;
  /* start from 10^(digits/2), close to the root, then Newton */
  int ip = ipart(a);
  num *x = num_int(1);
  if (ip > 1) { num *ten = num_int(10), *t = num_pow(ten, (ip + 1) / 2, 0); num_free(x); num_free(ten); x = t; }
  num *two = num_int(2);
  for (int iter = 0; iter < 10000; iter++) {
    num *q = num_div(a, x, work), *s = num_add(x, q), *nx = num_div(s, two, work);
    num_free(q); num_free(s);
    int same = num_cmp(nx, x) == 0;
    num_free(x);
    x = nx;
    if (same) break;
  }
  num_free(two);
  num *r = num_trunc(x, rs);
  num_free(x);
  return r;
}

static int digval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
num *num_parse(const char *s, int ibase) {
  const char *dot = strchr(s, '.');
  int il = dot ? (int)(dot - s) : (int)strlen(s), fl = dot ? (int)strlen(dot + 1) : 0;
  if (ibase == 10) {
    num *r = alloc(il + fl, fl);
    for (int i = 0; i < il; i++) r->d[fl + il - 1 - i] = digval(s[i]) > 9 ? 9 : digval(s[i]);
    for (int i = 0; i < fl; i++) r->d[fl - 1 - i] = digval(dot[1 + i]) > 9 ? 9 : digval(dot[1 + i]);
    return norm(r);
  }
  if (il == 1 && !fl) return num_int(digval(s[0])); /* a lone digit keeps its value */
  num *r = num_zero(), *b = num_int(ibase);
  for (int i = 0; i < il; i++) {
    int v = digval(s[i]);
    if (v >= ibase) v = ibase - 1;
    num *t = num_mul(r, b, 0), *dv = num_int(v);
    num_free(r);
    r = num_add(t, dv);
    num_free(t); num_free(dv);
  }
  if (fl) {
    num *f = num_zero(), *p = num_int(1);
    for (int i = 0; i < fl; i++) {
      int v = digval(dot[1 + i]);
      if (v >= ibase) v = ibase - 1;
      num *t = num_mul(f, b, 0), *dv = num_int(v), *tp = num_mul(p, b, 0);
      num_free(f); num_free(p);
      f = num_add(t, dv);
      p = tp;
      num_free(t); num_free(dv);
    }
    num *q = num_div(f, p, fl), *t = num_add(r, q);
    num_free(f); num_free(p); num_free(q); num_free(r);
    r = t;
  }
  num_free(b);
  return r;
}

/* divide a digit integer by a small number in place; returns the remainder */
static int divsmall(unsigned char *d, int n, int by) {
  int rem = 0;
  for (int i = n - 1; i >= 0; i--) { int v = rem * 10 + d[i]; d[i] = v / by; rem = v % by; }
  return rem;
}
static int iszero_arr(const unsigned char *d, int n) { for (int i = 0; i < n; i++) if (d[i]) return 0; return 1; }

struct sbuf { char *s; int n, cap; };
static void sput(struct sbuf *b, const char *s) {
  int l = strlen(s);
  if (b->n + l + 1 > b->cap) { while (b->n + l + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 64; b->s = realloc(b->s, b->cap); }
  memcpy(b->s + b->n, s, l + 1);
  b->n += l;
}
static void put_digit(struct sbuf *b, int v, int obase) {
  char t[16];
  if (obase <= 16) { t[0] = "0123456789ABCDEF"[v]; t[1] = 0; }
  else { int w = snprintf(t, sizeof t, "%d", obase - 1); snprintf(t, sizeof t, " %0*d", w, v); }
  sput(b, t);
}
char *num_str(const num *a, int obase) {
  struct sbuf b = {0};
  sput(&b, "");
  if (num_is_zero(a)) { sput(&b, "0"); return b.s; }
  if (a->sign < 0) sput(&b, "-");
  int ip = ipart(a);
  if (obase == 10) {
    for (int i = a->n - 1; i >= a->scale; i--) { char c[2] = {(char)('0' + a->d[i]), 0}; sput(&b, c); }
    if (a->scale) {
      sput(&b, ".");
      for (int i = a->scale - 1; i >= 0; i--) { char c[2] = {(char)('0' + a->d[i]), 0}; sput(&b, c); }
    }
    return b.s;
  }
  if (ip > 0) {
    unsigned char *d = malloc(ip);
    memcpy(d, a->d + a->scale, ip);
    int cap = 64, k = 0, *digs = malloc(cap * sizeof *digs);
    while (!iszero_arr(d, ip)) {
      if (k == cap) digs = realloc(digs, (cap *= 2) * sizeof *digs);
      digs[k++] = divsmall(d, ip, obase);
    }
    for (int i = k - 1; i >= 0; i--) put_digit(&b, digs[i], obase);
    free(d); free(digs);
  } else if (obase > 16) put_digit(&b, 0, obase);
  if (a->scale) {
    sput(&b, ".");
    num *f = alloc(a->scale, a->scale), *ob = num_int(obase);
    memcpy(f->d, a->d, a->scale);
    /* enough output digits that obase^k >= 10^scale */
    num *lim = num_int(1), *ten = num_int(10);
    for (int i = 0; i < a->scale; i++) { num *t = num_mul(lim, ten, 0); num_free(lim); lim = t; }
    num *p = num_int(1);
    while (num_cmp(p, lim) < 0) {
      num *t = num_mul(f, ob, f->scale);
      num_free(f);
      f = t;
      int v = 0;
      for (int i = f->n - 1; i >= f->scale; i--) v = v * 10 + f->d[i];
      for (int i = f->scale; i < f->n; i++) f->d[i] = 0;
      put_digit(&b, v, obase);
      num *tp = num_mul(p, ob, 0);
      num_free(p);
      p = tp;
    }
    num_free(f); num_free(ob); num_free(lim); num_free(ten); num_free(p);
  }
  return b.s;
}
long num_long(const num *a, int *overflow) {
  long long v = 0;
  *overflow = 0;
  for (int i = a->n - 1; i >= a->scale; i--) {
    v = v * 10 + a->d[i];
    if (v > 2147483647LL) { *overflow = 1; return 0; } /* long is 32 bits on wasm32 */
  }
  return (long)(a->sign < 0 ? -v : v);
}
int num_length(const num *a) {
  int n = a->n;
  while (n > a->scale && n > 0 && a->d[n - 1] == 0) n--;
  if (n == a->scale) { /* 0.00123: the leading zeros of the fraction do not count */
    int k = n;
    while (k > 0 && a->d[k - 1] == 0) k--;
    return k ? (a->scale > k ? a->scale : k) : (a->scale ? a->scale : 1);
  }
  return n ? n : 1;
}
