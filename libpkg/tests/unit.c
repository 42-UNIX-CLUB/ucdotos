// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for libpkg: versions, control paragraphs, relationship fields,
 * hashes. Native only. Exits non-zero if any check fails.
 *
 * Where the version table comes from: no dpkg binary was available to produce
 * answers, so every row is either an example given in deb-version(7) or Debian
 * Policy 5.6.12, or follows from the algorithm those two documents state (the
 * comment on each block says which rule it exercises). An independent check of
 * the same rules, written separately in Python, is version-oracle.py. */
#include "../pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static const struct { const char *a, *b; int r; } vt[] = {
  /* deb-version(7) / Policy: "~~" < "~~a" < "~" < "" < "a" */
  {"1.0~~", "1.0~~a", -1}, {"1.0~~a", "1.0~", -1}, {"1.0~", "1.0", -1}, {"1.0", "1.0a", -1},
  {"1.0~~", "1.0", -1}, {"1.0a", "1.0~", 1}, {"~~", "~~a", -1}, {"0~", "0", -1},
  /* tilde for pre-releases (Policy example: 1.0~rc1 sorts before 1.0) */
  {"1.0~rc1", "1.0", -1}, {"1.0~rc1-1", "1.0-1", -1}, {"1.0~rc1", "1.0~rc2", -1}, {"1.0~beta", "1.0~rc", -1},
  {"2:1.2~beta", "2:1.2", -1}, {"1.2.3-1~bpo1", "1.2.3-1", -1}, {"1.2.3-1~ubuntu1", "1.2.3-1", -1},
  /* equality, including leading zeros (digit runs compare as numbers) */
  {"1.0", "1.0", 0}, {"1.001", "1.1", 0}, {"1.01", "1.1", 0}, {"001", "1", 0}, {"1.0-0", "1.0", 0},
  {"0:1.0", "1.0", 0}, {"00:1.0", "1.0", 0}, {"1.0-00", "1.0-0", 0}, {"a", "a", 0}, {"1:0", "1:0", 0},
  /* missing revision equals revision 0 (Policy 5.6.12) */
  {"1.0", "1.0-1", -1}, {"1.0-0", "1.0-1", -1}, {"1.0", "1.0-0.1", -1},
  /* epochs dominate (Policy: epoch is compared first) */
  {"1:0.1", "2.0", 1}, {"0.9", "1:0.1", -1}, {"10:1", "9:1", 1}, {"1:1.0", "1:1.0-1", -1}, {"2:0", "1:99999", 1},
  {"1:2.0", "2:1.0", -1},
  /* numeric runs compare as numbers, not text */
  {"2.30-1", "2.4-1", 1}, {"1.0-2", "1.0-10", -1}, {"9", "10", -1}, {"1.9", "1.10", -1}, {"1.2.10", "1.2.9", 1},
  {"0.99", "1", -1}, {"12345678901234567890", "12345678901234567891", -1}, {"100", "99", 1},
  /* letters sort before non-letters; "+" and "." are non-letters */
  {"1.0+", "1.0a", 1}, {"1.0.", "1.0a", 1}, {"1.0+dfsg-1", "1.0-1", 1}, {"1.0a", "1.0b", -1}, {"1.0A", "1.0a", -1},
  {"1.0Z", "1.0a", -1}, {"1.0+", "1.0.", -1}, {"1.0-1+b1", "1.0-1", 1}, {"1.0-1+deb12u1", "1.0-2", -1},
  {"1.0-1+deb12u1", "1.0-1+deb12u2", -1}, {"1.0-1", "1.0-1a", -1}, {"1.0-a", "1.0-1", 1},
  /* a longer string wins when one is a prefix of the other, unless the extra part starts with "~" */
  {"1.0.0", "1.0", 1}, {"3.0", "3.0.0~", -1}, {"1.2.3", "1.2", 1}, {"1.2.3", "1.2.3.0", -1}, {"1.2", "1.2~", 1},
  {"1.2-3", "1.2-3.1", -1}, {"1.18.4ubuntu1.7", "1.18.4", 1}, {"1.18.4", "1.18.4ubuntu1", -1},
  /* realistic pairs */
  {"2.36-9+deb12u4", "2.36-9+deb12u10", -1}, {"9.4p1-1", "9.4-1", 1}, {"7.88.1-10+deb12u5", "8.5.0-2", -1},
  {"1:2.38.1-5+deb12u1", "2.39.3-6", 1}, {"0.0~git20230101.abcdef-1", "0.0~git20230202.123456-1", -1},
  {"1.0~git1", "1.0~~git1", 1}, {"5.2.15-2+b2", "5.2.15-2+b10", -1}, {"3.11.2-1+b1", "3.11.2-1", 1},
  {"2.4.57-2", "2.4.57-2~deb12u1", 1}, {"1:9.18.19-1~deb12u1", "1:9.18.24-1", -1},
  /* hyphens in upstream: the revision is after the LAST hyphen */
  {"1.0-beta-1", "1.0-beta-2", -1}, {"1.0-beta-1", "1.0-1", 1}, {"1-2-3", "1-2-4", -1}, {"1-2-3", "1-3-1", -1},
  /* colons in upstream are allowed when there is an epoch */
  {"1:1:2", "1:1:3", -1},
  {0, 0, 0}};

static const char *bad_versions[] = {"", " ", "a:1.0", ":1.0", "1.0-", "1.0 2", "1.0-1_2", "1:", "-1:1.0", "1.0-a-", "1.0$", 0};
static const char *good_versions[] = {"0", "1.0", "1:1.0", "1.0-1", "1.0~rc1+dfsg-1ubuntu0.1", "2:1.0.0+git20230101~1-1.1", "1.0-1-1", 0};

static void test_versions(void) {
  for (int i = 0; vt[i].a; i++) {
    int r = ver_cmp(vt[i].a, vt[i].b), q = ver_cmp(vt[i].b, vt[i].a);
    char what[200];
    snprintf(what, sizeof what, "ver_cmp(%s, %s) == %d (got %d)", vt[i].a, vt[i].b, vt[i].r, r);
    CHECK(what, r == vt[i].r);
    snprintf(what, sizeof what, "ver_cmp(%s, %s) is antisymmetric", vt[i].b, vt[i].a);
    CHECK(what, q == -vt[i].r);
  }
  for (int i = 0; bad_versions[i]; i++) { char w[100]; snprintf(w, sizeof w, "'%s' is rejected", bad_versions[i]); CHECK(w, ver_check(bad_versions[i]) != 0); }
  for (int i = 0; good_versions[i]; i++) { char w[100]; snprintf(w, sizeof w, "'%s' is accepted", good_versions[i]); CHECK(w, ver_check(good_versions[i]) == 0); }
  CHECK("non-digit start is flagged", ver_check("abc") && !strcmp(ver_check("abc"), "version number does not start with digit"));
  CHECK(">= satisfied", ver_satisfies("1.2", REL_GE, "1.2"));
  CHECK("<< not satisfied by equal", !ver_satisfies("1.2", REL_LT, "1.2"));
  CHECK(">> satisfied", ver_satisfies("1.2-1", REL_GT, "1.2"));
  CHECK("= with epoch", ver_satisfies("0:1.2", REL_EQ, "1.2"));
  CHECK("<= tilde", ver_satisfies("1.2~", REL_LE, "1.2"));
}

static void test_control(void) {
  const char *text =
    "# a comment before the paragraph\n"
    "Package: hello\n"
    "Version:   2.10-3  \n"
    "Depends: libc6 (>= 2.34),\n"
    " foo | bar\n"
    "# a comment between continuation lines\n"
    "\tbaz\n"
    "Description: example package\n"
    " This is the long description.\n"
    " .\n"
    " Second paragraph.\n"
    "\n"
    "   \t\n"
    "\n"
    "package: second\n"
    "conffiles:\n"
    " /etc/a 0123\n"
    "\n";
  ctl_reader *r = ctl_open_mem(text, strlen(text));
  ctl_para p = {0};
  CHECK("first paragraph", ctl_next(r, &p) == 1);
  CHECK("Package", ctl_get(&p, "package") && !strcmp(ctl_get(&p, "Package"), "hello"));
  CHECK("value trimmed", !strcmp(ctl_get(&p, "Version"), "2.10-3"));
  CHECK("folded field kept with continuation lines", !strcmp(ctl_get(&p, "Depends"), "libc6 (>= 2.34),\n foo | bar\n\tbaz"));
  CHECK("multi-line description", !strcmp(ctl_get(&p, "Description"), "example package\n This is the long description.\n .\n Second paragraph."));
  CHECK("four fields", p.n == 4);
  CHECK("paragraph line number", p.line == 2);
  CHECK("whitespace-only lines separate paragraphs", ctl_next(r, &p) == 1);
  CHECK("names are case-insensitive", !strcmp(ctl_get(&p, "Package"), "second"));
  CHECK("empty first line of a multi-line field", !strcmp(ctl_get(&p, "Conffiles"), "\n /etc/a 0123"));
  sbuf b = {0};
  ctl_write(&b, &p);
  CHECK("round trip keeps the empty first line", !strcmp(b.s, "package: second\nconffiles:\n /etc/a 0123\n"));
  sb_free(&b);
  CHECK("end", ctl_next(r, &p) == 0);
  ctl_close(r);

  const char *bad[] = {"Package hello\n", " leading continuation\n", "A: 1\nA: 2\n", "-Bad: x\n", "Bad Name: x\n", 0};
  for (int i = 0; bad[i]; i++) {
    r = ctl_open_mem(bad[i], strlen(bad[i]));
    char w[100]; snprintf(w, sizeof w, "malformed input %d is an error", i);
    CHECK(w, ctl_next(r, &p) == -1);
    ctl_close(r);
  }
  /* CRLF and no final newline */
  const char *crlf = "Package: x\r\nVersion: 1\r\n";
  r = ctl_open_mem(crlf, strlen(crlf));
  CHECK("CRLF paragraph", ctl_next(r, &p) == 1 && !strcmp(ctl_get(&p, "Version"), "1"));
  ctl_close(r);
  /* set, replace, delete */
  ctl_reset(&p);
  ctl_set(&p, "A", "1"); ctl_set(&p, "B", "2"); ctl_set(&p, "a", "3"); ctl_del(&p, "B");
  CHECK("set replaces case-insensitively", p.n == 1 && !strcmp(ctl_get(&p, "A"), "3"));
  ctl_free(&p);
  /* streaming through a reader with a tiny buffer source */
  struct { const char *s; size_t n, pos; } src = {"P: 1\n\nP: 2\nQ: very long value that crosses many reads\n", 0, 0};
  src.n = strlen(src.s);
  long one(void *ctx, void *buf, unsigned long n);
  r = ctl_open(one, &src);
  int count = 0, qok = 0;
  uint64_t off = 0;
  while (ctl_next(r, &p) == 1) { count++; qok = ctl_get(&p, "Q") && !strcmp(ctl_get(&p, "Q"), "very long value that crosses many reads"); off = ctl_offset(r); }
  CHECK("streaming parser with 1-byte reads", count == 2 && qok);
  CHECK("offset of the last paragraph", off == 6);
  ctl_close(r);
  ctl_free(&p);
}
long one(void *ctx, void *buf, unsigned long n) {
  struct { const char *s; size_t n, pos; } *s = ctx;
  if (s->pos >= s->n || !n) return 0;
  *(char *)buf = s->s[s->pos++];
  return 1;
}

static void test_deps(void) {
  dep_list l;
  char *err;
  CHECK("parse", dep_parse("libc6 (>= 2.34), foo:any | bar (<< 2~) , baz:native,\n qux (= 1:1.0-1)", DEP_ALT, &l, &err) == 0);
  CHECK("4 groups", l.n == 4);
  CHECK("alternatives", l.o[1].n == 2 && !strcmp(l.o[1].a[0].name, "foo") && !strcmp(l.o[1].a[0].archq, "any"));
  CHECK("relation <<", l.o[1].a[1].op == REL_LT && !strcmp(l.o[1].a[1].ver, "2~"));
  CHECK(":native", !strcmp(l.o[2].a[0].archq, "native"));
  CHECK("epoch version", l.o[3].a[0].op == REL_EQ && !strcmp(l.o[3].a[0].ver, "1:1.0-1"));
  sbuf b = {0};
  dep_format(&b, &l);
  CHECK("format", !strcmp(b.s, "libc6 (>= 2.34), foo:any | bar (<< 2~), baz:native, qux (= 1:1.0-1)"));
  sb_free(&b);
  dep_free(&l);
  CHECK("obsolete < means <=", dep_parse("a (< 1), b (> 2)", 0, &l, &err) == 0 && l.o[0].a[0].op == REL_LE && l.o[1].a[0].op == REL_GE);
  dep_free(&l);
  CHECK("all five relations", dep_parse("a (<< 1), b (<= 1), c (= 1), d (>= 1), e (>> 1)", 0, &l, &err) == 0 &&
        l.o[0].a[0].op == REL_LT && l.o[1].a[0].op == REL_LE && l.o[2].a[0].op == REL_EQ && l.o[3].a[0].op == REL_GE && l.o[4].a[0].op == REL_GT);
  dep_free(&l);
  CHECK("no alternatives in Conflicts", dep_parse("a | b", 0, &l, &err) == -1); free(err);
  CHECK("Provides with = version", dep_parse("foo (= 1.0), bar", DEP_PROVIDES, &l, &err) == 0 && l.n == 2); dep_free(&l);
  CHECK("Provides with >= is an error", dep_parse("foo (>= 1.0)", DEP_PROVIDES, &l, &err) == -1); free(err);
  CHECK("restrictions refused in binary fields", dep_parse("a [amd64]", DEP_ALT, &l, &err) == -1); free(err);
  CHECK("restriction lists", dep_parse("a [amd64 arm64] <!nocheck> <cross>, b [!i386] | c", DEP_ALT | DEP_RESTRICT, &l, &err) == 0 &&
        l.o[0].a[0].arches.n == 2 && !l.o[0].a[0].arch_neg && l.o[1].a[0].arch_neg && !strcmp(l.o[0].a[0].profiles, "<!nocheck> <cross>"));
  dep_reduce_arch(&l, "i386");
  CHECK("reduce for i386 drops a and b", l.n == 1 && l.o[0].n == 1 && !strcmp(l.o[0].a[0].name, "c"));
  dep_free(&l);
  CHECK("mixed negation is an error", dep_parse("a [amd64 !i386]", DEP_RESTRICT, &l, &err) == -1); free(err);
  CHECK("missing ) is an error", dep_parse("a (>= 1", 0, &l, &err) == -1); free(err);
  CHECK("bad version is an error", dep_parse("a (>= 1:)", 0, &l, &err) == -1); free(err);
  CHECK("trailing comma is fine", dep_parse("a, b,", 0, &l, &err) == 0 && l.n == 2); dep_free(&l);
  CHECK("upper case name is an error", dep_parse("Foo", 0, &l, &err) == -1); free(err);
  CHECK("empty field", dep_parse("", 0, &l, &err) == 0 && l.n == 0);
  CHECK("arch wildcards", arch_matches("linux-any", "riscv64") && arch_matches("any-arm64", "arm64") && !arch_matches("any-arm64", "amd64") && arch_matches("any", "all"));
}

static void test_hashes(void) {
  char h[129];
  unsigned char d[64];
  arc_sha256 s; arc_sha256_init(&s); arc_sha256_update(&s, "abc", 3); arc_sha256_final(&s, d); hex_encode(d, 32, h);
  CHECK("sha256(abc)", !strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  arc_md5 m; arc_md5_init(&m); arc_md5_update(&m, "abc", 3); arc_md5_final(&m, d); hex_encode(d, 16, h);
  CHECK("md5(abc)", !strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
  arc_sha512 x; arc_sha512_init(&x); arc_sha512_update(&x, "abc", 3); arc_sha512_final(&x, d); hex_encode(d, 64, h);
  CHECK("sha512(abc)", !strcmp(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
  arc_sha256_init(&s);
  char thousand[1000];
  memset(thousand, 'a', sizeof thousand);
  for (int i = 0; i < 1000; i++) arc_sha256_update(&s, thousand, 1000);
  arc_sha256_final(&s, d); hex_encode(d, 32, h);
  CHECK("sha256 of a million a's", !strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_paths(void) {
  char out[256];
  CHECK("./usr/bin/x", deb_norm_path("./usr/bin/x", out, sizeof out) && !strcmp(out, "/usr/bin/x"));
  CHECK("./", deb_norm_path("./", out, sizeof out) && !strcmp(out, "/"));
  CHECK("trailing slash", deb_norm_path("./usr/share/", out, sizeof out) && !strcmp(out, "/usr/share"));
  CHECK("doubled slashes", deb_norm_path("usr//./lib", out, sizeof out) && !strcmp(out, "/usr/lib"));
  CHECK(".. refused", !deb_norm_path("./usr/../../etc/passwd", out, sizeof out));
  char *j = path_join("/root/", "/var/lib"); CHECK("path_join", !strcmp(j, "/root/var/lib")); free(j);
  j = path_join("/", "etc"); CHECK("path_join root", !strcmp(j, "/etc")); free(j);
  CHECK("glob *", pkg_glob("lib*", "libc6") && !pkg_glob("lib*", "xlib"));
  CHECK("glob ? and []", pkg_glob("a?c", "abc") && pkg_glob("[a-c]x", "bx") && !pkg_glob("[!a-c]x", "bx"));
  CHECK("glob path", pkg_glob("/usr/share/doc/*", "/usr/share/doc/hello/copyright"));
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--vercmp")) {  /* for version-oracle.py: "a b" lines in, -1/0/1 out */
    char a[256], b[256];
    while (scanf("%255s %255s", a, b) == 2) printf("%d\n", ver_cmp(a, b));
    return 0;
  }
  test_versions();
  test_control();
  test_deps();
  test_hashes();
  test_paths();
  printf("unit: %d checks, %d failed\n", pass + fail, fail);
  return fail != 0;
}
