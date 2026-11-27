// SPDX-License-Identifier: GPL-2.0-or-later
/* Ed25519 signature verification (RFC 8032, sections 5.1.3, 5.1.4 and 5.1.7).
 *
 * Field elements modulo p = 2^255 - 19 are eight 32-bit limbs, least
 * significant first, kept below 2^256 (not always below p) between steps.
 * Products are reduced with 2^256 = 38 (mod p). Points are in extended
 * coordinates (X:Y:Z:T) on -x^2 + y^2 = 1 + d x^2 y^2, added with the complete
 * formula of RFC 8032 5.1.4 (the same formula doubles).
 *
 * Verification, with the choices the RFC leaves open:
 *   - S must be below the group order L, as 5.1.7 requires.
 *   - The public key A must decode (5.1.3: y < p, x = 0 only with sign bit 0)
 *     and must not be a point of small order (checked when a key is loaded,
 *     see sig_ed25519_small_order; this function does not refuse it).
 *   - R is not decoded. The check is the cofactorless equation
 *         encode([S]B - [k]A) == R, byte for byte,
 *     with k = SHA-512(R || A || M) mod L. The encoding of a point is
 *     canonical, so an R with y >= p or a wrong sign bit never matches.
 *     RFC 8032 allows this check or the cofactored one ([8][S]B = [8]R +
 *     [8][k]A); they differ only for signatures crafted with small-order
 *     components, which an honest signer never makes.
 * No secret is ever handled, so nothing here needs to run in constant time. */
#include "sig_int.h"

typedef struct { uint32_t v[8]; } fe;
typedef struct { fe X, Y, Z, T; } ge;

static const uint32_t P25519[8] = {0xffffffed, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff};
/* L = 2^252 + 27742317777372353535851937790883648493 */
static const uint32_t L25519[8] = {0x5cf5d3ed, 0x5812631a, 0xa2f79cd6, 0x14def9de, 0, 0, 0, 0x10000000};

static void fold(fe *r, uint64_t c) {   /* add c * 2^256 = c * 38 */
  while (c) {
    uint64_t t = c * 38;
    for (int i = 0; i < 8; i++) { t += r->v[i]; r->v[i] = (uint32_t)t; t >>= 32; }
    c = t;
  }
}
static void fe_add(fe *r, const fe *a, const fe *b) {
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) { c += (uint64_t)a->v[i] + b->v[i]; r->v[i] = (uint32_t)c; c >>= 32; }
  fold(r, c);
}
static void fe_sub(fe *r, const fe *a, const fe *b) {
  uint64_t d;
  uint32_t bw = 0;
  for (int i = 0; i < 8; i++) { d = (uint64_t)a->v[i] - b->v[i] - bw; r->v[i] = (uint32_t)d; bw = (uint32_t)(d >> 63); }
  while (bw) {  /* the result wrapped by 2^256 = 38 (mod p): take 38 away again */
    d = (uint64_t)r->v[0] - 38; r->v[0] = (uint32_t)d; bw = (uint32_t)(d >> 63);
    for (int i = 1; i < 8 && bw; i++) { d = (uint64_t)r->v[i] - 1; r->v[i] = (uint32_t)d; bw = (uint32_t)(d >> 63); }
  }
}
static void fe_mul(fe *r, const fe *a, const fe *b) {
  uint32_t t[16] = {0};
  for (int i = 0; i < 8; i++) {
    uint64_t c = 0;
    for (int j = 0; j < 8; j++) { c += (uint64_t)t[i + j] + (uint64_t)a->v[i] * b->v[j]; t[i + j] = (uint32_t)c; c >>= 32; }
    t[i + 8] = (uint32_t)c;
  }
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) { c += (uint64_t)t[i] + (uint64_t)t[i + 8] * 38; r->v[i] = (uint32_t)c; c >>= 32; }
  fold(r, c);
}
static int ge_limbs(const uint32_t *a, const uint32_t *b) {  /* a >= b, eight limbs */
  for (int i = 7; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i];
  return 1;
}
static void fe_freeze(fe *r) {  /* fully reduce: below 2^256 < 3p, so at most two subtractions */
  for (int k = 0; k < 2; k++)
    if (ge_limbs(r->v, P25519)) {
      uint64_t d; uint32_t bw = 0;
      for (int i = 0; i < 8; i++) { d = (uint64_t)r->v[i] - P25519[i] - bw; r->v[i] = (uint32_t)d; bw = (uint32_t)(d >> 63); }
    }
}
static void fe_set(fe *r, uint32_t x) { memset(r, 0, sizeof *r); r->v[0] = x; }
static void fe_frombytes(fe *r, const uint8_t b[32]) {
  for (int i = 0; i < 8; i++) r->v[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
}
static void fe_tobytes(uint8_t b[32], const fe *a) {
  fe t = *a;
  fe_freeze(&t);
  for (int i = 0; i < 32; i++) b[i] = (uint8_t)(t.v[i / 4] >> (8 * (i % 4)));
}
static int fe_eq(const fe *a, const fe *b) {
  uint8_t x[32], y[32];
  fe_tobytes(x, a); fe_tobytes(y, b);
  return memcmp(x, y, 32) == 0;
}
static int fe_iszero(const fe *a) { fe z; fe_set(&z, 0); return fe_eq(a, &z); }
static int fe_isodd(const fe *a) { uint8_t x[32]; fe_tobytes(x, a); return x[0] & 1; }
static void fe_pow(fe *r, const fe *a, const uint8_t e[32]) {  /* e little-endian */
  fe acc; fe_set(&acc, 1);
  for (int i = 255; i >= 0; i--) {
    fe_mul(&acc, &acc, &acc);
    if (e[i / 8] >> (i % 8) & 1) fe_mul(&acc, &acc, a);
  }
  *r = acc;
}
/* exponents, little-endian: p - 2, (p - 5) / 8 = 2^252 - 3, (p - 1) / 4 = 2^253 - 5 */
static void exp_bytes(uint8_t e[32], uint8_t low, uint8_t high) { memset(e, 0xff, 32); e[0] = low; e[31] = high; }
static void fe_inv(fe *r, const fe *a) { uint8_t e[32]; exp_bytes(e, 0xeb, 0x7f); fe_pow(r, a, e); }

static fe D, D2, SQRTM1;
static ge BASE;
static int ready;
static int ge_decode(ge *p, const uint8_t s[32]);
static void init(void) {
  if (ready) return;
  fe a, b, zero;
  fe_set(&zero, 0);
  fe_set(&a, 121665); fe_set(&b, 121666);
  fe_inv(&b, &b);
  fe_sub(&a, &zero, &a);
  fe_mul(&D, &a, &b);                         /* d = -121665 / 121666 */
  fe_add(&D2, &D, &D);
  uint8_t e[32];
  fe_set(&a, 2);
  exp_bytes(e, 0xfb, 0x1f);
  fe_pow(&SQRTM1, &a, e);                     /* sqrt(-1) = 2^((p-1)/4) */
  uint8_t bb[32];
  memset(bb, 0x66, 32); bb[0] = 0x58;         /* B: y = 4/5, x even */
  ge_decode(&BASE, bb);
  ready = 1;
}

/* RFC 8032 5.1.3 */
static int ge_decode(ge *p, const uint8_t s[32]) {
  uint8_t yb[32];
  memcpy(yb, s, 32);
  int x0 = yb[31] >> 7;
  yb[31] &= 0x7f;
  fe y, u, v, v3, x, t, one;
  fe_frombytes(&y, yb);
  if (ge_limbs(y.v, P25519)) return -1;       /* y >= p: not canonical */
  fe_set(&one, 1);
  fe_mul(&u, &y, &y);
  fe_mul(&v, &u, &D);
  fe_sub(&u, &u, &one);                       /* u = y^2 - 1 */
  fe_add(&v, &v, &one);                       /* v = d y^2 + 1 */
  fe_mul(&v3, &v, &v); fe_mul(&v3, &v3, &v);  /* v^3 */
  fe_mul(&t, &v3, &v3); fe_mul(&t, &t, &v);   /* v^7 */
  fe_mul(&t, &t, &u);                         /* u v^7 */
  uint8_t e[32];
  exp_bytes(e, 0xfd, 0x0f);
  fe_pow(&t, &t, e);                          /* (u v^7)^((p-5)/8) */
  fe_mul(&x, &u, &v3); fe_mul(&x, &x, &t);    /* x = u v^3 (u v^7)^((p-5)/8) */
  fe vx2, negu, zero;
  fe_mul(&vx2, &x, &x); fe_mul(&vx2, &vx2, &v);
  fe_set(&zero, 0);
  fe_sub(&negu, &zero, &u);
  if (fe_eq(&vx2, &u)) {
  } else if (fe_eq(&vx2, &negu)) {
    fe_mul(&x, &x, &SQRTM1);
  } else return -1;                           /* not a square: no such point */
  if (fe_iszero(&x) && x0) return -1;
  if (fe_isodd(&x) != x0) fe_sub(&x, &zero, &x);
  p->X = x; p->Y = y; fe_set(&p->Z, 1); fe_mul(&p->T, &x, &y);
  return 0;
}
static void ge_encode(uint8_t s[32], const ge *p) {
  fe zi, x, y;
  fe_inv(&zi, &p->Z);
  fe_mul(&x, &p->X, &zi);
  fe_mul(&y, &p->Y, &zi);
  fe_tobytes(s, &y);
  s[31] |= (uint8_t)(fe_isodd(&x) << 7);
}
/* RFC 8032 5.1.4, a = -1 */
static void ge_add(ge *r, const ge *p, const ge *q) {
  fe a, b, c, d, e, f, g, h, t;
  fe_sub(&a, &p->Y, &p->X); fe_sub(&t, &q->Y, &q->X); fe_mul(&a, &a, &t);
  fe_add(&b, &p->Y, &p->X); fe_add(&t, &q->Y, &q->X); fe_mul(&b, &b, &t);
  fe_mul(&c, &p->T, &D2); fe_mul(&c, &c, &q->T);
  fe_mul(&d, &p->Z, &q->Z); fe_add(&d, &d, &d);
  fe_sub(&e, &b, &a); fe_sub(&f, &d, &c); fe_add(&g, &d, &c); fe_add(&h, &b, &a);
  fe_mul(&r->X, &e, &f); fe_mul(&r->Y, &g, &h); fe_mul(&r->T, &e, &h); fe_mul(&r->Z, &f, &g);
}
static void ge_identity(ge *p) { fe_set(&p->X, 0); fe_set(&p->Y, 1); fe_set(&p->Z, 1); fe_set(&p->T, 0); }
static int ge_is_identity(const ge *p) { return fe_iszero(&p->X) && fe_eq(&p->Y, &p->Z); }

int sig_ed25519_small_order(const unsigned char pub[32]) {
  init();
  ge a;
  if (ge_decode(&a, pub) < 0) return 0;
  for (int i = 0; i < 3; i++) ge_add(&a, &a, &a);
  return ge_is_identity(&a);
}

/* r = x mod L for a 512-bit little-endian x, one bit at a time. */
static void mod_l(uint32_t r[8], const uint8_t x[64]) {
  uint32_t t[9] = {0};
  for (int i = 511; i >= 0; i--) {
    for (int j = 8; j > 0; j--) t[j] = t[j] << 1 | t[j - 1] >> 31;
    t[0] = t[0] << 1 | (x[i / 8] >> (i % 8) & 1);
    if (t[8] || ge_limbs(t, L25519)) {
      uint64_t d; uint32_t bw = 0;
      for (int j = 0; j < 8; j++) { d = (uint64_t)t[j] - L25519[j] - bw; t[j] = (uint32_t)d; bw = (uint32_t)(d >> 63); }
      t[8] -= bw;
    }
  }
  memcpy(r, t, 32);
}

int sig_ed25519_verify(const unsigned char pub[32], const unsigned char *msg, size_t msglen, const unsigned char sig[64]) {
  init();
  uint32_t s[8], k[8];
  for (int i = 0; i < 8; i++) s[i] = (uint32_t)sig[32 + 4 * i] | (uint32_t)sig[33 + 4 * i] << 8 | (uint32_t)sig[34 + 4 * i] << 16 | (uint32_t)sig[35 + 4 * i] << 24;
  if (ge_limbs(s, L25519)) return 0;          /* S >= L */
  ge a;
  if (ge_decode(&a, pub) < 0) return 0;
  arc_sha512 h;
  uint8_t dig[64];
  arc_sha512_init(&h);
  arc_sha512_update(&h, sig, 32);
  arc_sha512_update(&h, pub, 32);
  arc_sha512_update(&h, msg, msglen);
  arc_sha512_final(&h, dig);
  mod_l(k, dig);
  fe zero; fe_set(&zero, 0);
  fe_sub(&a.X, &zero, &a.X);                  /* -A */
  fe_sub(&a.T, &zero, &a.T);
  ge q;
  ge_identity(&q);
  for (int i = 255; i >= 0; i--) {            /* [S]B + [k](-A), both scalars at once */
    ge_add(&q, &q, &q);
    if (s[i / 32] >> (i % 32) & 1) ge_add(&q, &q, &BASE);
    if (k[i / 32] >> (i % 32) & 1) ge_add(&q, &q, &a);
  }
  uint8_t enc[32];
  ge_encode(enc, &q);
  return memcmp(enc, sig, 32) == 0;
}
