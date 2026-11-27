// SPDX-License-Identifier: GPL-2.0-or-later
/* libsig internals shared by its files. */
#ifndef SIG_INT_H
#define SIG_INT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sig.h"
#include "../libarc/arc.h"

/* OpenPGP algorithm numbers (RFC 9580 9.1 and 9.5) */
enum { PK_RSA = 1, PK_RSA_S = 3, PK_EDDSA = 22, PK_ED25519 = 27 };
enum { H_MD5 = 1, H_SHA1 = 2, H_RIPEMD160 = 3, H_SHA256 = 8, H_SHA384 = 9, H_SHA512 = 10, H_SHA224 = 11 };

/* A running hash of one of the accepted algorithms. */
typedef struct { int algo; union { arc_sha256 s256; arc_sha512 s512; } u; } sig_hash;
int sig_hash_init(sig_hash *h, int algo);         /* 0, or -1 for a hash this code does not accept */
void sig_hash_update(sig_hash *h, const void *p, size_t n);
int sig_hash_final(sig_hash *h, unsigned char *out);  /* returns the digest length */
int sig_hash_len(int algo);                       /* 0 if not accepted */

void sig_errf(char *err, size_t errlen, const char *msg);
#endif
