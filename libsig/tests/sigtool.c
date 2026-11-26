// SPDX-License-Identifier: GPL-2.0-or-later
/* sigtool: drives libsig from the command line for the tests. Host only.
 *   sigtool keys KEYRING...                         list keys
 *   sigtool clear [-t TIME] [-s FPRS] FILE KEYRING...  check a cleartext signed file
 *   sigtool detached [-t TIME] DOC SIG KEYRING...    check a detached signature
 *   sigtool text FILE                               print the signed text of a cleartext file (no check)
 *   sigtool batch < LINES                           one test per line, one answer per line:
 *       rsa N E S HASHALGO DIGEST   -> 1 (good), 0 (bad), -1 (key refused)
 *       ed PUB MSG SIG              -> 1 or 0
 *       small PUB                   -> 1 if the point has small order
 *       modexp B E M / slowexp B E M -> the result in hex, or "err"
 *       sha1|sha256|sha384|sha512 DATA -> the digest; crc24 DATA -> six hex digits
 *     every value in hex; "-" is the empty string.
 *   sigtool bench KEYRING FILE N                    time N cleartext checks
 *   sigtool prim N                                  time one RSA-4096 (e = 65537) and one Ed25519
 *                                                   verification: N batches of 10, first and best batch
 * Exit status for clear/detached: 0 when trusted (one good, no bad), 1 when
 * not, 2 when the file is malformed. */
#include "../sig.h"
#include "../../libarc/arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *readall(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  size_t cap = 1 << 16, len = 0;
  unsigned char *b = malloc(cap);
  size_t r;
  while ((r = fread(b + len, 1, cap - len, f)) > 0) { len += r; if (len == cap) b = realloc(b, cap *= 2); }
  fclose(f);
  *n = len;
  return b;
}
static sig_keyring *load(char **files, int nf) {
  sig_keyring *kr = sig_keyring_new();
  for (int i = 0; i < nf; i++) {
    size_t n;
    unsigned char *d = readall(files[i], &n);
    char err[200];
    if (sig_keyring_add(kr, d, n, err, sizeof err) < 0) { fprintf(stderr, "%s: %s\n", files[i], err); free(d); sig_keyring_free(kr); exit(2); }
    free(d);
  }
  return kr;
}
static void show(const sig_report *r) {
  for (int i = 0; i < r->n; i++) {
    const sig_result *x = &r->r[i];
    printf("%s %s algo=%d hash=%d fpr=%s uid=\"%s\": %s\n", sig_status_name(x->status), x->keyid[0] ? x->keyid : "-", x->pkalgo, x->hashalgo,
           x->fpr[0] ? x->fpr : "-", x->uid, x->why);
  }
  printf("%s (%d good, %d bad)\n", sig_report_trusted(r) ? "TRUSTED" : "NOT TRUSTED", r->ngood, r->nbad);
}
static int hexdec(const char *s, unsigned char **out) {
  if (!strcmp(s, "-")) { *out = malloc(1); return 0; }
  size_t l = strlen(s);
  *out = malloc(l / 2 + 1);
  for (size_t i = 0; i + 1 < l; i += 2) { unsigned v; sscanf(s + i, "%2x", &v); (*out)[i / 2] = (unsigned char)v; }
  return (int)(l / 2);
}
static void hexprint(const unsigned char *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }

static int batch(void) {
  static char line[1 << 20];
  while (fgets(line, sizeof line, stdin)) {
    char *w[8]; int nw = 0;
    for (char *t = strtok(line, " \t\n"); t && nw < 8; t = strtok(0, " \t\n")) w[nw++] = t;
    if (!nw) continue;
    unsigned char *a[6] = {0}; int al[6] = {0};
    for (int i = 1; i < nw && i <= 6; i++) if (strcmp(w[0], "rsa") || i != 4) al[i - 1] = hexdec(w[i], &a[i - 1]);
    if (!strcmp(w[0], "rsa") && nw == 6) {
      printf("%d\n", sig_rsa_verify(a[0], al[0], a[1], al[1], a[2], al[2], atoi(w[4]), a[4]));
    } else if (!strcmp(w[0], "ed") && nw == 4) {
      printf("%d\n", al[0] == 32 && al[2] == 64 ? sig_ed25519_verify(a[0], a[1], al[1], a[2]) : 0);
    } else if (!strcmp(w[0], "small") && nw == 2) {
      printf("%d\n", al[0] == 32 ? sig_ed25519_small_order(a[0]) : 0);
    } else if ((!strcmp(w[0], "modexp") || !strcmp(w[0], "slowexp")) && nw == 4) {
      unsigned char *o = malloc(al[2] + 1);
      int r = (!strcmp(w[0], "modexp") ? sig_modexp : sig_modexp_slow)(a[0], al[0], a[1], al[1], a[2], al[2], o);
      if (r < 0) printf("err\n"); else hexprint(o, al[2]);
      free(o);
    } else if (!strcmp(w[0], "sha1") && nw == 2) {
      arc_sha1 c; unsigned char d[20]; arc_sha1_init(&c); arc_sha1_update(&c, a[0], al[0]); arc_sha1_final(&c, d); hexprint(d, 20);
    } else if (!strcmp(w[0], "sha256") && nw == 2) {
      arc_sha256 c; unsigned char d[32]; arc_sha256_init(&c); arc_sha256_update(&c, a[0], al[0]); arc_sha256_final(&c, d); hexprint(d, 32);
    } else if (!strcmp(w[0], "sha384") && nw == 2) {
      arc_sha512 c; unsigned char d[48]; arc_sha384_init(&c); arc_sha512_update(&c, a[0], al[0]); arc_sha384_final(&c, d); hexprint(d, 48);
    } else if (!strcmp(w[0], "sha512") && nw == 2) {
      arc_sha512 c; unsigned char d[64]; arc_sha512_init(&c); arc_sha512_update(&c, a[0], al[0]); arc_sha512_final(&c, d); hexprint(d, 64);
    } else if (!strcmp(w[0], "crc24") && nw == 2) {
      printf("%06x\n", sig_crc24(a[0], al[0]));
    } else printf("?\n");
    for (int i = 0; i < 6; i++) free(a[i]);
    fflush(stdout);
  }
  return 0;
}

static double ms_since(struct timespec *t0) {
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (t1.tv_sec - t0->tv_sec) * 1e3 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}
/* RFC 8032 section 7.1, test 1 */
static const char ed_pub[] = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char ed_sig[] = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
static volatile int sink;
__attribute__((noinline)) static void batch_rsa(const unsigned char *n, const unsigned char *s, const unsigned char *d) {
  static const unsigned char e[3] = {1, 0, 1};
  for (int i = 0; i < 10; i++) sink += sig_rsa_verify(n, 512, e, 3, s, 512, 10, d);
}
__attribute__((noinline)) static void batch_ed(const unsigned char *pub, const unsigned char *sig) {
  for (int i = 0; i < 10; i++) sink += sig_ed25519_verify(pub, (const unsigned char *)"", 0, sig);
}
static void prim(int N) {
  unsigned char n[512], s[512], d[64], *pub, *sig;
  for (int i = 0; i < 512; i++) { n[i] = (unsigned char)(0xff - (i * 37 & 0x3f)); s[i] = (unsigned char)(i * 101 + 7); }
  n[511] |= 1; s[0] = 0x12;
  memset(d, 0x5a, 64);
  hexdec(ed_pub, &pub); hexdec(ed_sig, &sig);
  if (sig_ed25519_verify(pub, (const unsigned char *)"", 0, sig) != 1) { printf("Ed25519 self-check failed\n"); exit(1); }
  for (int k = 0; k < 2; k++) {
    double first = 0, best = 1e9;
    for (int b = 0; b < N; b++) {
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (k == 0) batch_rsa(n, s, d); else batch_ed(pub, sig);
      double ms = ms_since(&t0) / 10;
      if (b == 0) first = ms;
      if (ms < best) best = ms;
    }
    printf("%s: first batch %.3f ms per verification, best batch %.3f ms (%d batches of 10)\n", k ? "Ed25519" : "RSA-4096 e=65537 SHA-512", first, best, N);
  }
  free(pub); free(sig);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: see the comment at the top of sigtool.c\n"); return 2; }
  const char *cmd = argv[1];
  argv += 2; argc -= 2;
  long long now = time(0);
  const char *sel = 0;
  while (argc >= 2 && argv[0][0] == '-') {
    if (!strcmp(argv[0], "-t")) now = atoll(argv[1]);
    else if (!strcmp(argv[0], "-s")) sel = argv[1];
    else break;
    argv += 2; argc -= 2;
  }
  if (!strcmp(cmd, "batch")) return batch();
  if (!strcmp(cmd, "prim") && argc == 1) { prim(atoi(argv[0])); return 0; }
  if (!strcmp(cmd, "keys")) {
    sig_keyring *kr = load(argv, argc);
    for (int i = 0; i < sig_keyring_count(kr); i++) {
      sig_key_info k;
      sig_key_info_get(kr, i, &k);
      printf("%s %s algo=%d bits=%d created=%lld expires=%lld revoked=%d can_sign=%d uid=\"%s\"%s%s\n", k.is_sub ? "sub" : "pub", k.fpr, k.algo, k.bits,
             k.created, k.expires, k.revoked, k.can_sign, k.is_sub ? "" : k.uid, k.problem ? " problem: " : "", k.problem ? k.problem : "");
    }
    sig_keyring_free(kr);
    return 0;
  }
  if (!strcmp(cmd, "text") && argc == 1) {
    size_t n; unsigned char *d = readall(argv[0], &n);
    sig_clear c; char err[200];
    if (sig_clear_split(d, n, &c, err, sizeof err) < 0) { fprintf(stderr, "%s\n", err); free(d); return 2; }
    fwrite(c.text, 1, c.textlen, stdout);
    sig_clear_free(&c); free(d);
    return 0;
  }
  if (!strcmp(cmd, "clear") && argc >= 1) {
    size_t n; unsigned char *d = readall(argv[0], &n);
    sig_keyring *kr = load(argv + 1, argc - 1);
    if (sel) sig_keyring_select(kr, sel);
    sig_report r; char err[200];
    if (sig_check_cleartext(kr, d, n, now, &r, err, sizeof err) < 0) { printf("MALFORMED: %s\n", err); sig_keyring_free(kr); free(d); return 2; }
    show(&r);
    int t = sig_report_trusted(&r);
    sig_report_free(&r); sig_keyring_free(kr); free(d);
    return t ? 0 : 1;
  }
  if (!strcmp(cmd, "detached") && argc >= 2) {
    size_t n, sn; unsigned char *d = readall(argv[0], &n), *s = readall(argv[1], &sn);
    sig_keyring *kr = load(argv + 2, argc - 2);
    if (sel) sig_keyring_select(kr, sel);
    sig_report r; char err[200];
    if (sig_check_detached(kr, d, n, s, sn, now, &r, err, sizeof err) < 0) { printf("MALFORMED: %s\n", err); sig_keyring_free(kr); free(d); free(s); return 2; }
    show(&r);
    int t = sig_report_trusted(&r);
    sig_report_free(&r); sig_keyring_free(kr); free(d); free(s);
    return t ? 0 : 1;
  }
  if (!strcmp(cmd, "bench") && argc == 3) {
    struct timespec tk;
    clock_gettime(CLOCK_MONOTONIC, &tk);
    sig_keyring *kr = load(argv, 1);
    printf("keyring load (every self-signature, binding and back-signature checked): %.3f ms, %d keys and subkeys\n", ms_since(&tk), sig_keyring_count(kr));
    size_t n; unsigned char *d = readall(argv[1], &n);
    int N = atoi(argv[2]);
    struct timespec t0, t1;
    double best = 1e9;
    for (int i = 0; i < N; i++) {
      clock_gettime(CLOCK_MONOTONIC, &t0);
      sig_report r; char err[200];
      if (sig_check_cleartext(kr, d, n, now, &r, err, sizeof err) < 0) { printf("MALFORMED: %s\n", err); return 2; }
      sig_report_free(&r);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
      if (i == 0) printf("first %.3f ms\n", ms);
      if (ms < best) best = ms;
    }
    printf("best %.3f ms of %d\n", best, N);
    return 0;
  }
  fprintf(stderr, "sigtool: bad command line\n");
  return 2;
}
