// SPDX-License-Identifier: GPL-2.0-or-later
/* Trusted keys for apt, as sources.list(5) and apt-secure(8) describe them,
 * and the apt-keyring tool that lists and checks them.
 *
 * Which keys a source trusts:
 *   - Signed-By (deb822) or [signed-by=...] (one-line): a list, separated by
 *     commas or white space, of keyring files and of fingerprints. Files give
 *     the keys; fingerprints then select among them (or, with no file in the
 *     list, among the default keyrings). A primary key's fingerprint also
 *     selects its subkeys; "FPR!" selects exactly that key. In a .sources file
 *     the value may also be an armored key block written in the field itself
 *     (continuation lines, " ." for an empty line).
 *   - Otherwise the default keyrings: Dir::Etc::Trusted ("trusted.gpg", if it
 *     exists) and every *.gpg and *.asc file in Dir::Etc::TrustedParts
 *     ("trusted.gpg.d/"). Other names there are ignored, with a note.
 * File names in Signed-By are taken under apt's Dir, like every other path of
 * the package tools (apt itself uses them as given): with the image's root
 * /debian, signed-by=/usr/share/keyrings/x.pgp reads /debian/usr/share/keyrings/x.pgp.
 * Keyrings are binary OpenPGP or ASCII-armored; GnuPG's keybox (.kbx) is not
 * read (apt does not read it either). */
#include "pkg.h"
#include "../libsig/sig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>

static int is_fpr(const char *s) {
  size_t n = strlen(s);
  if (n && s[n - 1] == '!') n--;
  if (n != 40) return 0;
  for (size_t i = 0; i < n; i++) if (!strchr("0123456789abcdefABCDEF", s[i])) return 0;
  return 1;
}
static int add_file(sig_keyring *kr, const char *path, sbuf *err, strlist *used) {
  char *data;
  size_t n;
  if (file_read(path, &data, &n) < 0) { sb_printf(err, "cannot read the keyring %s", path); return -1; }
  char e[200];
  int r = sig_keyring_add(kr, data, n, e, sizeof e);
  free(data);
  if (r < 0) { sb_printf(err, "%s: %s", path, e); return -1; }
  if (used) sl_add(used, path);
  return 0;
}
/* Names apt ignores without a note (its Dir::Ignore-Files-Silently defaults). */
static int quiet_name(const char *n) {
  static const char *const suf[] = {"~", ".disabled", ".bak", ".save", ".orig", ".dpkg-old", ".dpkg-new", ".dpkg-dist", ".dpkg-tmp", ".ucf-old", ".ucf-new", ".ucf-dist", 0};
  size_t l = strlen(n);
  for (int i = 0; suf[i]; i++) { size_t k = strlen(suf[i]); if (l >= k && !strcmp(n + l - k, suf[i])) return 1; }
  return 0;
}
/* The default keyrings. Returns 0, or -1 with err set for a file that exists and cannot be read. */
static int add_defaults(sig_keyring *kr, sbuf *err, strlist *used) {
  char *main = apt_dir("Dir::Etc::Trusted", "trusted.gpg");
  int rc = 0;
  if (path_exists(main) && add_file(kr, main, err, used) < 0) rc = -1;
  free(main);
  char *parts = apt_dir("Dir::Etc::TrustedParts", "trusted.gpg.d");
  DIR *d = opendir(parts);
  if (d) {
    strlist names = {0};
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') sl_add(&names, e->d_name);
    closedir(d);
    sl_sort(&names);
    for (int i = 0; i < names.n && rc == 0; i++) {
      size_t l = strlen(names.v[i]);
      char *p = path_join(parts, names.v[i]);
      if (l > 4 && (!strcmp(names.v[i] + l - 4, ".gpg") || !strcmp(names.v[i] + l - 4, ".asc"))) { if (add_file(kr, p, err, used) < 0) rc = -1; }
      else if (!quiet_name(names.v[i]))
        fprintf(stderr, "N: Ignoring file '%s' in directory '%s' as it has an invalid filename extension\n", names.v[i], parts);
      free(p);
    }
    sl_free(&names);
  }
  free(parts);
  return rc;
}
/* An armored block written into a deb822 field: drop the continuation indent, " ." is an empty line. */
static char *inline_block(const char *v) {
  sbuf b = {0};
  const char *p = v;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    const char *s = p;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    if (!(n == 1 && *s == '.')) sb_add(&b, s, n);
    sb_putc(&b, '\n');
    p += (nl ? (size_t)(nl - p) + 1 : strlen(p));
  }
  return b.s ? b.s : xstrdup("");
}

sig_keyring *apt_source_keyring(const char *signed_by, sbuf *err, strlist *used) {
  sig_keyring *kr = sig_keyring_new();
  if (!kr) { sb_printf(err, "out of memory"); return 0; }
  if (!signed_by || !*signed_by) {
    if (add_defaults(kr, err, used) < 0) { sig_keyring_free(kr); return 0; }
    return kr;
  }
  const char *v = signed_by;
  while (*v == ' ' || *v == '\t' || *v == '\n') v++;
  if (!strncmp(v, "-----BEGIN PGP PUBLIC KEY BLOCK-----", 36)) {
    char *blk = inline_block(v);
    char e[200];
    int r = sig_keyring_add(kr, blk, strlen(blk), e, sizeof e);
    free(blk);
    if (r < 0) { sb_printf(err, "the key block in Signed-By: %s", e); sig_keyring_free(kr); return 0; }
    if (used) sl_add(used, "(key block in Signed-By)");
    return kr;
  }
  strlist items = {0}, fprs = {0};
  char *c = xstrdup(v), *save = 0;
  for (char *t = strtok_r(c, ", \t\n", &save); t; t = strtok_r(0, ", \t\n", &save)) sl_add(is_fpr(t) ? &fprs : &items, t);
  free(c);
  int rc = 0;
  char *root = apt_dir("Dir", "/");
  for (int i = 0; i < items.n && rc == 0; i++) {
    if (items.v[i][0] != '/') { sb_printf(err, "Signed-By names '%s', which is neither an absolute file name nor a fingerprint", items.v[i]); rc = -1; break; }
    char *p = path_join(root, items.v[i]);
    if (add_file(kr, p, err, used) < 0) rc = -1;
    free(p);
  }
  free(root);
  if (rc == 0 && !items.n) rc = add_defaults(kr, err, used);
  if (rc == 0 && fprs.n) {
    sbuf f = {0};
    for (int i = 0; i < fprs.n; i++) sb_printf(&f, "%s ", fprs.v[i]);
    sig_keyring_select(kr, f.s);
    sb_free(&f);
  }
  sl_free(&items); sl_free(&fprs);
  if (rc < 0) { sig_keyring_free(kr); return 0; }
  return kr;
}

/* ---------- apt-keyring ---------- */
static void ymd(long long t, char *out, size_t cap) {
  time_t tt = (time_t)t;
  struct tm tm;
  gmtime_r(&tt, &tm);
  snprintf(out, cap, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}
static const char *algo_name(int a, int bits, char *buf, size_t cap) {
  if (a == 1 || a == 3) snprintf(buf, cap, "rsa%d", bits);
  else if (a == 22 || a == 27) snprintf(buf, cap, "ed25519");
  else snprintf(buf, cap, "algo%d", a);
  return buf;
}
static void spaced_fpr(const char *f, char *out) {  /* "AAAA BBBB ... EEEE  FFFF ..." as apt-key prints it */
  size_t o = 0;
  for (int i = 0; i < 40; i += 4) {
    if (i) out[o++] = ' ';
    if (i == 20) out[o++] = ' ';
    memcpy(out + o, f + i, 4); o += 4;
  }
  out[o] = 0;
}
static void list_keyring(const sig_keyring *kr, long long now) {
  for (int i = 0; i < sig_keyring_count(kr); i++) {
    sig_key_info k;
    sig_key_info_get(kr, i, &k);
    char a[24], d1[16], d2[16], fs[64];
    ymd(k.created, d1, sizeof d1);
    algo_name(k.algo, k.bits, a, sizeof a);
    char state[64] = "";
    if (k.revoked) snprintf(state, sizeof state, " [revoked]");
    else if (k.expires) { ymd(k.expires, d2, sizeof d2); snprintf(state, sizeof state, " [%s: %s]", k.expires <= now ? "expired" : "expires", d2); }
    if (!k.is_sub) {
      spaced_fpr(k.fpr, fs);
      printf("pub   %s %s [%s]%s\n      %s\n", a, d1, k.can_sign ? "SC" : "C", state, fs);
      printf("uid           %s\n", *k.uid ? k.uid : "(no valid user ID)");
    } else printf("sub   %s %s [%s]%s\n      %s\n", a, d1, k.can_sign ? "S" : "-", state, k.fpr);
    if (k.problem && (k.problem[0] != 'i' || strcmp(k.problem, "its key flags do not allow signing data")))
      printf("      not usable for signatures: %s\n", k.problem);
    if (!k.selected) printf("      not selected by the fingerprints in Signed-By\n");
    sig_key_info nx;
    if (i + 1 == sig_keyring_count(kr) || (sig_key_info_get(kr, i + 1, &nx) == 0 && !nx.is_sub)) printf("\n");
  }
}
static long long sig_now(void) {
  const char *t = aptconf_get("Debug::Acquire::Signature-Time", 0);
  return t ? atoll(t) : (long long)time(0);
}
long long apt_signature_time(void) { return sig_now(); }

int keyring_main(int argc, char **argv) {
  pkg_progname = "apt-keyring";
  /* -o and Dir as apt reads them */
  strlist rest = {0}, kfiles = {0};
  pkg_config_init();
  for (int i = 1; i < argc; i++) {
    if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--option")) && i + 1 < argc) {
      char *eq = strchr(argv[++i], '=');
      if (eq) { *eq = 0; aptconf_set(argv[i], eq + 1); *eq = '='; }
      continue;
    }
    if (!strcmp(argv[i], "--keyring") && i + 1 < argc) { sl_add(&kfiles, argv[++i]); continue; }
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("Usage: apt-keyring [-o Key=Value] COMMAND\n"
             "  list                     the default keyrings and each source's Signed-By keys\n"
             "  list FILE...             the keys in these keyring files\n"
             "  verify FILE [SIG]        check a signed file (InRelease, or Release with Release.gpg)\n"
             "                           against the default keyrings, or --keyring FILE (repeatable)\n");
      return 0;
    }
    sl_add(&rest, argv[i]);
  }
  const char *dir = aptconf_get("Dir", 0);
  if (dir) pkg_config_option("root", dir);
  else if (pkg_cfg.root) aptconf_set("Dir", pkg_cfg.root);
  pkg_config_finish();
  if (!aptconf_get("Dir", 0)) aptconf_set("Dir", pkg_cfg.root);
  char *mainf = apt_dir("Dir::Etc::main", "apt.conf");
  aptconf_parse_file(mainf);
  free(mainf);
  long long now = sig_now();
  if (!rest.n) { pkg_error("no command given (try --help)"); return 100; }
  const char *cmd = rest.v[0];
  int rc = 0;
  if (!strcmp(cmd, "list") && rest.n > 1) {
    for (int i = 1; i < rest.n; i++) {
      sig_keyring *kr = sig_keyring_new();
      sbuf err = {0};
      if (add_file(kr, rest.v[i], &err, 0) < 0) { pkg_error("%s", err.s); rc = 100; }
      else { printf("%s\n", rest.v[i]); for (size_t k = strlen(rest.v[i]); k; k--) putchar('-'); printf("\n"); list_keyring(kr, now); }
      sb_free(&err);
      sig_keyring_free(kr);
    }
  } else if (!strcmp(cmd, "list")) {
    strlist used = {0};
    sbuf err = {0};
    sig_keyring *kr = apt_source_keyring(0, &err, &used);
    if (!kr) { pkg_error("%s", err.s); rc = 100; }
    else {
      for (int i = 0; i < used.n; i++) {
        sig_keyring *k1 = sig_keyring_new();
        sbuf e2 = {0};
        if (add_file(k1, used.v[i], &e2, 0) == 0) {
          printf("%s\n", used.v[i]);
          for (size_t k = strlen(used.v[i]); k; k--) putchar('-');
          printf("\n");
          list_keyring(k1, now);
        }
        sb_free(&e2);
        sig_keyring_free(k1);
      }
      if (!used.n) printf("No default keyrings (%s or *.gpg / *.asc in %s).\n", "trusted.gpg", "trusted.gpg.d/");
      sig_keyring_free(kr);
    }
    sl_free(&used);
    apt_sources srcs;
    sources_load(&srcs);
    for (int i = 0; i < srcs.n; i++) {
      apt_source *s = &srcs.v[i];
      printf("Source %s %s (%s): ", s->uri, s->suite, s->origin);
      if (s->trusted) printf("trusted=yes: signatures are not checked\n");
      else if (!s->signed_by) printf("the default keyrings\n");
      else {
        sbuf e = {0};
        strlist u = {0};
        sig_keyring *k = apt_source_keyring(s->signed_by, &e, &u);
        if (!k) { printf("cannot be checked: %s\n", e.s); rc = 100; }
        else {
          int sel = 0;
          for (int j = 0; j < sig_keyring_count(k); j++) { sig_key_info ki; sig_key_info_get(k, j, &ki); sel += ki.selected; }
          printf("Signed-By, %d key%s and subkey%s selected from:", sel, sel == 1 ? "" : "s", sel == 1 ? "" : "s");
          for (int j = 0; j < u.n; j++) printf(" %s", u.v[j]);
          printf("\n");
          sig_keyring_free(k);
        }
        sb_free(&e); sl_free(&u);
      }
    }
    sources_free(&srcs);
    sb_free(&err);
  } else if (!strcmp(cmd, "verify") && (rest.n == 2 || rest.n == 3)) {
    sbuf err = {0};
    sig_keyring *kr = 0;
    if (kfiles.n) {
      kr = sig_keyring_new();
      for (int i = 0; i < kfiles.n && kr; i++) if (add_file(kr, kfiles.v[i], &err, 0) < 0) { sig_keyring_free(kr); kr = 0; }
    } else kr = apt_source_keyring(0, &err, 0);
    if (!kr) { pkg_error("%s", err.s); sb_free(&err); return 100; }
    char *data, *sig = 0;
    size_t n, sn = 0;
    if (file_read(rest.v[1], &data, &n) < 0 || (rest.n == 3 && file_read(rest.v[2], &sig, &sn) < 0)) { pkg_error("cannot read %s", rest.v[rest.n - 1]); sig_keyring_free(kr); return 100; }
    sig_report r;
    char e[200];
    int k = rest.n == 3 ? sig_check_detached(kr, data, n, sig, sn, now, &r, e, sizeof e) : sig_check_cleartext(kr, data, n, now, &r, e, sizeof e);
    if (k < 0) { printf("%s: not a signed file: %s\n", rest.v[1], e); rc = 2; }
    else {
      for (int i = 0; i < r.n; i++) {
        sig_result *x = &r.r[i];
        printf("%-10s %s %s%s%s\n", sig_status_name(x->status), x->keyid[0] ? x->keyid : "-", *x->uid ? x->uid : "", *x->uid ? ": " : "", x->why);
      }
      int t = sig_report_trusted(&r);
      printf("%s: %s (%d good, %d bad of %d)\n", rest.v[1], t ? "trusted" : "NOT trusted", r.ngood, r.nbad, r.n);
      rc = t ? 0 : 1;
      sig_report_free(&r);
    }
    free(data); free(sig);
    sig_keyring_free(kr);
    sb_free(&err);
  } else { pkg_error("unknown command '%s' (try --help)", cmd); rc = 100; }
  sl_free(&rest); sl_free(&kfiles);
  return rc;
}
