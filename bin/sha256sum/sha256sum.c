// SPDX-License-Identifier: GPL-2.0-or-later
/* sha256sum, sha512sum, md5sum (by the name they are started under).
 *   NAME [-bt] [--tag] [file...]              print checksums
 *   NAME -c [--quiet] [--status] [--strict] [-w] [--ignore-missing] [file...]
 * Output and check-file format are GNU coreutils': "hash  name", with a
 * backslash in front of lines whose name holds a backslash or a newline. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "arc.h"

enum { MD5, SHA256, SHA512 };
static int algo = SHA256, dlen = 32;
static const char *prog = "sha256sum", *tagname = "SHA256";
static int tag, check, quiet, status, strict, warn_fmt, ignore_missing, zero;

/* Hashes a file into out. Returns 0, or -1 with errno set. */
static int hash_fd(int fd, unsigned char *out) {
  static unsigned char buf[65536];
  arc_md5 m;
  arc_sha256 s2;
  arc_sha512 s5;
  if (algo == MD5) arc_md5_init(&m);
  else if (algo == SHA256) arc_sha256_init(&s2);
  else arc_sha512_init(&s5);
  long r;
  while ((r = read(fd, buf, sizeof buf)) > 0) {
    if (algo == MD5) arc_md5_update(&m, buf, r);
    else if (algo == SHA256) arc_sha256_update(&s2, buf, r);
    else arc_sha512_update(&s5, buf, r);
  }
  if (r < 0) return -1;
  if (algo == MD5) arc_md5_final(&m, out);
  else if (algo == SHA256) arc_sha256_final(&s2, out);
  else arc_sha512_final(&s5, out);
  return 0;
}

static int hash_file(const char *name, unsigned char *out) {
  int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
  if (fd < 0) return -1;
  int r = hash_fd(fd, out);
  int e = errno;
  if (fd) close(fd);
  errno = e;
  return r;
}

static void put_hex(const unsigned char *d) {
  for (int i = 0; i < dlen; i++) printf("%02x", d[i]);
}

static void print_one(const char *name, const unsigned char *d) {
  int esc = !zero && (strchr(name, '\\') || strchr(name, '\n'));
  if (tag) {
    printf("%s%s (", esc ? "\\" : "", tagname);
  } else {
    if (esc) putchar('\\');
    put_hex(d);
    printf("  ");
  }
  for (const char *p = name; *p; p++) {
    if (esc && *p == '\\') printf("\\\\");
    else if (esc && *p == '\n') printf("\\n");
    else putchar(*p);
  }
  if (tag) { printf(") = "); put_hex(d); }
  putchar(zero ? 0 : '\n');
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex(const char *s, unsigned char *d) {
  for (int i = 0; i < dlen; i++) {
    int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
    if (b < 0) return -1;
    d[i] = a << 4 | b;
  }
  return 0;
}

/* Undo the backslash escapes of an escaped line, in place. */
static int unescape(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s != '\\') { *o++ = *s; continue; }
    s++;
    if (*s == '\\') *o++ = '\\';
    else if (*s == 'n') *o++ = '\n';
    else return -1;
  }
  *o = 0;
  return 0;
}

static int check_file(const char *list) {
  FILE *f = strcmp(list, "-") ? fopen(list, "r") : stdin;
  if (!f) { fprintf(stderr, "%s: %s: %s\n", prog, list, strerror(errno)); return 1; }
  char *line = 0;
  size_t cap = 0;
  long n, lineno = 0;
  int bad_lines = 0, failed = 0, unreadable = 0, ok_any = 0, rc = 0;
  while ((n = getline(&line, &cap, f)) > 0) {
    lineno++;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    char *p = line, *name = 0;
    unsigned char want[64];
    int esc = 0;
    if (*p == '\\') { esc = 1; p++; }
    size_t tl = strlen(tagname);
    if (!strncmp(p, tagname, tl) && p[tl] == ' ' && p[tl + 1] == '(') { /* BSD style */
      char *close = strstr(p, ") = ");
      if (close && strlen(close + 4) == (size_t)dlen * 2 && parse_hex(close + 4, want) == 0) {
        *close = 0;
        name = p + tl + 2;
      }
    } else if (strlen(p) > (size_t)dlen * 2 + 2 && parse_hex(p, want) == 0 && p[dlen * 2] == ' ' &&
               (p[dlen * 2 + 1] == ' ' || p[dlen * 2 + 1] == '*')) {
      name = p + dlen * 2 + 2;
    }
    if (!name || (esc && unescape(name) < 0)) {
      bad_lines++;
      if (warn_fmt) fprintf(stderr, "%s: %s: %ld: improperly formatted %s checksum line\n", prog, list, lineno, tagname);
      continue;
    }
    unsigned char got[64];
    if (hash_file(name, got) < 0) {
      if (ignore_missing && errno == ENOENT) continue;
      unreadable++;
      if (!status) { printf("%s: FAILED open or read\n", name); }
      fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno));
      continue;
    }
    ok_any = 1;
    if (memcmp(got, want, dlen)) {
      failed++;
      if (!status) printf("%s: FAILED\n", name);
    } else if (!status && !quiet) printf("%s: OK\n", name);
  }
  free(line);
  if (f != stdin) fclose(f);
  fflush(stdout);
  if (!ok_any && !failed && !unreadable && bad_lines) {
    fprintf(stderr, "%s: %s: no properly formatted %s checksum lines found\n", prog, list, tagname);
    return 1;
  }
  if (!status) {
    if (bad_lines) fprintf(stderr, "%s: WARNING: %d line%s improperly formatted\n", prog, bad_lines, bad_lines == 1 ? " is" : "s are");
    if (unreadable) fprintf(stderr, "%s: WARNING: %d listed file%s could not be read\n", prog, unreadable, unreadable == 1 ? "" : "s");
    if (failed) fprintf(stderr, "%s: WARNING: %d computed checksum%s did NOT match\n", prog, failed, failed == 1 ? "" : "s");
  }
  if (ignore_missing && !ok_any && !failed && !unreadable) {
    fprintf(stderr, "%s: %s: no file was verified\n", prog, list);
    rc = 1;
  }
  if (failed || unreadable || (strict && bad_lines)) rc = 1;
  return rc;
}

int main(int argc, char **argv) {
  const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
  if (strstr(me, "md5")) { algo = MD5; dlen = 16; prog = "md5sum"; tagname = "MD5"; }
  else if (strstr(me, "512")) { algo = SHA512; dlen = 64; prog = "sha512sum"; tagname = "SHA512"; }
  char **files = calloc(argc + 1, sizeof *files);
  int nf = 0, opts = 1, rc = 0;
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (!opts || a[0] != '-' || !a[1]) { files[nf++] = a; continue; }
    if (!strcmp(a, "--")) { opts = 0; continue; }
    if (!strcmp(a, "--check")) check = 1;
    else if (!strcmp(a, "--tag")) tag = 1;
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "--status")) status = 1;
    else if (!strcmp(a, "--strict")) strict = 1;
    else if (!strcmp(a, "--warn")) warn_fmt = 1;
    else if (!strcmp(a, "--ignore-missing")) ignore_missing = 1;
    else if (!strcmp(a, "--zero")) zero = 1;
    else if (!strcmp(a, "--binary") || !strcmp(a, "--text")) {}
    else if (!strcmp(a, "--help")) { printf("usage: %s [-bctwz] [--tag] [--quiet] [--status] [--strict] [--ignore-missing] [file...]\n", prog); return 0; }
    else if (!strcmp(a, "--version")) { printf("%s (UC.OS libarc) 1.0\n", prog); return 0; }
    else if (a[1] == '-') { fprintf(stderr, "%s: unknown option '%s'\n", prog, a); return 1; }
    else
      for (char *f = a + 1; *f; f++) {
        if (*f == 'c') check = 1;
        else if (*f == 'w') warn_fmt = 1;
        else if (*f == 'z') zero = 1;
        else if (*f == 'b' || *f == 't') {}
        else { fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *f); return 1; }
      }
  }
  if (nf == 0) files[nf++] = "-";
  for (int i = 0; i < nf; i++) {
    if (check) { rc |= check_file(files[i]); continue; }
    unsigned char d[64];
    if (hash_file(files[i], d) < 0) { fprintf(stderr, "%s: %s: %s\n", prog, files[i], strerror(errno)); rc = 1; continue; }
    print_one(files[i], d);
  }
  fflush(stdout);
  free(files);
  return rc;
}
