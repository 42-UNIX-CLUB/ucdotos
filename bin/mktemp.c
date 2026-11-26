// SPDX-License-Identifier: GPL-2.0-or-later
/* mktemp [-d] [-u] [-q] [-t] [-p DIR] [--suffix=SUFF] [--tmpdir[=DIR]] [TEMPLATE]
 * Creates a file (or a directory with -d) with a unique name and prints it.
 * The trailing X's of the template (at least three; default tmp.XXXXXXXXXX)
 * become random letters and digits from the kernel's random source. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/random.h>

static int quiet;
/* usage and template errors are always reported; -q only silences failures to create */
static void fail(const char *fmt, const char *arg) {
  fprintf(stderr, "mktemp: ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  exit(1);
}
int main(int argc, char **argv) {
  int i = 1, dir = 0, dry = 0, tflag = 0;
  const char *tmpdir = NULL, *suffix = NULL;
  int use_tmpdir = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--directory")) dir = 1;
    else if (!strcmp(a, "--dry-run")) dry = 1;
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strncmp(a, "--suffix=", 9)) suffix = a + 9;
    else if (!strcmp(a, "--tmpdir")) use_tmpdir = 1;
    else if (!strncmp(a, "--tmpdir=", 9)) { use_tmpdir = 1; tmpdir = a + 9; }
    else if (a[1] == '-') fail("unknown option '%s'", a);
    else for (char *f = a + 1; *f; f++) {
      if (*f == 'd') dir = 1;
      else if (*f == 'u') dry = 1;
      else if (*f == 'q') quiet = 1;
      else if (*f == 't') { tflag = 1; use_tmpdir = 1; }
      else if (*f == 'p') {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) fail("option requires an argument -- '%s'", "p");
        use_tmpdir = 1; tmpdir = v;
        break;
      } else { char o[2] = {*f, 0}; fail("invalid option -- '%s'", o); }
    }
  }
  if (argc - i > 1) fail("too many templates", NULL);
  const char *tmpl = i < argc ? argv[i] : NULL;
  if (!tmpl) { tmpl = "tmp.XXXXXXXXXX"; use_tmpdir = 1; }
  if (use_tmpdir) {
    if (!tmpdir || !*tmpdir) {
      const char *env = getenv("TMPDIR");
      tmpdir = env && *env ? env : "/tmp";
      if (tflag && i >= argc) {}
    }
    if (strchr(tmpl, '/') && (tflag || i < argc)) fail("invalid template, '%s', contains directory separator", tmpl);
  }
  size_t slen = suffix ? strlen(suffix) : 0;
  char *name = malloc((tmpdir ? strlen(tmpdir) : 0) + strlen(tmpl) + slen + 3);
  if (use_tmpdir) sprintf(name, "%s/%s", tmpdir, tmpl); else strcpy(name, tmpl);
  if (suffix) {
    if (strchr(suffix, '/')) fail("invalid suffix '%s', contains directory separator", suffix);
    strcat(name, suffix);
  } else {
    /* GNU: text after the last run of X's is a suffix when the template has no --suffix */
    char *last = strrchr(name, 'X');
    if (last) slen = strlen(last + 1);
    if (strchr(name + strlen(name) - slen, '/')) slen = 0;
  }
  size_t len = strlen(name), end = len - slen, start = end;
  while (start > 0 && name[start - 1] == 'X') start--;
  if (end - start < 3) fail("too few X's in template '%s'", tmpl);
  static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int tries = 0; tries < 1000; tries++) {
    unsigned char r[256];
    size_t nx = end - start;
    if (nx > sizeof r) nx = sizeof r;
    if (getrandom(r, nx, 0) != (ssize_t)nx) fail("cannot read random bytes: %s", strerror(errno));
    for (size_t k = 0; k < nx; k++) name[start + k] = set[r[k] % (sizeof set - 1)];
    if (dry) {
      struct stat st;
      if (lstat(name, &st) == 0) continue;
      puts(name);
      return 0;
    }
    int fd = -1;
    if (dir ? mkdir(name, 0700) == 0 : (fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600)) >= 0) {
      if (fd >= 0) close(fd);
      puts(name);
      return fflush(stdout) ? 1 : 0;
    }
    if (errno != EEXIST) {
      if (!quiet) fprintf(stderr, "mktemp: failed to create %s via template '%s': %s\n", dir ? "directory" : "file", tmpl, strerror(errno));
      return 1;
    }
  }
  if (!quiet) fprintf(stderr, "mktemp: cannot create a unique name from template '%s'\n", tmpl);
  return 1;
}
