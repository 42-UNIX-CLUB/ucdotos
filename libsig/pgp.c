// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenPGP packets, keys and signatures (RFC 4880 and RFC 9580).
 *
 * Packets: the old and the new header formats. Partial body lengths exist
 * only for data packets (literal, compressed, encrypted data); signatures and
 * keys never use them, so they are refused here, and so is the old format's
 * "indeterminate length".
 *
 * Keys (version 4 only): RSA (1, 3), EdDSA "legacy" with the Ed25519 curve
 * OID (22), Ed25519 (27). A key can sign data only when
 *   - a self-signature on a user ID (or a direct-key signature) checks, made
 *     with SHA-256, SHA-384 or SHA-512;
 *   - its key flags, if present, allow signing;
 *   - for a subkey: a subkey binding signature by the primary key checks, its
 *     key flags allow signing, and the binding carries a primary key binding
 *     signature ("back-signature", type 0x19) made by the subkey, which checks;
 *   - no revocation (0x20 for the key, 0x28 for a subkey) made by the primary
 *     key checks. Any such revocation counts, whatever its reason and date:
 *     that is stricter than RFC 9580, which lets a "superseded" or "retired"
 *     revocation keep older signatures valid;
 *   - it has not expired (key expiration time from the newest self-signature
 *     or binding) at the time given to the check.
 * Revocations by a designated revoker (another key, subpacket 12) cannot be
 * checked without that key and are ignored, as GnuPG does when it does not
 * hold the revoker's key.
 *
 * Signatures: version 4 only. Versions 3, 5 and 6 are parsed far enough to
 * name them and reported as unsupported. A subpacket marked critical that
 * this code does not know makes the signature invalid (RFC 9580 5.2.3.7). */
#include "sig_int.h"
#include <stdarg.h>

void sig_errf(char *err, size_t errlen, const char *msg) {
  if (!err || !errlen) return;
  size_t n = strlen(msg);
  if (n >= errlen) n = errlen - 1;
  memcpy(err, msg, n);
  err[n] = 0;
}
static void hexs(char *out, const uint8_t *p, int n) {
  static const char hx[] = "0123456789ABCDEF";
  for (int i = 0; i < n; i++) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
  out[2 * n] = 0;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void cpy(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}
/* A small formatter for reasons: "%s" and "%d" and "%x" (two hex digits) only. */
static void why(char *out, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  size_t o = 0;
  for (const char *f = fmt; *f && o + 1 < cap; f++) {
    if (*f != '%' || !f[1]) { out[o++] = *f; continue; }
    f++;
    char num[24];
    const char *s = num;
    if (*f == 's') s = va_arg(ap, const char *);
    else if (*f == 'd') {
      long v = va_arg(ap, int);
      char t[24]; int k = 0, neg = v < 0;
      unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
      do { t[k++] = '0' + u % 10; u /= 10; } while (u);
      int j = 0;
      if (neg) num[j++] = '-';
      while (k) num[j++] = t[--k];
      num[j] = 0;
    } else if (*f == 'x') {
      unsigned v = va_arg(ap, unsigned);
      static const char hx[] = "0123456789abcdef";
      num[0] = hx[v >> 4 & 15]; num[1] = hx[v & 15]; num[2] = 0;
    } else { num[0] = *f; num[1] = 0; }
    while (*s && o + 1 < cap) out[o++] = *s++;
  }
  out[o] = 0;
  va_end(ap);
}

/* ---------- hashes ---------- */
int sig_hash_len(int algo) { return algo == H_SHA256 ? 32 : algo == H_SHA384 ? 48 : algo == H_SHA512 ? 64 : 0; }
int sig_hash_init(sig_hash *h, int algo) {
  h->algo = algo;
  if (algo == H_SHA256) arc_sha256_init(&h->u.s256);
  else if (algo == H_SHA384) arc_sha384_init(&h->u.s512);
  else if (algo == H_SHA512) arc_sha512_init(&h->u.s512);
  else return -1;
  return 0;
}
void sig_hash_update(sig_hash *h, const void *p, size_t n) {
  if (h->algo == H_SHA256) arc_sha256_update(&h->u.s256, p, n);
  else arc_sha512_update(&h->u.s512, p, n);
}
int sig_hash_final(sig_hash *h, unsigned char *out) {
  if (h->algo == H_SHA256) arc_sha256_final(&h->u.s256, out);
  else if (h->algo == H_SHA384) arc_sha384_final(&h->u.s512, out);
  else arc_sha512_final(&h->u.s512, out);
  return sig_hash_len(h->algo);
}
static const char *hash_name(int a) {
  switch (a) {
  case H_MD5: return "MD5"; case H_SHA1: return "SHA-1"; case H_RIPEMD160: return "RIPEMD-160";
  case H_SHA256: return "SHA-256"; case H_SHA384: return "SHA-384"; case H_SHA512: return "SHA-512"; case H_SHA224: return "SHA-224";
  case 12: return "SHA3-256"; case 14: return "SHA3-512";
  default: return "an unknown hash";
  }
}

/* ---------- packets ---------- */
typedef struct { int tag; const uint8_t *p; size_t n; } pkt;
/* 1 = a packet, 0 = end of data, -1 = malformed (msg says why) */
static int next_packet(const uint8_t *d, size_t n, size_t *pos, pkt *out, const char **msg) {
  size_t i = *pos;
  if (i >= n) return 0;
  uint8_t c = d[i++];
  if (!(c & 0x80)) { *msg = "not an OpenPGP packet (bit 7 of the packet tag is clear)"; return -1; }
  size_t len;
  if (c & 0x40) {                               /* new format */
    out->tag = c & 0x3f;
    if (i >= n) { *msg = "truncated packet header"; return -1; }
    uint8_t o = d[i++];
    if (o < 192) len = o;
    else if (o < 224) {
      if (i >= n) { *msg = "truncated packet header"; return -1; }
      len = ((size_t)(o - 192) << 8) + d[i++] + 192;
    } else if (o == 255) {
      if (n - i < 4) { *msg = "truncated packet header"; return -1; }
      len = be32(d + i); i += 4;
    } else { *msg = "partial body length in a packet that may not have one"; return -1; }
  } else {                                      /* old format */
    out->tag = (c >> 2) & 0x0f;
    int lt = c & 3;
    if (lt == 3) { *msg = "indeterminate packet length"; return -1; }
    int nb = 1 << lt;
    if (n - i < (size_t)nb) { *msg = "truncated packet header"; return -1; }
    len = 0;
    for (int k = 0; k < nb; k++) len = len << 8 | d[i++];
  }
  if (len > n - i) { *msg = "packet longer than the data"; return -1; }
  out->p = d + i;
  out->n = len;
  *pos = i + len;
  return 1;
}
/* An MPI: two bytes of bit count, then the bytes. */
static int read_mpi(const uint8_t *p, size_t n, size_t *pos, const uint8_t **v, size_t *vl) {
  if (n - *pos < 2 || *pos > n) return -1;
  size_t bits = (size_t)p[*pos] << 8 | p[*pos + 1];
  size_t bytes = (bits + 7) / 8;
  *pos += 2;
  if (bytes > n - *pos) return -1;
  *v = p + *pos;
  *vl = bytes;
  *pos += bytes;
  return 0;
}

/* ---------- signatures ---------- */
typedef struct {
  int version, type, pkalgo, hashalgo;
  const uint8_t *hstart; size_t hlen;      /* version byte to end of the hashed subpackets */
  uint8_t left16[2];
  const uint8_t *m1, *m2; size_t m1l, m2l;
  long long created; int has_created;
  long long sigexp;                        /* seconds after creation, 0 = none */
  long long keyexp; int has_keyexp;
  int flags, has_flags, primary_uid;
  uint8_t issuer[8]; int has_issuer;
  uint8_t ifpr[32]; int ifpr_len;
  const uint8_t *emb; size_t embl;
  const char *invalid;                     /* structure makes it invalid (unknown critical subpacket, no creation time) */
} psig;

static int known_subpacket(int t) {
  switch (t) {
  case 2: case 3: case 4: case 5: case 7: case 9: case 11: case 12: case 16: case 21: case 22: case 23: case 24:
  case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 39:
    return 1;
  default: return 0;   /* 6 (regular expression) and 20 (notation) are not understood: critical ones invalidate */
  }
}
static int parse_subpackets(const uint8_t *p, size_t n, int hashed, psig *s) {
  size_t i = 0;
  while (i < n) {
    size_t len;
    uint8_t o = p[i++];
    if (o < 192) len = o;
    else if (o < 255) { if (i >= n) return -1; len = ((size_t)(o - 192) << 8) + p[i++] + 192; }
    else { if (n - i < 4) return -1; len = be32(p + i); i += 4; }
    if (len == 0 || len > n - i) return -1;
    int t = p[i] & 0x7f, crit = p[i] & 0x80;
    const uint8_t *v = p + i + 1;
    size_t vl = len - 1;
    i += len;
    if (crit && !known_subpacket(t)) { if (!s->invalid) s->invalid = "it has a critical subpacket this system does not understand"; continue; }
    if (t == 16 && vl == 8) { memcpy(s->issuer, v, 8); s->has_issuer = 1; continue; }
    if (t == 33 && vl >= 1 && (v[0] == 4 || v[0] == 6) && vl - 1 <= 32 && vl - 1 >= 20) {
      memcpy(s->ifpr, v + 1, vl - 1); s->ifpr_len = (int)(vl - 1); continue;
    }
    if (t == 32) { if (!s->emb) { s->emb = v; s->embl = vl; } continue; }
    if (!hashed) continue;                 /* nothing else counts unless it is signed */
    if (t == 2 && vl == 4) { if (!s->has_created) { s->created = be32(v); s->has_created = 1; } }
    else if (t == 3 && vl == 4) s->sigexp = be32(v);
    else if (t == 9 && vl == 4) { s->keyexp = be32(v); s->has_keyexp = 1; }
    else if (t == 27 && vl >= 1) { s->flags = v[0]; s->has_flags = 1; }
    else if (t == 25 && vl == 1) s->primary_uid = v[0] != 0;
    else if ((t == 2 || t == 3 || t == 9) && crit) { if (!s->invalid) s->invalid = "a critical time subpacket has the wrong size"; }
  }
  return 0;
}
/* 0 = a version 4 signature, 1 = another version (only version is set), -1 = malformed */
static int parse_sig(const uint8_t *p, size_t n, psig *s) {
  memset(s, 0, sizeof *s);
  if (n < 1) return -1;
  s->version = p[0];
  if (p[0] != 4) {
    if (p[0] == 3 && n >= 16) { s->type = p[2]; s->pkalgo = p[15]; s->hashalgo = n > 16 ? p[16] : 0; }
    else if ((p[0] == 5 || p[0] == 6) && n >= 4) { s->type = p[1]; s->pkalgo = p[2]; s->hashalgo = p[3]; }
    return 1;
  }
  if (n < 6) return -1;
  s->type = p[1]; s->pkalgo = p[2]; s->hashalgo = p[3];
  size_t hl = (size_t)p[4] << 8 | p[5];
  if (6 + hl + 2 > n) return -1;
  s->hstart = p;
  s->hlen = 6 + hl;
  if (parse_subpackets(p + 6, hl, 1, s) < 0) return -1;
  size_t i = 6 + hl;
  size_t ul = (size_t)p[i] << 8 | p[i + 1];
  i += 2;
  if (ul + 2 > n - i) return -1;
  if (parse_subpackets(p + i, ul, 0, s) < 0) return -1;
  i += ul;
  s->left16[0] = p[i]; s->left16[1] = p[i + 1];
  i += 2;
  if (s->pkalgo == PK_RSA || s->pkalgo == PK_RSA_S) {
    if (read_mpi(p, n, &i, &s->m1, &s->m1l) < 0) return -1;
  } else if (s->pkalgo == PK_EDDSA) {
    if (read_mpi(p, n, &i, &s->m1, &s->m1l) < 0 || read_mpi(p, n, &i, &s->m2, &s->m2l) < 0) return -1;
  } else if (s->pkalgo == PK_ED25519) {
    if (n - i < 64) return -1;
    s->m1 = p + i; s->m1l = 64; i += 64;
  } else i = n;                            /* another algorithm: its fields are not read */
  if (i != n) return -1;                   /* trailing bytes in the packet */
  if (!s->has_created && !s->invalid) s->invalid = "it has no signed creation time";
  return 0;
}

/* ---------- keys ---------- */
typedef struct {
  int is_sub, primary;
  int version, algo, bits;
  long long created;
  uint8_t *body; size_t bodylen;
  const uint8_t *rsa_n, *rsa_e; size_t rsa_nl, rsa_el;
  uint8_t ed[32];
  uint8_t fpr[20], keyid[8];
  char fprhex[41], idhex[17];
  char *uid;
  long long expires;          /* absolute; 0 never */
  long long bind_expires;     /* expiry of the self-signature or binding in use; 0 never */
  int revoked, can_sign, selected;
  const char *problem;
} key;
struct sig_keyring { key *k; int n, cap; };

sig_keyring *sig_keyring_new(void) { return calloc(1, sizeof(sig_keyring)); }
void sig_keyring_free(sig_keyring *kr) {
  if (!kr) return;
  for (int i = 0; i < kr->n; i++) { free(kr->k[i].body); free(kr->k[i].uid); }
  free(kr->k);
  free(kr);
}
int sig_keyring_count(const sig_keyring *kr) { return kr ? kr->n : 0; }

static int parse_key(key *k, const uint8_t *p, size_t n) {
  memset(k, 0, sizeof *k);
  k->body = malloc(n ? n : 1);
  if (!k->body) return -1;
  memcpy(k->body, p, n);
  k->bodylen = n;
  p = k->body;
  if (n < 6) { k->problem = "malformed key packet"; return 0; }
  k->version = p[0];
  if (p[0] != 4) { k->problem = p[0] == 6 ? "version 6 keys are not supported" : "only version 4 keys are supported"; return 0; }
  k->created = be32(p + 1);
  k->algo = p[5];
  /* v4 fingerprint: SHA-1 over 0x99, a two-byte length and the body */
  arc_sha1 h;
  uint8_t hdr[3] = {0x99, (uint8_t)(n >> 8), (uint8_t)n};
  arc_sha1_init(&h); arc_sha1_update(&h, hdr, 3); arc_sha1_update(&h, p, n); arc_sha1_final(&h, k->fpr);
  if (n > 0xffff) k->problem = "key packet too large";
  memcpy(k->keyid, k->fpr + 12, 8);
  hexs(k->fprhex, k->fpr, 20);
  hexs(k->idhex, k->keyid, 8);
  size_t i = 6;
  if (k->algo == PK_RSA || k->algo == PK_RSA_S) {
    if (read_mpi(p, n, &i, &k->rsa_n, &k->rsa_nl) < 0 || read_mpi(p, n, &i, &k->rsa_e, &k->rsa_el) < 0 || i != n) { k->problem = "malformed RSA key"; return 0; }
    while (k->rsa_nl && !*k->rsa_n) { k->rsa_n++; k->rsa_nl--; }
    int top = 0;
    if (k->rsa_nl) for (top = 8; top > 0 && !(k->rsa_n[0] >> (top - 1) & 1); top--) ;
    k->bits = k->rsa_nl ? (int)(8 * (k->rsa_nl - 1)) + top : 0;
    if (k->bits < 2048) k->problem = "RSA keys under 2048 bits are not accepted";
    else if (k->bits > SIG_MAX_MODULUS_BITS) k->problem = "RSA key larger than 8192 bits";
    else if (!(k->rsa_n[k->rsa_nl - 1] & 1)) k->problem = "malformed RSA key (even modulus)";
  } else if (k->algo == PK_EDDSA) {
    static const uint8_t oid[] = {0x2b, 0x06, 0x01, 0x04, 0x01, 0xda, 0x47, 0x0f, 0x01};  /* 1.3.6.1.4.1.11591.15.1 */
    const uint8_t *q; size_t ql;
    if (i >= n || p[i] == 0 || p[i] == 0xff || (size_t)p[i] > n - i - 1) { k->problem = "malformed EdDSA key"; return 0; }
    size_t olen = p[i];
    int ok_oid = olen == sizeof oid && !memcmp(p + i + 1, oid, sizeof oid);
    i += 1 + olen;
    if (read_mpi(p, n, &i, &q, &ql) < 0 || i != n) { k->problem = "malformed EdDSA key"; return 0; }
    if (!ok_oid) { k->problem = "EdDSA on a curve other than Ed25519 is not supported"; return 0; }
    if (ql != 33 || q[0] != 0x40) { k->problem = "malformed Ed25519 public key"; return 0; }
    memcpy(k->ed, q + 1, 32);
    k->bits = 255;
  } else if (k->algo == PK_ED25519) {
    if (n - i != 32) { k->problem = "malformed Ed25519 key"; return 0; }
    memcpy(k->ed, p + i, 32);
    k->bits = 255;
  } else {
    k->problem = "public key algorithm not supported";
    return 0;
  }
  if ((k->algo == PK_EDDSA || k->algo == PK_ED25519) && sig_ed25519_small_order(k->ed)) k->problem = "Ed25519 key is a point of small order";
  return 0;
}

/* The hash of what a signature covers, and its check against one key.
 * pieces: what comes before the signature's own trailer. */
typedef struct { const uint8_t *p; size_t n; } piece;
static int sig_digest(const psig *s, const piece *pc, int npc, uint8_t *out) {
  sig_hash h;
  if (sig_hash_init(&h, s->hashalgo) < 0) return -1;
  for (int i = 0; i < npc; i++) sig_hash_update(&h, pc[i].p, pc[i].n);
  sig_hash_update(&h, s->hstart, s->hlen);
  uint8_t tr[6] = {4, 0xff, (uint8_t)(s->hlen >> 24), (uint8_t)(s->hlen >> 16), (uint8_t)(s->hlen >> 8), (uint8_t)s->hlen};
  sig_hash_update(&h, tr, 6);
  return sig_hash_final(&h, out);
}
static int pk_family(int a) { return a == PK_RSA_S ? PK_RSA : a; }
/* 1 = checks, 0 = does not */
static int check_math(const psig *s, const key *k, const uint8_t *dig) {
  if (k->version != 4 || (k->problem && !k->rsa_nl && !k->bits)) return 0;
  if (pk_family(s->pkalgo) != pk_family(k->algo)) return 0;
  if (dig[0] != s->left16[0] || dig[1] != s->left16[1]) return 0;
  int dl = sig_hash_len(s->hashalgo);
  if (k->algo == PK_RSA || k->algo == PK_RSA_S) {
    if (!k->rsa_nl) return 0;
    return sig_rsa_verify(k->rsa_n, k->rsa_nl, k->rsa_e, k->rsa_el, s->m1, s->m1l, s->hashalgo, dig) == 1;
  }
  uint8_t rs[64];
  if (k->algo == PK_EDDSA) {
    /* the two halves of the native signature, as MPIs: leading zero bytes were dropped */
    if (s->m1l > 32 || s->m2l > 32) return 0;
    memset(rs, 0, 64);
    memcpy(rs + 32 - s->m1l, s->m1, s->m1l);
    memcpy(rs + 64 - s->m2l, s->m2, s->m2l);
  } else if (k->algo == PK_ED25519) {
    memcpy(rs, s->m1, 64);
  } else return 0;
  return sig_ed25519_verify(k->ed, dig, dl, rs) == 1;
}
static int issuer_matches(const psig *s, const key *k) {
  if (s->ifpr_len == 20) return !memcmp(s->ifpr, k->fpr, 20);
  if (s->ifpr_len) return 0;
  if (s->has_issuer) return !memcmp(s->issuer, k->keyid, 8);
  return 1;   /* no issuer named: try it */
}
static void key_piece(const key *k, uint8_t hdr[3], piece *pc) {
  hdr[0] = 0x99; hdr[1] = (uint8_t)(k->bodylen >> 8); hdr[2] = (uint8_t)k->bodylen;
  pc[0].p = hdr; pc[0].n = 3;
  pc[1].p = k->body; pc[1].n = k->bodylen;
}
/* Is s a correct self-signature (made by signer) over pieces? why_hash notes a refused hash. */
static int check_self(const psig *s, const key *signer, const piece *pc, int npc, int *weak) {
  if (s->version != 4 || !issuer_matches(s, signer)) return 0;
  if (!sig_hash_len(s->hashalgo)) { if (weak && (s->hashalgo == H_SHA1 || s->hashalgo == H_MD5)) *weak = 1; return 0; }
  if (s->invalid || s->created < signer->created) return 0;
  uint8_t dig[64];
  if (sig_digest(s, pc, npc, dig) < 0) return 0;
  return check_math(s, signer, dig);
}

/* One transferable public key, collected before it is judged. */
enum { CTX_DIRECT, CTX_UID, CTX_ATTR, CTX_SUB };
typedef struct { int ctx; int sub; const uint8_t *uid; size_t uidl; const uint8_t *p; size_t n; } tsig;
typedef struct { tsig *s; int n, cap; } tsigs;
static int tsig_add(tsigs *t, tsig x) {
  if (t->n == t->cap) {
    int c = t->cap ? 2 * t->cap : 16;
    tsig *v = realloc(t->s, c * sizeof *v);
    if (!v) return -1;
    t->s = v; t->cap = c;
  }
  t->s[t->n++] = x;
  return 0;
}
static int kr_push(sig_keyring *kr) {
  if (kr->n == kr->cap) {
    int c = kr->cap ? 2 * kr->cap : 8;
    key *v = realloc(kr->k, c * sizeof *v);
    if (!v) return -1;
    kr->k = v; kr->cap = c;
  }
  return kr->n++;
}
static long long abs_exp(long long created, long long secs) { return secs ? created + secs : 0; }

static void judge(sig_keyring *kr, int pi, tsigs *ts) {
  key *P = &kr->k[pi];
  if (P->problem && !P->bits) { /* unusable material: nothing can be checked */
    for (int i = pi + 1; i < kr->n; i++) if (!kr->k[i].problem) kr->k[i].problem = "its primary key cannot be used";
    return;
  }
  uint8_t ph[3];
  piece pc[4];
  key_piece(P, ph, pc);
  int weak = 0;
  /* user IDs: for each, the newest certification and the newest certification revocation */
  long long best_t = -1, dir_flags_t = -1, dir_exp_t = -1;
  psig best = {0}, dflags = {0}, dexp = {0};
  const uint8_t *best_uid = 0; size_t best_uidl = 0;
  int best_primary = 0;
  for (int i = 0; i < ts->n; i++) {
    tsig *t = &ts->s[i];
    psig s;
    if (parse_sig(t->p, t->n, &s) != 0) continue;
    if (t->ctx == CTX_DIRECT) {
      if (s.type != 0x1f && s.type != 0x20) continue;
      /* a direct-key signature that sets neither key flags nor an expiry
       * changes nothing here (Debian's keys carry five, for designated
       * revokers): not checking it saves an RSA verification each */
      if (s.type == 0x1f && !s.has_flags && !s.has_keyexp) continue;
      if (!check_self(&s, P, pc, 2, &weak)) continue;
      if (s.type == 0x20) { P->revoked = 1; continue; }
      if (s.has_flags && s.created > dir_flags_t) { dir_flags_t = s.created; dflags = s; }
      if (s.has_keyexp && s.created > dir_exp_t) { dir_exp_t = s.created; dexp = s; }
      continue;
    }
    if (t->ctx != CTX_UID || s.type < 0x10 || s.type > 0x13) continue;
    uint8_t uh[5] = {0xb4, (uint8_t)(t->uidl >> 24), (uint8_t)(t->uidl >> 16), (uint8_t)(t->uidl >> 8), (uint8_t)t->uidl};
    pc[2].p = uh; pc[2].n = 5; pc[3].p = t->uid; pc[3].n = t->uidl;
    if (!check_self(&s, P, pc, 4, &weak)) continue;
    /* revoked user ID? a valid 0x30 by the key on the same user ID, newer than this certification */
    int revoked_uid = 0;
    for (int j = 0; j < ts->n; j++) {
      tsig *r = &ts->s[j];
      psig rs;
      if (r->ctx != CTX_UID || r->uid != t->uid || parse_sig(r->p, r->n, &rs) != 0 || rs.type != 0x30) continue;
      if (rs.created >= s.created && check_self(&rs, P, pc, 4, 0)) revoked_uid = 1;
    }
    if (revoked_uid) continue;
    /* prefer a primary user ID, then the newest */
    if ((s.primary_uid && !best_primary) || ((s.primary_uid || !best_primary) && s.created > best_t)) {
      best_t = s.created; best = s; best_uid = t->uid; best_uidl = t->uidl; best_primary = s.primary_uid;
    }
  }
  if (best_t < 0 && dir_flags_t < 0 && dir_exp_t < 0) {
    if (!P->problem) P->problem = weak ? "its self-signatures use SHA-1 or MD5, which are not accepted" : "it has no valid self-signature";
  } else {
    int flags = best.has_flags ? best.flags : -1;
    long long kexp = best.has_keyexp ? best.keyexp : 0;
    long long bexp = best_t >= 0 ? abs_exp(best.created, best.sigexp) : 0;
    if (dir_flags_t > best_t) flags = dflags.flags;
    if (dir_exp_t > best_t) kexp = dexp.keyexp;
    P->can_sign = flags < 0 || (flags & 0x02);
    P->expires = abs_exp(P->created, kexp);
    P->bind_expires = bexp;
    if (best_uid) {
      P->uid = malloc(best_uidl + 1);
      if (P->uid) { memcpy(P->uid, best_uid, best_uidl); P->uid[best_uidl] = 0; for (size_t i = 0; i < best_uidl; i++) if (!P->uid[i]) P->uid[i] = '?'; }
    }
  }
  /* subkeys */
  for (int si = pi + 1; si < kr->n; si++) {
    key *S = &kr->k[si];
    if (S->problem && !S->bits) continue;
    uint8_t sh[3];
    key_piece(S, sh, pc + 2);
    long long bt = -1;
    psig bind = {0};
    int bind_backsig = 0, weak2 = 0;
    for (int i = 0; i < ts->n; i++) {
      tsig *t = &ts->s[i];
      psig s;
      if (t->ctx != CTX_SUB || t->sub != si || parse_sig(t->p, t->n, &s) != 0) continue;
      if (s.type != 0x18 && s.type != 0x28) continue;
      if (!check_self(&s, P, pc, 4, &weak2)) continue;
      if (s.type == 0x28) { S->revoked = 1; continue; }
      if (s.created <= bt) continue;
      bt = s.created;
      bind = s;
      bind_backsig = 0;
      if (s.has_flags && (s.flags & 0x02) && s.emb) {
        psig b;
        if (parse_sig(s.emb, s.embl, &b) == 0 && b.type == 0x19 && check_self(&b, S, pc, 4, 0)) bind_backsig = 1;
      }
    }
    if (bt < 0) { if (!S->problem) S->problem = weak2 ? "its binding signature uses SHA-1 or MD5, which are not accepted" : "it has no valid binding signature"; continue; }
    S->expires = abs_exp(S->created, bind.has_keyexp ? bind.keyexp : 0);
    S->bind_expires = abs_exp(bind.created, bind.sigexp);
    S->can_sign = bind.has_flags && (bind.flags & 0x02) && bind_backsig;
    if (bind.has_flags && (bind.flags & 0x02) && !bind_backsig && !S->problem) S->problem = "its binding has no valid back-signature (primary key binding signature)";
  }
}

int sig_keyring_add(sig_keyring *kr, const void *data, size_t n, char *err, size_t errlen) {
  const uint8_t *d = data;
  /* armored? one or more PGP PUBLIC KEY BLOCKs */
  size_t i = 0;
  while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) i++;
  if (n - i >= 5 && !memcmp(d + i, "-----", 5)) {
    size_t pos = 0;
    int total = 0, blocks = 0;
    for (;;) {
      unsigned char *bin;
      long bl = sig_armor_decode(data, n, &pos, "PGP PUBLIC KEY BLOCK", &bin, err, errlen);
      if (bl < 0) return -1;
      if (bl == 0) break;
      blocks++;
      int r = sig_keyring_add(kr, bin, (size_t)bl, err, errlen);
      free(bin);
      if (r < 0) return -1;
      total += r;
    }
    if (!blocks) { sig_errf(err, errlen, "no PGP PUBLIC KEY BLOCK in the armored file"); return -1; }
    return total;
  }
  if (n >= 12 && !memcmp(d + 8, "KBXf", 4)) { sig_errf(err, errlen, "this is a GnuPG keybox (.kbx), not an OpenPGP keyring; export it with gpg --export"); return -1; }
  size_t pos = 0;
  pkt p;
  const char *msg = 0;
  int added = 0, cur = -1, ctx = CTX_DIRECT, sub = -1, skipping = 0, any = 0;
  const uint8_t *uid = 0; size_t uidl = 0;
  tsigs ts = {0};
  int r;
  while ((r = next_packet(d, n, &pos, &p, &msg)) == 1) {
    any = 1;
    if (p.tag == 6 || p.tag == 5) {
      if (cur >= 0) judge(kr, cur, &ts);
      ts.n = 0;
      cur = -1; skipping = 0;
      if (p.tag == 5) { skipping = 1; continue; }   /* a secret key in a keyring: not used */
      int ki = kr_push(kr);
      if (ki < 0 || parse_key(&kr->k[ki], p.p, p.n) < 0) { free(ts.s); sig_errf(err, errlen, "out of memory"); return -1; }
      kr->k[ki].primary = ki;
      kr->k[ki].selected = 1;
      cur = ki; ctx = CTX_DIRECT; sub = -1; uid = 0; uidl = 0;
      added++;
      continue;
    }
    if (cur < 0 || skipping) continue;
    switch (p.tag) {
    case 13: ctx = CTX_UID; uid = p.p; uidl = p.n; break;
    case 17: ctx = CTX_ATTR; break;
    case 14: {
      int ki = kr_push(kr);
      if (ki < 0 || parse_key(&kr->k[ki], p.p, p.n) < 0) { free(ts.s); sig_errf(err, errlen, "out of memory"); return -1; }
      kr->k[ki].is_sub = 1; kr->k[ki].primary = cur; kr->k[ki].selected = 1;
      ctx = CTX_SUB; sub = ki;
      break;
    }
    case 2: {
      tsig t = {ctx, sub, uid, uidl, p.p, p.n};
      if (tsig_add(&ts, t) < 0) { free(ts.s); sig_errf(err, errlen, "out of memory"); return -1; }
      break;
    }
    case 7: skipping = 1; break;             /* secret subkey: drop the rest of this key */
    default: break;                          /* trust (12), marker (10), others: ignored */
    }
  }
  if (r < 0) {
    if (cur >= 0) judge(kr, cur, &ts);
    free(ts.s);
    sig_errf(err, errlen, msg);
    return -1;
  }
  if (cur >= 0) judge(kr, cur, &ts);
  free(ts.s);
  if (!any) { sig_errf(err, errlen, "empty keyring"); return -1; }
  return added;
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
int sig_keyring_select(sig_keyring *kr, const char *fprs) {
  for (int i = 0; i < kr->n; i++) kr->k[i].selected = 0;
  const char *p = fprs;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    if (!*p) break;
    uint8_t f[20];
    int nb = 0, ok = 1;
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != ',' && *q != '!') {
      int a = hexval(q[0]), b = q[1] ? hexval(q[1]) : -1;
      if (a < 0 || b < 0 || nb >= 20) { ok = 0; while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != ',') q++; break; }
      f[nb++] = (uint8_t)(a << 4 | b);
      q += 2;
    }
    int exact = *q == '!';
    if (exact) q++;
    if (ok && nb == 20)
      for (int i = 0; i < kr->n; i++) {
        if (memcmp(kr->k[i].fpr, f, 20)) continue;
        kr->k[i].selected = 1;
        if (!exact && !kr->k[i].is_sub)
          for (int j = i + 1; j < kr->n && kr->k[j].is_sub; j++) kr->k[j].selected = 1;
      }
    p = q;
  }
  int c = 0;
  for (int i = 0; i < kr->n; i++) c += kr->k[i].selected;
  return c;
}

int sig_key_info_get(const sig_keyring *kr, int i, sig_key_info *o) {
  if (!kr || i < 0 || i >= kr->n) return -1;
  const key *k = &kr->k[i];
  memset(o, 0, sizeof *o);
  o->is_sub = k->is_sub; o->primary = k->primary; o->version = k->version; o->algo = k->algo; o->bits = k->bits;
  memcpy(o->fpr, k->fprhex, 41); memcpy(o->keyid, k->idhex, 17);
  o->uid = kr->k[k->primary].uid ? kr->k[k->primary].uid : "";
  o->created = k->created; o->expires = k->expires; o->revoked = k->revoked; o->can_sign = k->can_sign; o->selected = k->selected;
  o->problem = k->problem;
  if (!o->problem && !k->can_sign) o->problem = k->is_sub ? "its binding does not allow signing" : "its key flags do not allow signing data";
  return 0;
}

/* ---------- checking data signatures ---------- */
const char *sig_status_name(int s) {
  switch (s) {
  case SIG_GOOD: return "GOODSIG"; case SIG_BAD: return "BADSIG"; case SIG_NOKEY: return "NO_PUBKEY";
  case SIG_EXPKEY: return "EXPKEYSIG"; case SIG_REVKEY: return "REVKEYSIG"; case SIG_EXPSIG: return "EXPSIG";
  case SIG_UNSUPPORTED: return "UNSUPPORTED"; case SIG_UNUSABLE: return "UNUSABLE";
  default: return "?";
  }
}
static int report_add(sig_report *r, sig_result *x) {
  sig_result *v = realloc(r->r, (r->n + 1) * sizeof *v);
  if (!v) return -1;
  r->r = v;
  r->r[r->n++] = *x;
  if (x->status == SIG_GOOD) r->ngood++;
  if (x->status == SIG_BAD) r->nbad++;
  return 0;
}
/* The hash of the signed data, once per hash algorithm for a whole block:
 * every signature then only adds its own trailer to a copy of the state. */
typedef struct { sig_hash h[3]; int have[3]; } data_hashes;
static int data_digest(data_hashes *dh, const psig *s, const piece *data, uint8_t *out) {
  int k = s->hashalgo == H_SHA256 ? 0 : s->hashalgo == H_SHA384 ? 1 : s->hashalgo == H_SHA512 ? 2 : -1;
  if (k < 0) return -1;
  if (!dh->have[k]) {
    sig_hash_init(&dh->h[k], s->hashalgo);
    sig_hash_update(&dh->h[k], data->p, data->n);
    dh->have[k] = 1;
  }
  sig_hash h = dh->h[k];
  sig_hash_update(&h, s->hstart, s->hlen);
  uint8_t tr[6] = {4, 0xff, (uint8_t)(s->hlen >> 24), (uint8_t)(s->hlen >> 16), (uint8_t)(s->hlen >> 8), (uint8_t)s->hlen};
  sig_hash_update(&h, tr, 6);
  return sig_hash_final(&h, out);
}
/* Check one signature packet over data (hashed as given). want_type: 0x00 or 0x01.
 * hashes: the Hash header mask of a cleartext message, or 0. Returns -1 for a
 * malformed packet. */
static int check_data_sig(const sig_keyring *kr, const uint8_t *body, size_t bn, const piece *data, data_hashes *dh, int want_type, unsigned hashes,
                          long long now, sig_result *res) {
  memset(res, 0, sizeof *res);
  psig s;
  int pr = parse_sig(body, bn, &s);
  if (pr < 0) return -1;
  res->version = s.version; res->type = s.type; res->pkalgo = s.pkalgo; res->hashalgo = s.hashalgo;
  if (pr == 1) { res->status = SIG_UNSUPPORTED; why(res->why, sizeof res->why, "version %d signatures are not supported", s.version); return 0; }
  res->created = s.created;
  if (s.ifpr_len == 20) { hexs(res->keyid, s.ifpr + 12, 8); }
  else if (s.has_issuer) hexs(res->keyid, s.issuer, 8);
  if (s.type != want_type) {
    res->status = SIG_UNSUPPORTED;
    why(res->why, sizeof res->why, "signature type 0x%x is not a %s", (unsigned)s.type, want_type ? "text signature (0x01)" : "binary document signature (0x00)");
    return 0;
  }
  if (!sig_hash_len(s.hashalgo)) {
    res->status = SIG_UNSUPPORTED;
    why(res->why, sizeof res->why, "it uses %s, which is not accepted (only SHA-256, SHA-384, SHA-512)", hash_name(s.hashalgo));
    return 0;
  }
  if (s.pkalgo != PK_RSA && s.pkalgo != PK_RSA_S && s.pkalgo != PK_EDDSA && s.pkalgo != PK_ED25519) {
    res->status = SIG_UNSUPPORTED;
    why(res->why, sizeof res->why, "public key algorithm %d is not supported", s.pkalgo);
    return 0;
  }
  /* find the key */
  int found = 0;
  uint8_t dig[64];
  int have_dig = 0;
  for (int i = 0; i < kr->n; i++) {
    const key *k = &kr->k[i];
    if (!k->selected || k->version != 4 || !issuer_matches(&s, k)) continue;
    if (!s.ifpr_len && !s.has_issuer && pk_family(k->algo) != pk_family(s.pkalgo)) continue;
    found = 1;
    if (!res->fpr[0]) { cpy(res->fpr, sizeof res->fpr, k->fprhex); cpy(res->primary_fpr, sizeof res->primary_fpr, kr->k[k->primary].fprhex); cpy(res->uid, sizeof res->uid, kr->k[k->primary].uid ? kr->k[k->primary].uid : ""); }
    if (s.invalid) continue;
    if (hashes && !(hashes & (1u << s.hashalgo))) continue;
    if (!have_dig) {
      data_digest(dh, &s, data, dig);
      have_dig = 1;
    }
    if (!check_math(&s, k, dig)) continue;
    /* it checks with this key: now is the key allowed to have made it? */
    const key *P = &kr->k[k->primary];
    cpy(res->fpr, sizeof res->fpr, k->fprhex);
    cpy(res->primary_fpr, sizeof res->primary_fpr, P->fprhex);
    cpy(res->uid, sizeof res->uid, P->uid ? P->uid : "");
    if (P->problem) { res->status = SIG_UNUSABLE; why(res->why, sizeof res->why, "the key cannot be used: %s", P->problem); return 0; }
    if (k->problem) { res->status = SIG_UNUSABLE; why(res->why, sizeof res->why, "the subkey cannot be used: %s", k->problem); return 0; }
    if (!k->can_sign) { res->status = SIG_UNUSABLE; cpy(res->why, sizeof res->why, k->is_sub ? "the subkey is not bound for signing" : "the key's flags do not allow signing data"); return 0; }
    if (P->revoked || k->revoked) { res->status = SIG_REVKEY; cpy(res->why, sizeof res->why, P->revoked ? "the key is revoked" : "the subkey is revoked"); return 0; }
    if ((P->expires && now >= P->expires) || (k->expires && now >= k->expires)) { res->status = SIG_EXPKEY; cpy(res->why, sizeof res->why, "the key has expired"); return 0; }
    if ((P->bind_expires && now >= P->bind_expires) || (k->bind_expires && now >= k->bind_expires)) { res->status = SIG_EXPKEY; cpy(res->why, sizeof res->why, "the key's self-signature or binding has expired"); return 0; }
    if (s.created < k->created) { res->status = SIG_BAD; cpy(res->why, sizeof res->why, "the signature is older than the key"); return 0; }
    if (s.created > now) { res->status = SIG_UNUSABLE; cpy(res->why, sizeof res->why, "the signature was made after the current time (is the clock right?)"); return 0; }
    if (s.sigexp && now >= s.created + s.sigexp) { res->status = SIG_EXPSIG; cpy(res->why, sizeof res->why, "the signature has expired"); return 0; }
    res->status = SIG_GOOD;
    cpy(res->why, sizeof res->why, "good signature");
    return 0;
  }
  if (!found) { res->status = SIG_NOKEY; cpy(res->why, sizeof res->why, "no key in the keyring has the issuer's key ID"); return 0; }
  res->status = SIG_BAD;
  if (s.invalid) why(res->why, sizeof res->why, "the signature is invalid: %s", s.invalid);
  else if (hashes && !(hashes & (1u << s.hashalgo))) why(res->why, sizeof res->why, "its hash, %s, is not named in the Hash header", hash_name(s.hashalgo));
  else cpy(res->why, sizeof res->why, "the signature does not match the signed data");
  return 0;
}

#define MAX_SIGS 32
/* sigs: binary signature packets. */
static int check_block(const sig_keyring *kr, const uint8_t *sigs, size_t sn, const piece *data, int want_type, unsigned hashes,
                       long long now, sig_report *out, char *err, size_t errlen) {
  size_t pos = 0;
  pkt p;
  const char *msg = 0;
  int r;
  data_hashes dh;
  memset(&dh, 0, sizeof dh);
  while ((r = next_packet(sigs, sn, &pos, &p, &msg)) == 1) {
    if (p.tag == 10) continue;   /* marker packet: ignored, as RFC 9580 says */
    if (p.tag != 2) { sig_errf(err, errlen, "the signature block contains a packet that is not a signature"); return -1; }
    if (out->n >= MAX_SIGS) { sig_errf(err, errlen, "too many signatures"); return -1; }
    sig_result x;
    if (check_data_sig(kr, p.p, p.n, data, &dh, want_type, hashes, now, &x) < 0) { sig_errf(err, errlen, "malformed signature packet"); return -1; }
    if (report_add(out, &x) < 0) { sig_errf(err, errlen, "out of memory"); return -1; }
  }
  if (r < 0) { sig_errf(err, errlen, msg); return -1; }
  if (!out->n) { sig_errf(err, errlen, "the signature block holds no signature"); return -1; }
  return 0;
}

void sig_report_free(sig_report *r) {
  free(r->text);
  free(r->r);
  memset(r, 0, sizeof *r);
}
int sig_report_trusted(const sig_report *r) { return r->ngood > 0 && r->nbad == 0; }

int sig_check_cleartext(const sig_keyring *kr, const void *data, size_t n, long long now, sig_report *out, char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  sig_clear c;
  if (sig_clear_split(data, n, &c, err, errlen) < 0) return -1;
  piece pc = {(const uint8_t *)c.canon, c.canonlen};
  int r = check_block(kr, c.sig, c.siglen, &pc, 0x01, c.hashes, now, out, err, errlen);
  if (r < 0) { sig_report_free(out); sig_clear_free(&c); return -1; }
  out->text = c.text; out->len = c.textlen;
  c.text = 0;
  sig_clear_free(&c);
  return 0;
}

int sig_check_detached(const sig_keyring *kr, const void *doc, size_t docn, const void *sig, size_t sign, long long now,
                       sig_report *out, char *err, size_t errlen) {
  memset(out, 0, sizeof *out);
  const uint8_t *s = sig;
  unsigned char *bin = 0;
  size_t bl = sign;
  size_t i = 0;
  while (i < sign && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
  if (sign - i >= 5 && !memcmp(s + i, "-----", 5)) {
    size_t pos = 0;
    long l = sig_armor_decode(sig, sign, &pos, "PGP SIGNATURE", &bin, err, errlen);
    if (l < 0) return -1;
    if (l == 0) { free(bin); sig_errf(err, errlen, "no PGP SIGNATURE block"); return -1; }
    /* nothing but white space may follow */
    for (size_t k = pos; k < sign; k++)
      if (s[k] != ' ' && s[k] != '\t' && s[k] != '\r' && s[k] != '\n') { free(bin); sig_errf(err, errlen, "text after the signature block"); return -1; }
    s = bin; bl = (size_t)l;
  }
  piece pc = {doc, docn};
  int r = check_block(kr, s, bl, &pc, 0x00, 0, now, out, err, errlen);
  free(bin);
  if (r < 0) { sig_report_free(out); return -1; }
  return 0;
}
