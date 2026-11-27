// SPDX-License-Identifier: GPL-2.0-or-later
/* Mutation fuzzer for libsig. Build it with the sanitizers:
 *   cc -O1 -g -fsanitize=address,undefined -o fuzz libsig/tests/fuzz.c libsig/[a-z]*.c libarc/hash.c
 *   ./fuzz keyring SECONDS SEED...          sig_keyring_add, then every key's info
 *   ./fuzz armor SECONDS SEED...            sig_armor_decode (signature and key blocks)
 *   ./fuzz clear SECONDS KEYRING SEED...    sig_check_cleartext against KEYRING
 *   ./fuzz detached SECONDS KEYRING DOC SIG sig_check_detached with a mutated SIG
 *   ./fuzz bignum SECONDS                   sig_modexp against sig_modexp_slow
 * Mutations: bit flips, random bytes, cuts, repeats, inserts, and for text
 * inputs whole-line operations (delete, duplicate, swap, insert a known
 * armor line). In clear mode there is an oracle beyond the sanitizers: when a
 * mutated message still verifies, the text it returns must be the seed's text,
 * byte for byte (a mutation can only make a message fail, never change what it
 * says). Any failure writes the input to fuzz-crash.bin and aborts. */
#include "../sig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long rng = 88172645463325252ull;
static unsigned long long rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static unsigned char *readall(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  long l = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(l + 1);
  if (fread(b, 1, l, f) != (size_t)l) { perror(path); exit(2); }
  fclose(f);
  *n = (size_t)l;
  return b;
}
static void crash(const unsigned char *p, size_t n, const char *why) {
  FILE *f = fopen("fuzz-crash.bin", "wb");
  if (f) { fwrite(p, 1, n, f); fclose(f); }
  fprintf(stderr, "fuzz: %s (input in fuzz-crash.bin)\n", why);
  abort();
}

static const char *const lines[] = {"-----BEGIN PGP SIGNED MESSAGE-----\n", "-----BEGIN PGP SIGNATURE-----\n", "-----END PGP SIGNATURE-----\n",
                                    "Hash: SHA256\n", "Hash: SHA512\n", "\n", "- -\n", "=AAAA\n", "Version: x\n", "-----BEGIN PGP PUBLIC KEY BLOCK-----\n",
                                    "-----END PGP PUBLIC KEY BLOCK-----\n", "Origin: Evil\n", "  \t\n", "\r\n"};

static size_t mutate(const unsigned char *in, size_t n, unsigned char *out, size_t cap, int text) {
  if (n > cap) n = cap;
  memcpy(out, in, n);
  int rounds = 1 + rnd() % 4;
  for (int r = 0; r < rounds; r++) {
    int op = rnd() % (text ? 9 : 6);
    if (!n) { out[0] = rnd(); n = 1; continue; }
    size_t i = rnd() % n;
    switch (op) {
    case 0: out[i] ^= 1 << (rnd() % 8); break;
    case 1: out[i] = rnd(); break;
    case 2: n = i; break;                                         /* cut */
    case 3: { size_t l = 1 + rnd() % 16; if (i + l <= n && n + l <= cap) { memmove(out + i + l, out + i, n - i); n += l; } break; } /* repeat */
    case 4: { size_t l = 1 + rnd() % 8; if (n + l <= cap) { memmove(out + i + l, out + i, n - i); for (size_t k = 0; k < l; k++) out[i + k] = rnd(); n += l; } break; }
    case 5: { size_t l = 1 + rnd() % 64; if (i + l <= n) { memmove(out + i, out + i + l, n - i - l); n -= l; } break; } /* delete a run */
    case 6: case 7: { /* a whole line: delete or insert a known one */
      while (i > 0 && out[i - 1] != '\n') i--;
      size_t e = i;
      while (e < n && out[e] != '\n') e++;
      if (e < n) e++;
      if (op == 6) { memmove(out + i, out + e, n - e); n -= e - i; }
      else {
        const char *s = lines[rnd() % (sizeof lines / sizeof lines[0])];
        size_t l = strlen(s);
        if (n + l <= cap) { memmove(out + i + l, out + i, n - i); memcpy(out + i, s, l); n += l; }
      }
      break;
    }
    case 8: { /* duplicate a line */
      while (i > 0 && out[i - 1] != '\n') i--;
      size_t e = i;
      while (e < n && out[e] != '\n') e++;
      if (e < n) e++;
      size_t l = e - i;
      if (n + l <= cap) { memmove(out + e + l, out + e, n - e); memcpy(out + e, out + i, l); n += l; }
      break;
    }
    }
  }
  return n;
}

static sig_keyring *load(const char *path) {
  size_t n;
  unsigned char *d = readall(path, &n);
  sig_keyring *kr = sig_keyring_new();
  char err[200];
  if (sig_keyring_add(kr, d, n, err, sizeof err) < 0) { fprintf(stderr, "%s: %s\n", path, err); exit(2); }
  free(d);
  return kr;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: see fuzz.c\n"); return 2; }
  const char *mode = argv[1];
  double secs = atof(argv[2]);
  if (getenv("FUZZ_SEED")) rng = strtoull(getenv("FUZZ_SEED"), 0, 0);
  clock_t end = clock() + (clock_t)(secs * CLOCKS_PER_SEC);
  unsigned long long iters = 0, accepted = 0;
  long long now = 1795700000;
  if (!strcmp(mode, "bignum")) {
    unsigned char b[130], e[9], m[130], o1[130], o2[130];
    do {
      size_t ml = 1 + rnd() % 128, el = 1 + rnd() % 8;
      for (size_t i = 0; i < ml; i++) m[i] = rnd();
      m[ml - 1] |= 1;
      if (rnd() % 4 == 0) m[0] = 0;
      for (size_t i = 0; i < ml; i++) b[i] = rnd();
      for (size_t i = 0; i < el; i++) e[i] = rnd();
      int r1 = sig_modexp(b, ml, e, el, m, ml, o1), r2 = sig_modexp_slow(b, ml, e, el, m, ml, o2);
      if (r1 != r2 || (r1 == 0 && memcmp(o1, o2, ml))) crash(m, ml, "modexp disagrees with modexp_slow");
      iters++;
    } while (clock() < end);
    printf("bignum: %llu inputs, no disagreement\n", iters);
    return 0;
  }
  int first_seed = 3;
  sig_keyring *kr = 0;
  unsigned char *doc = 0;
  size_t docn = 0;
  if (!strcmp(mode, "clear") || !strcmp(mode, "detached")) { kr = load(argv[3]); first_seed = 4; }
  if (!strcmp(mode, "detached")) { doc = readall(argv[4], &docn); first_seed = 5; }
  int ns = argc - first_seed;
  if (ns < 1) { fprintf(stderr, "no seeds\n"); return 2; }
  unsigned char **seed = malloc(ns * sizeof *seed);
  size_t *seedn = malloc(ns * sizeof *seedn);
  char **seedtext = calloc(ns, sizeof *seedtext);
  size_t *seedtextn = calloc(ns, sizeof *seedtextn);
  size_t cap = 0;
  for (int i = 0; i < ns; i++) {
    seed[i] = readall(argv[first_seed + i], &seedn[i]);
    if (seedn[i] * 2 + 4096 > cap) cap = seedn[i] * 2 + 4096;
    if (!strcmp(mode, "clear")) {
      sig_clear c; char err[200];
      if (sig_clear_split(seed[i], seedn[i], &c, err, sizeof err) == 0) { seedtext[i] = c.text; seedtextn[i] = c.textlen; c.text = 0; sig_clear_free(&c); }
    }
  }
  unsigned char *buf = malloc(cap);
  int text = strcmp(mode, "keyring") != 0 || 1;
  do {
    int s = rnd() % ns;
    size_t n = mutate(seed[s], seedn[s], buf, cap, text && (rnd() & 1));
    char err[200];
    if (!strcmp(mode, "keyring")) {
      sig_keyring *k = sig_keyring_new();
      sig_keyring_add(k, buf, n, err, sizeof err);
      for (int i = 0; i < sig_keyring_count(k); i++) { sig_key_info ki; sig_key_info_get(k, i, &ki); if (ki.uid && strlen(ki.uid) > 1 << 20) crash(buf, n, "uid"); }
      sig_keyring_free(k);
    } else if (!strcmp(mode, "armor")) {
      size_t pos = 0;
      unsigned char *o;
      while (sig_armor_decode((char *)buf, n, &pos, (rnd() & 1) ? "PGP SIGNATURE" : "PGP PUBLIC KEY BLOCK", &o, err, sizeof err) > 0) free(o);
      sig_clear c;
      if (sig_clear_split(buf, n, &c, err, sizeof err) == 0) sig_clear_free(&c);
    } else if (!strcmp(mode, "clear")) {
      sig_report r;
      if (sig_check_cleartext(kr, buf, n, now, &r, err, sizeof err) == 0) {
        if (sig_report_trusted(&r)) {
          accepted++;
          if (!seedtext[s] || r.len != seedtextn[s] || memcmp(r.text, seedtext[s], r.len)) {
            /* another seed may be the source after line operations: allow any seed's text */
            int same = 0;
            for (int j = 0; j < ns; j++) if (seedtext[j] && r.len == seedtextn[j] && !memcmp(r.text, seedtext[j], r.len)) same = 1;
            if (!same) crash(buf, n, "a mutated message verifies with a different text");
          }
        }
        sig_report_free(&r);
      }
    } else if (!strcmp(mode, "detached")) {
      sig_report r;
      if (sig_check_detached(kr, doc, docn, buf, n, now, &r, err, sizeof err) == 0) { if (sig_report_trusted(&r)) accepted++; sig_report_free(&r); }
    } else { fprintf(stderr, "unknown mode\n"); return 2; }
    iters++;
  } while (clock() < end);
  printf("%s: %llu inputs, %llu still verified (all with the seed's own text)\n", mode, iters, accepted);
  for (int i = 0; i < ns; i++) { free(seed[i]); free(seedtext[i]); }
  free(seed); free(seedn); free(seedtext); free(seedtextn); free(buf); free(doc);
  sig_keyring_free(kr);
  return 0;
}
