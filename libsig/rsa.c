// SPDX-License-Identifier: GPL-2.0-or-later
/* Multi-precision arithmetic for RSA signature verification, and the
 * verification itself (RFC 8017 sections 5.2.2, 8.2.2 and 9.2).
 *
 * Numbers are arrays of 32-bit limbs, least significant first, with 64-bit
 * products. Modular exponentiation uses Montgomery multiplication (the CIOS
 * form: multiply and reduce one limb at a time), left to right over the
 * exponent bits. For e = 65537 that is 16 squarings and 1 multiplication,
 * plus 2 to enter and leave the Montgomery form.
 *
 * Nothing here runs in constant time, and it does not need to: verification
 * only handles public values (the key, the signature, the digest). This code
 * must never be used with a secret exponent. */
#include "sig_int.h"

#define MAXL (SIG_MAX_MODULUS_BITS / 32 + 1)

static int load_be(uint32_t *out, int nl, const uint8_t *p, size_t len) {
  while (len && !*p) { p++; len--; }      /* leading zero bytes do not count */
  if (len > (size_t)nl * 4) return -1;
  memset(out, 0, nl * sizeof *out);
  for (size_t i = 0; i < len; i++) out[i / 4] |= (uint32_t)p[len - 1 - i] << (8 * (i % 4));
  return 0;
}
static void store_be(const uint32_t *v, int nl, uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) p[len - 1 - i] = i / 4 < (size_t)nl ? (uint8_t)(v[i / 4] >> (8 * (i % 4))) : 0;
}
static int cmp(const uint32_t *a, const uint32_t *b, int n) {
  for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
static uint32_t sub_in(uint32_t *a, const uint32_t *b, int n) {  /* a -= b, returns the borrow */
  uint64_t borrow = 0;
  for (int i = 0; i < n; i++) {
    uint64_t d = (uint64_t)a[i] - b[i] - borrow;
    a[i] = (uint32_t)d;
    borrow = d >> 63;
  }
  return (uint32_t)borrow;
}
static int top_limbs(const uint32_t *a, int n) { while (n > 0 && !a[n - 1]) n--; return n; }

/* -m^-1 mod 2^32, for odd m0, by Newton's iteration (each step doubles the correct low bits). */
static uint32_t neg_inv32(uint32_t m0) {
  uint32_t x = m0;            /* correct to 3 bits for odd m0 */
  for (int i = 0; i < 5; i++) x *= 2 - m0 * x;
  return (uint32_t)0 - x;
}
/* r = a * b * 2^(-32n) mod m, for a, b < m. r may alias a or b. */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int n, uint32_t minv) {
  uint32_t t[MAXL + 2];
  memset(t, 0, (n + 2) * sizeof *t);
  for (int i = 0; i < n; i++) {
    uint64_t c = 0, bi = b[i];
    for (int j = 0; j < n; j++) {
      uint64_t s = t[j] + a[j] * bi + c;
      t[j] = (uint32_t)s;
      c = s >> 32;
    }
    uint64_t s = (uint64_t)t[n] + c;
    t[n] = (uint32_t)s;
    t[n + 1] = (uint32_t)(s >> 32);
    uint64_t u = (uint32_t)(t[0] * minv);
    s = t[0] + u * m[0];
    c = s >> 32;
    for (int j = 1; j < n; j++) {
      s = t[j] + u * m[j] + c;
      t[j - 1] = (uint32_t)s;
      c = s >> 32;
    }
    s = (uint64_t)t[n] + c;
    t[n - 1] = (uint32_t)s;
    t[n] = t[n + 1] + (uint32_t)(s >> 32);
  }
  /* t < 2m here; one subtraction brings it below m */
  if (t[n] || cmp(t, m, n) >= 0) sub_in(t, m, n);
  memcpy(r, t, n * sizeof *r);
}

int sig_modexp(const unsigned char *b, size_t blen, const unsigned char *e, size_t elen,
               const unsigned char *m, size_t mlen, unsigned char *out) {
  uint32_t M[MAXL], B[MAXL], X[MAXL], A[MAXL], R2[MAXL], one[MAXL];
  if (load_be(M, MAXL, m, mlen) < 0) return -1;
  int n = top_limbs(M, MAXL);
  if (n == 0 || !(M[0] & 1) || n > MAXL - 1) return -1;
  if (load_be(B, MAXL, b, blen) < 0 || top_limbs(B, MAXL) > n || cmp(B, M, n) >= 0) return -1;
  if (n == 1 && M[0] == 1) { memset(out, 0, mlen); return 0; }   /* everything is 0 modulo 1 */
  uint32_t minv = neg_inv32(M[0]);
  /* R2 = R^2 mod m, R = 2^(32n), needed to enter the Montgomery form.
   * Start from 2^(bits-1), which is below m, and double it (subtracting m
   * when needed) up to 2^(32n+1) mod m: at most 33 doublings. That value is
   * 2 in Montgomery form; raising it to the power 32n with Montgomery
   * products gives 2^(32n) in Montgomery form, which is R * R mod m. */
  int top = 31;
  while (!(M[n - 1] >> top & 1)) top--;
  int bits = 32 * (n - 1) + top + 1;
  memset(R2, 0, n * sizeof *R2);
  R2[(bits - 1) / 32] = 1u << ((bits - 1) % 32);
  for (int i = bits - 1; i < 32 * n + 1; i++) {
    uint32_t carry = R2[n - 1] >> 31;
    for (int j = n - 1; j > 0; j--) R2[j] = R2[j] << 1 | R2[j - 1] >> 31;
    R2[0] <<= 1;
    if (carry || cmp(R2, M, n) >= 0) sub_in(R2, M, n);
  }
  {
    uint32_t two[MAXL];
    memcpy(two, R2, n * sizeof *two);
    unsigned k = 32u * (unsigned)n;
    int hb = 31;
    while (!(k >> hb & 1)) hb--;
    for (int b = hb - 1; b >= 0; b--) {
      mont_mul(R2, R2, R2, M, n, minv);
      if (k >> b & 1) mont_mul(R2, R2, two, M, n, minv);
    }
  }
  mont_mul(X, B, R2, M, n, minv);         /* X = b in Montgomery form */
  memset(one, 0, n * sizeof *one);
  one[0] = 1;
  mont_mul(A, one, R2, M, n, minv);       /* A = 1 in Montgomery form */
  while (elen && !*e) { e++; elen--; }
  for (size_t i = 0; i < elen; i++)
    for (int bit = 7; bit >= 0; bit--) {
      mont_mul(A, A, A, M, n, minv);
      if (e[i] >> bit & 1) mont_mul(A, A, X, M, n, minv);
    }
  mont_mul(A, A, one, M, n, minv);        /* leave the Montgomery form */
  store_be(A, n, out, mlen);
  return 0;
}

/* The second implementation: schoolbook product, then reduction one bit at a
 * time. Slow and simple; used only by the tests. */
static void mod_slow(uint32_t *r, const uint32_t *a, int an, const uint32_t *m, int n) {
  uint32_t t[MAXL + 1];
  memset(t, 0, (n + 1) * sizeof *t);
  for (int i = an * 32 - 1; i >= 0; i--) {
    for (int j = n; j > 0; j--) t[j] = t[j] << 1 | t[j - 1] >> 31;
    t[0] = t[0] << 1 | (a[i / 32] >> (i % 32) & 1);
    if (t[n] || cmp(t, m, n) >= 0) { uint32_t bw = sub_in(t, m, n); t[n] -= bw; }
  }
  memcpy(r, t, n * sizeof *r);
}
static void mul_slow(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) { /* r has 2n limbs */
  memset(r, 0, 2 * n * sizeof *r);
  for (int i = 0; i < n; i++) {
    uint64_t c = 0;
    for (int j = 0; j < n; j++) {
      uint64_t s = r[i + j] + (uint64_t)a[j] * b[i] + c;
      r[i + j] = (uint32_t)s;
      c = s >> 32;
    }
    r[i + n] = (uint32_t)c;
  }
}
int sig_modexp_slow(const unsigned char *b, size_t blen, const unsigned char *e, size_t elen,
                    const unsigned char *m, size_t mlen, unsigned char *out) {
  uint32_t M[MAXL], B[MAXL], A[MAXL], P[2 * MAXL];
  if (load_be(M, MAXL, m, mlen) < 0) return -1;
  int n = top_limbs(M, MAXL);
  if (n == 0 || !(M[0] & 1) || n > MAXL - 1) return -1;
  if (load_be(B, MAXL, b, blen) < 0 || top_limbs(B, MAXL) > n || cmp(B, M, n) >= 0) return -1;
  if (n == 1 && M[0] == 1) { memset(out, 0, mlen); return 0; }
  memset(A, 0, n * sizeof *A);
  A[0] = 1;
  while (elen && !*e) { e++; elen--; }
  for (size_t i = 0; i < elen; i++)
    for (int bit = 7; bit >= 0; bit--) {
      mul_slow(P, A, A, n); mod_slow(A, P, 2 * n, M, n);
      if (e[i] >> bit & 1) { mul_slow(P, A, B, n); mod_slow(A, P, 2 * n, M, n); }
    }
  store_be(A, n, out, mlen);
  return 0;
}

/* The DER prefixes of DigestInfo for each hash (RFC 8017 9.2, note 1). */
static const uint8_t di_sha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
static const uint8_t di_sha384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
static const uint8_t di_sha512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

/* RSASSA-PKCS1-v1_5 verification. The check builds the encoded message the
 * signer must have produced,
 *     EM = 0x00 0x01 0xFF...0xFF 0x00 DigestInfo-prefix digest
 * (as long as the modulus, at least 8 bytes of 0xFF), and compares it with
 * s^e mod n as a whole. It never parses the padding of s^e mod n. Parsing
 * verifiers have been forged again and again: one that skips the 0xFF bytes
 * up to the first 0x00 and then reads a DigestInfo accepts garbage after the
 * digest, or a DigestInfo with extra fields or a wrong length, and with a
 * small exponent (e = 3) such an encoded message can be produced without the
 * key, by taking a cube root (Bleichenbacher, 2006). With a whole-message
 * comparison only one value of s^e mod n is accepted. */
int sig_rsa_verify(const unsigned char *n, size_t nlen, const unsigned char *e, size_t elen,
                   const unsigned char *s, size_t slen, int hashalgo, const unsigned char *digest) {
  const uint8_t *di;
  size_t dil, hl;
  switch (hashalgo) {
  case 8: di = di_sha256; dil = sizeof di_sha256; hl = 32; break;
  case 9: di = di_sha384; dil = sizeof di_sha384; hl = 48; break;
  case 10: di = di_sha512; dil = sizeof di_sha512; hl = 64; break;
  default: return -1;
  }
  while (nlen && !*n) { n++; nlen--; }
  while (elen && !*e) { e++; elen--; }
  if (!nlen || nlen > SIG_MAX_MODULUS_BITS / 8 || !(n[nlen - 1] & 1)) return -1;
  if (!elen || !(e[elen - 1] & 1) || (elen == 1 && e[0] < 3) || elen > nlen) return -1;
  size_t k = nlen;
  if (k < dil + hl + 11) return -1;
  uint8_t *em = malloc(2 * k);
  if (!em) return -1;
  uint8_t *got = em + k;
  em[0] = 0x00;
  em[1] = 0x01;
  memset(em + 2, 0xff, k - 3 - dil - hl);
  em[k - dil - hl - 1] = 0x00;
  memcpy(em + k - dil - hl, di, dil);
  memcpy(em + k - hl, digest, hl);
  int r = sig_modexp(s, slen, e, elen, n, nlen, got);   /* -1 also when s >= n */
  int ok = r == 0 && memcmp(em, got, k) == 0;
  free(em);
  return ok;
}
