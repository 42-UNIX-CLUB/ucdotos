// SPDX-License-Identifier: GPL-2.0-or-later
/* MD5 (RFC 1321), SHA-256 and SHA-512 (FIPS 180-4). */
#include "arc_int.h"

#define ROR32(x, n) ((x) >> (n) | (x) << (32 - (n)))
#define ROL32(x, n) ((x) << (n) | (x) >> (32 - (n)))
#define ROR64(x, n) ((x) >> (n) | (x) << (64 - (n)))

/* ---- SHA-256 ---- */
static const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = rd32be(p + 4 * i);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = k + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void arc_sha256_init(arc_sha256 *c) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, iv, sizeof iv);
  c->len = 0;
  c->n = 0;
}

void arc_sha256_update(arc_sha256 *c, const void *buf, unsigned long n) {
  if (n == 0) return; /* buf may be NULL then; memcpy must not see it */
  const uint8_t *p = buf;
  c->len += n;
  if (c->n) {
    unsigned k = 64 - c->n;
    if (k > n) k = n;
    memcpy(c->buf + c->n, p, k);
    c->n += k; p += k; n -= k;
    if (c->n < 64) return;
    sha256_block(c->h, c->buf);
    c->n = 0;
  }
  while (n >= 64) { sha256_block(c->h, p); p += 64; n -= 64; }
  memcpy(c->buf, p, n);
  c->n = n;
}

void arc_sha256_final(arc_sha256 *c, unsigned char out[32]) {
  uint64_t bits = c->len * 8;
  c->buf[c->n++] = 0x80;
  if (c->n > 56) { memset(c->buf + c->n, 0, 64 - c->n); sha256_block(c->h, c->buf); c->n = 0; }
  memset(c->buf + c->n, 0, 56 - c->n);
  for (int i = 0; i < 8; i++) c->buf[56 + i] = bits >> (56 - 8 * i);
  sha256_block(c->h, c->buf);
  for (int i = 0; i < 8; i++) wr32be(out + 4 * i, c->h[i]);
}

/* ---- SHA-512 ---- */
static const uint64_t K512[80] = {
  0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
  0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
  0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
  0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
  0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
  0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
  0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
  0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
  0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
  0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
  0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
  0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
  0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
  0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
  0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
  0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

static uint64_t rd64be(const uint8_t *p) { return (uint64_t)rd32be(p) << 32 | rd32be(p + 4); }

static void sha512_block(unsigned long long h[8], const uint8_t *p) {
  uint64_t w[80];
  for (int i = 0; i < 16; i++) w[i] = rd64be(p + 8 * i);
  for (int i = 16; i < 80; i++) {
    uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 80; i++) {
    uint64_t t1 = k + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
    uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void arc_sha512_init(arc_sha512 *c) {
  static const uint64_t iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
  memcpy(c->h, iv, sizeof iv);
  c->len = 0;
  c->n = 0;
}

void arc_sha512_update(arc_sha512 *c, const void *buf, unsigned long n) {
  if (n == 0) return; /* buf may be NULL then; memcpy must not see it */
  const uint8_t *p = buf;
  c->len += n;
  if (c->n) {
    unsigned k = 128 - c->n;
    if (k > n) k = n;
    memcpy(c->buf + c->n, p, k);
    c->n += k; p += k; n -= k;
    if (c->n < 128) return;
    sha512_block(c->h, c->buf);
    c->n = 0;
  }
  while (n >= 128) { sha512_block(c->h, p); p += 128; n -= 128; }
  memcpy(c->buf, p, n);
  c->n = n;
}

void arc_sha512_final(arc_sha512 *c, unsigned char out[64]) {
  uint64_t bits = c->len * 8; /* the 128-bit length's high half is zero for inputs under 2^61 bytes */
  c->buf[c->n++] = 0x80;
  if (c->n > 112) { memset(c->buf + c->n, 0, 128 - c->n); sha512_block(c->h, c->buf); c->n = 0; }
  memset(c->buf + c->n, 0, 120 - c->n);
  for (int i = 0; i < 8; i++) c->buf[120 + i] = bits >> (56 - 8 * i);
  sha512_block(c->h, c->buf);
  for (int i = 0; i < 8; i++) {
    wr32be(out + 8 * i, c->h[i] >> 32);
    wr32be(out + 8 * i + 4, (uint32_t)c->h[i]);
  }
}

/* ---- MD5 ---- */
static const uint32_t KMD5[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t SMD5[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t h[4], const uint8_t *p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++) m[i] = rd32le(p + 4 * i);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + ROL32(a + f + KMD5[i] + m[g], SMD5[i]);
    a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void arc_md5_init(arc_md5 *c) {
  c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476;
  c->len = 0;
  c->n = 0;
}

void arc_md5_update(arc_md5 *c, const void *buf, unsigned long n) {
  if (n == 0) return; /* buf may be NULL then; memcpy must not see it */
  const uint8_t *p = buf;
  c->len += n;
  if (c->n) {
    unsigned k = 64 - c->n;
    if (k > n) k = n;
    memcpy(c->buf + c->n, p, k);
    c->n += k; p += k; n -= k;
    if (c->n < 64) return;
    md5_block(c->h, c->buf);
    c->n = 0;
  }
  while (n >= 64) { md5_block(c->h, p); p += 64; n -= 64; }
  memcpy(c->buf, p, n);
  c->n = n;
}

void arc_md5_final(arc_md5 *c, unsigned char out[16]) {
  uint64_t bits = c->len * 8;
  c->buf[c->n++] = 0x80;
  if (c->n > 56) { memset(c->buf + c->n, 0, 64 - c->n); md5_block(c->h, c->buf); c->n = 0; }
  memset(c->buf + c->n, 0, 56 - c->n);
  for (int i = 0; i < 8; i++) c->buf[56 + i] = bits >> (8 * i);
  md5_block(c->h, c->buf);
  for (int i = 0; i < 4; i++) wr32le(out + 4 * i, c->h[i]);
}

/* ---- SHA-384 (FIPS 180-4, 6.5): SHA-512 with its own initial values, cut to 48 bytes ---- */
void arc_sha384_init(arc_sha512 *c) {
  static const uint64_t iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
                                 0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
  memcpy(c->h, iv, sizeof iv);
  c->len = 0;
  c->n = 0;
}
void arc_sha384_final(arc_sha512 *c, unsigned char out[48]) {
  unsigned char full[64];
  arc_sha512_final(c, full);
  memcpy(out, full, 48);
}

/* ---- SHA-1 (FIPS 180-4, 6.1). OpenPGP version 4 key fingerprints are SHA-1
 * values, so the signature code needs it to name keys. It is broken for
 * signatures and nothing here uses it to check one. ---- */
static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = rd32be(p + 4 * i);
  for (int i = 16; i < 80; i++) w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else { f = b ^ c ^ d; k = 0xca62c1d6; }
    uint32_t t = ROL32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = ROL32(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void arc_sha1_init(arc_sha1 *c) {
  c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
  c->len = 0;
  c->n = 0;
}
void arc_sha1_update(arc_sha1 *c, const void *buf, unsigned long n) {
  if (n == 0) return;
  const uint8_t *p = buf;
  c->len += n;
  if (c->n) {
    unsigned k = 64 - c->n;
    if (k > n) k = n;
    memcpy(c->buf + c->n, p, k);
    c->n += k; p += k; n -= k;
    if (c->n < 64) return;
    sha1_block(c->h, c->buf);
    c->n = 0;
  }
  while (n >= 64) { sha1_block(c->h, p); p += 64; n -= 64; }
  memcpy(c->buf, p, n);
  c->n = n;
}
void arc_sha1_final(arc_sha1 *c, unsigned char out[20]) {
  uint64_t bits = c->len * 8;
  c->buf[c->n++] = 0x80;
  if (c->n > 56) { memset(c->buf + c->n, 0, 64 - c->n); sha1_block(c->h, c->buf); c->n = 0; }
  memset(c->buf + c->n, 0, 56 - c->n);
  for (int i = 0; i < 8; i++) c->buf[56 + i] = bits >> (56 - 8 * i);
  sha1_block(c->h, c->buf);
  for (int i = 0; i < 5; i++) wr32be(out + 4 * i, c->h[i]);
}
