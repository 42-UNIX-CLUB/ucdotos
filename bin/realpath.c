// SPDX-License-Identifier: GPL-2.0-or-later
/* realpath [-e | -m] [-s] [-q] [-z] [--relative-to=DIR] [--relative-base=DIR] FILE...
 * Makes each name absolute, removes . and .. and repeated slashes, and
 * follows symbolic links (unless -s). By default every component but the
 * last must exist; -e requires all of them, -m none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __wasm__
#include <sys/syscall.h>
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif

enum { E_LAST, E_ALL, E_NONE };
static int need = E_LAST, nolinks, quiet;

static long rdlink(const char *p, char *buf, size_t n) {
#ifdef __wasm__
  return __sysret(__syscall4(78, AT_FDCWD, p, buf, n)); /* readlinkat: the kernel may not have links yet */
#else
  return readlink(p, buf, n);
#endif
}

/* canonical form of path into out (PATH_MAX); 0 or an errno */
static int canon(const char *path, char *out) {
  char work[8192];
  size_t n = 0;
  int links = 0;
  if (!*path) return ENOENT;
  if (path[0] != '/') {
    if (!getcwd(out, 4096)) return errno;
    n = strlen(out);
    if (n == 1) n = 0;
  }
  if (strlen(path) >= sizeof work) return ENAMETOOLONG;
  strcpy(work, path);
  char *p = work;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    char *e = p;
    while (*e && *e != '/') e++;
    size_t len = e - p;
    int last = !*e || !e[strspn(e, "/")];
    if (len == 1 && p[0] == '.') { p = e; continue; }
    if (len == 2 && p[0] == '.' && p[1] == '.') { while (n && out[n - 1] != '/') n--; if (n) n--; p = e; continue; }
    if (n + 1 + len >= 4096) return ENAMETOOLONG;
    out[n] = '/';
    memcpy(out + n + 1, p, len);
    size_t save = n;
    n += 1 + len;
    out[n] = 0;
    struct stat st;
    if (lstat(out, &st) != 0) {
      if (need != E_NONE && (errno != ENOENT || need == E_ALL || !last)) return errno;
      p = e;
      continue;
    }
    if (!nolinks && (st.st_mode & 0170000) == S_IFLNK) {
      char target[4096];
      long r = rdlink(out, target, sizeof target - 1);
      if (r < 0) return errno;
      target[r] = 0;
      if (++links > 40) return ELOOP;
      if (r + strlen(e) + 1 >= sizeof work) return ENAMETOOLONG;
      char rest[8192];
      strcpy(rest, e);
      strcpy(work, target);
      strcat(work, rest);
      p = work;
      if (target[0] == '/') n = 0; else n = save;
      continue;
    }
    if (!last && !S_ISDIR(st.st_mode)) {
      if (need != E_NONE) return ENOTDIR;
    }
    p = e;
  }
  if (!n) out[n++] = '/';
  out[n] = 0;
  return 0;
}

/* path of a relative to directory b (both canonical and absolute) */
static void relative(const char *a, const char *b, char *out) {
  size_t common = 0; /* length of the shared leading components */
  if (strcmp(b, "/")) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    if ((!a[i] || a[i] == '/') && (!b[i] || b[i] == '/')) common = i;
    else { while (i > 0 && a[i - 1] != '/') i--; common = i ? i - 1 : 0; }
  }
  out[0] = 0;
  if (strcmp(b, "/"))
    for (const char *q = b + common; *q; q++)
      if (*q == '/') strcat(out, *out ? "/.." : "..");
  const char *tail = a + common;
  if (*tail == '/') tail++;
  if (*tail) { if (*out) strcat(out, "/"); strcat(out, tail); }
  if (!*out) strcpy(out, ".");
}

int main(int argc, char **argv) {
  int i = 1, rc = 0, zero = 0;
  const char *relto = NULL, *relbase = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--canonicalize-existing")) need = E_ALL;
    else if (!strcmp(a, "--canonicalize-missing")) need = E_NONE;
    else if (!strcmp(a, "--strip") || !strcmp(a, "--no-symlinks")) nolinks = 1;
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "--zero")) zero = 1;
    else if (!strncmp(a, "--relative-to=", 14)) relto = a + 14;
    else if (!strncmp(a, "--relative-base=", 16)) relbase = a + 16;
    else if (a[1] == '-') { fprintf(stderr, "realpath: unknown option '%s'\n", a); return 1; }
    else for (char *f = a + 1; *f; f++) {
      if (*f == 'e') need = E_ALL;
      else if (*f == 'm') need = E_NONE;
      else if (*f == 's') nolinks = 1;
      else if (*f == 'q') quiet = 1;
      else if (*f == 'z') zero = 1;
      else if (*f == 'L' || *f == 'P') ;
      else { fprintf(stderr, "realpath: invalid option -- '%c'\nusage: realpath [-e|-m] [-s] [-q] [-z] FILE...\n", *f); return 1; }
    }
  }
  if (i >= argc) { fprintf(stderr, "realpath: missing operand\n"); return 1; }
  static char to[4096], base[4096];
  if (relto) { int e = canon(relto, to); if (e) { fprintf(stderr, "realpath: %s: %s\n", relto, strerror(e)); return 1; } }
  if (relbase) { int e = canon(relbase, base); if (e) { fprintf(stderr, "realpath: %s: %s\n", relbase, strerror(e)); return 1; } if (!relto) strcpy(to, base); }
  for (; i < argc; i++) {
    char out[4096], rel[8192];
    int e = canon(argv[i], out);
    if (e) { if (!quiet) fprintf(stderr, "realpath: %s: %s\n", argv[i], strerror(e)); rc = 1; continue; }
    const char *show = out;
    if (relto || relbase) {
      size_t bl = strlen(base);
      int inside = !relbase || !strcmp(base, "/") || (!strncmp(out, base, bl) && (out[bl] == '/' || !out[bl]));
      int toinside = !relbase || !strcmp(base, "/") || (!strncmp(to, base, strlen(base)) && (to[bl] == '/' || !to[bl]));
      if (inside && toinside) { relative(out, to, rel); show = rel; }
    }
    fputs(show, stdout);
    putchar(zero ? 0 : '\n');
  }
  return rc;
}
