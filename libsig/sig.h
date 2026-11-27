// SPDX-License-Identifier: GPL-2.0-or-later
/* libsig: OpenPGP signature verification for UC.OS, written from RFC 4880,
 * RFC 9580 (OpenPGP), RFC 8017 (RSA, EMSA-PKCS1-v1_5), RFC 8032 (Ed25519) and
 * FIPS 180-4 (the hashes, in libarc).
 *
 * Verification only: no signing, no secret keys, no encryption.
 *
 * What it reads:
 *   - keyrings: binary or ASCII-armored transferable public keys, several per
 *     file (version 4 keys: RSA, EdDSA "legacy" on Ed25519, native Ed25519);
 *   - cleartext signed messages (apt's InRelease), with every signature in the
 *     block checked;
 *   - detached signatures over a binary document (Release + Release.gpg),
 *     binary or armored.
 * Signatures must be version 4 and use SHA-256, SHA-384 or SHA-512. MD5, SHA-1,
 * other hashes, other packet versions and other public key algorithms are
 * parsed and reported as unsupported, never accepted.
 *
 * Everything treats its input as hostile: every length is checked, nothing is
 * read twice from the input, and the cleartext parser refuses any input whose
 * signed region is not exactly one well-formed block (see clear.c).
 *
 * The library uses malloc, free, memcpy, memcmp, memset and libarc's hashes,
 * nothing else, so it builds for UC.OS and for the host. */
#ifndef SIG_H
#define SIG_H
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Keyrings */
typedef struct sig_keyring sig_keyring;
sig_keyring *sig_keyring_new(void);
void sig_keyring_free(sig_keyring *kr);
/* Add every key in data (binary packets, or one or more armored "PGP PUBLIC
 * KEY BLOCK"s). Each key's self-signatures, subkey bindings, back-signatures
 * and revocations are checked here. Returns the number of primary keys added
 * (keys that are present but unusable count too: see sig_key_info), or -1 with
 * a message in err when the data is not a keyring. */
int sig_keyring_add(sig_keyring *kr, const void *data, size_t n, char *err, size_t errlen);
/* Keep only the keys a list of fingerprints names (apt's Signed-By with
 * fingerprints): a primary fingerprint keeps that key and its subkeys, a
 * fingerprint followed by '!' keeps exactly that (sub)key. The list is
 * separated by spaces, tabs, newlines or commas. Returns how many keys are
 * still usable for lookup; names that match nothing are ignored. */
int sig_keyring_select(sig_keyring *kr, const char *fprs);

/* One entry per key and subkey, primary first, then its subkeys. */
typedef struct {
  int is_sub;               /* 0 primary, 1 subkey */
  int primary;              /* index of the primary key of this entry */
  int version, algo, bits;  /* algo: 1 RSA, 3 RSA sign-only, 22 EdDSA, 27 Ed25519, or another number */
  char fpr[41];             /* upper-case hex */
  char keyid[17];
  const char *uid;          /* primary: the user ID of the newest valid self-signature; "" if none */
  long long created;
  long long expires;        /* 0: never */
  int revoked;
  int can_sign;             /* key flags allow signing data (and, for a subkey, a valid back-signature) */
  int selected;             /* 0 when sig_keyring_select dropped it */
  const char *problem;      /* why the key cannot sign at all, or NULL. Expiry is not included: compare with now */
} sig_key_info;
int sig_keyring_count(const sig_keyring *kr);
int sig_key_info_get(const sig_keyring *kr, int i, sig_key_info *out);

/* ---------------------------------------------------------------------------
 * Verification */
enum {
  SIG_GOOD,        /* checks, by a key in the keyring that may sign now */
  SIG_BAD,         /* a key in the keyring was found and the signature does not check against it */
  SIG_NOKEY,       /* no key in the keyring has the issuer's key ID or fingerprint */
  SIG_EXPKEY,      /* checks, but the key (or its primary key) has expired */
  SIG_REVKEY,      /* checks, but the key (or its primary key) is revoked */
  SIG_EXPSIG,      /* checks, but the signature itself has expired */
  SIG_UNSUPPORTED, /* version, hash or public key algorithm not accepted (MD5 and SHA-1 land here) */
  SIG_UNUSABLE     /* key found but may not sign data: no valid self-signature or binding, no signing flag, too small */
};
typedef struct {
  int status;
  int version, type, pkalgo, hashalgo;
  long long created;
  char keyid[17];           /* issuer key ID from the signature, or "" */
  char fpr[41];             /* the key that verified it (or that was found), or "" */
  char primary_fpr[41];
  char uid[128];            /* the primary key's user ID (cut to fit), or "" */
  char why[120];            /* a short reason in plain words */
} sig_result;
typedef struct {
  /* cleartext only: the signed text, with dash escapes removed, trailing
   * spaces and tabs removed from every line (they are not signed), lines
   * ending in "\n" (the last one too). NUL-terminated; len excludes the NUL.
   * This is exactly what was signed, up to the line endings; a caller must
   * parse these bytes and no others. */
  char *text;
  size_t len;
  sig_result *r;
  int n;
  int ngood, nbad;          /* SIG_GOOD and SIG_BAD counts */
} sig_report;
/* Parse and check a cleartext signed message. Returns 0 when the message is
 * well formed (each signature's result is in out, even if none is good), or
 * -1 when it is not a valid cleartext signed message at all (err says why;
 * out is empty). now is the time to check expiry against (seconds since 1970). */
int sig_check_cleartext(const sig_keyring *kr, const void *data, size_t n, long long now, sig_report *out, char *err, size_t errlen);
/* The same for a document and its detached signature (binary or armored). */
int sig_check_detached(const sig_keyring *kr, const void *doc, size_t docn, const void *sig, size_t sign, long long now,
                       sig_report *out, char *err, size_t errlen);
/* The policy apt applies: at least one good signature and no bad one. */
int sig_report_trusted(const sig_report *r);
void sig_report_free(sig_report *r);
const char *sig_status_name(int status);   /* "GOODSIG", "BADSIG", "NO_PUBKEY", "EXPKEYSIG", ... as gpgv names them */

/* ---------------------------------------------------------------------------
 * The layers below, exported for tests and tools. */
/* ASCII armor (RFC 9580 section 6). Finds the first block of the given kind
 * ("PGP SIGNATURE", "PGP PUBLIC KEY BLOCK") at or after *pos and decodes it.
 * The CRC-24 line is optional; when present it must match. Returns the
 * decoded length (malloc'd in *out) and moves *pos past the END line, 0 when
 * no further block is found, -1 when a block is malformed. */
long sig_armor_decode(const char *text, size_t n, size_t *pos, const char *kind, unsigned char **out, char *err, size_t errlen);
unsigned sig_crc24(const void *p, size_t n);
/* Split a cleartext signed message into its parts without checking any
 * signature. canon is the text as hashed (lines joined with CR LF, no final
 * line ending), text is the form described for sig_report.text. hashes is a
 * bit mask of the hash algorithms named in "Hash:" headers (bit 1 << algo);
 * without a Hash header it holds MD5 alone, as RFC 4880 says (and GnuPG does),
 * so a version 4 signature over another hash does not verify. */
typedef struct {
  char *canon; size_t canonlen;
  char *text; size_t textlen;
  unsigned char *sig; size_t siglen;
  unsigned hashes;
} sig_clear;
int sig_clear_split(const void *data, size_t n, sig_clear *out, char *err, size_t errlen);
void sig_clear_free(sig_clear *c);
/* RSA (RFC 8017 8.2.2 with EMSA-PKCS1-v1_5): 1 good, 0 bad, -1 unusable key.
 * digest is the hash value; hashalgo the OpenPGP number (8, 9, 10). */
int sig_rsa_verify(const unsigned char *n, size_t nlen, const unsigned char *e, size_t elen,
                   const unsigned char *s, size_t slen, int hashalgo, const unsigned char *digest);
/* Ed25519 (RFC 8032 5.1.7): 1 good, 0 bad (also for a malformed key or signature). */
int sig_ed25519_verify(const unsigned char pub[32], const unsigned char *msg, size_t msglen, const unsigned char sig[64]);
/* 1 when the encoded point is a valid point of small order (8 or less). */
int sig_ed25519_small_order(const unsigned char pub[32]);
/* Modular exponentiation (out = b^e mod m, all big-endian, out has mlen
 * bytes). m must be odd. Returns 0, or -1 when b >= m or the sizes are out of
 * range. sig_modexp_slow does the same with a plain shift-and-subtract
 * reduction, as a second implementation for differential tests. */
int sig_modexp(const unsigned char *b, size_t blen, const unsigned char *e, size_t elen,
               const unsigned char *m, size_t mlen, unsigned char *out);
int sig_modexp_slow(const unsigned char *b, size_t blen, const unsigned char *e, size_t elen,
                    const unsigned char *m, size_t mlen, unsigned char *out);
#define SIG_MAX_MODULUS_BITS 8192

#endif
