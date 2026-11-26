// SPDX-License-Identifier: GPL-2.0-or-later
/* install [-cCDpsTv] [-m MODE] [-o OWNER] [-g GROUP] SRC DEST
 * install [...] SRC... DIR  |  install [...] -t DIR SRC...  |  install -d [...] DIR...
 * Copy files and set their mode (default 0755), owner and group. -D creates
 * the leading directories of DEST, -d creates directories, -p keeps the
 * times, -C leaves an identical destination alone, -s is accepted and does
 * nothing (there is no strip for WebAssembly). If the kernel cannot set modes
 * or owners (ENOSYS), install says so once and carries on. */
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

static mode_t mode = 0755;
static const char *owner, *group;
static int mkdirs_mode, leading, preserve, verbose, compare, no_target_dir;
static long uid = -1, gid = -1;
static int warned, rc;

static void enosys_warn(void) {
  if (!warned) fprintf(stderr, "install: warning: this system cannot set modes or owners yet; continuing without them\n");
  warned = 1;
}
static int set_attrs(const char *path) {
  if (chmod(path, mode) != 0) {
    if (errno == ENOSYS) enosys_warn();
    else { fprintf(stderr, "install: cannot change permissions of '%s': %s\n", path, strerror(errno)); return -1; }
  }
  if (uid >= 0 || gid >= 0) {
    if (chown(path, uid >= 0 ? (uid_t)uid : (uid_t)-1, gid >= 0 ? (gid_t)gid : (gid_t)-1) != 0) {
      if (errno == ENOSYS) enosys_warn();
      else { fprintf(stderr, "install: cannot change ownership of '%s': %s\n", path, strerror(errno)); return -1; }
    }
  }
  return 0;
}

/* symbolic or octal mode, relative to `base` (for symbolic forms) */
static int parse_mode(const char *s, mode_t base, mode_t *out) {
  if (*s >= '0' && *s <= '7') {
    char *e;
    long v = strtol(s, &e, 8);
    if (*e || v > 07777) return -1;
    *out = v;
    return 0;
  }
  mode_t m = base;
  const char *p = s;
  while (*p) {
    int who = 0;
    while (*p && strchr("ugoa", *p)) {
      who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
      p++;
    }
    if (!who) who = 07777;
    if (!*p || !strchr("+-=", *p)) return -1;
    while (*p && strchr("+-=", *p)) {
      int op = *p++, bits = 0;
      while (*p && strchr("rwxXst", *p)) {
        switch (*p++) {
        case 'r': bits |= 0444; break;
        case 'w': bits |= 0222; break;
        case 'x': bits |= 0111; break;
        case 'X': if ((m & 0111) || S_ISDIR(base)) bits |= 0111; break;
        case 's': bits |= 06000; break;
        case 't': bits |= 01000; break;
        }
      }
      bits &= who;
      if (op == '+') m |= bits;
      else if (op == '-') m &= ~bits;
      else m = (m & ~(who & 07777)) | bits;
    }
    if (*p == ',') p++;
    else if (*p) return -1;
  }
  *out = m & 07777;
  return 0;
}

static long lookup_id(const char *file, const char *name) {
  char *e;
  long v = strtol(name, &e, 10);
  if (*name && !*e) return v;
  FILE *f = fopen(file, "r");
  char line[1024];
  size_t nl = strlen(name);
  while (f && fgets(line, sizeof line, f)) {
    if (!strncmp(line, name, nl) && line[nl] == ':') {
      char *c = strchr(line + nl + 1, ':');
      if (c) { fclose(f); return atol(c + 1); }
    }
  }
  if (f) fclose(f);
  return -2;
}

static int make_dirs(const char *path, int report) {
  char *p = strdup(path);
  size_t len = strlen(p);
  while (len > 1 && p[len - 1] == '/') p[--len] = 0;
  for (char *s = p + 1; ; s++) {
    if (*s == '/' || !*s) {
      char c = *s;
      *s = 0;
      struct stat st;
      if (stat(p, &st) != 0) {
        if (mkdir(p, 0755) != 0 && errno != EEXIST) { fprintf(stderr, "install: cannot create directory '%s': %s\n", p, strerror(errno)); free(p); return -1; }
        if (verbose && report) printf("install: creating directory '%s'\n", p);
      } else if (!S_ISDIR(st.st_mode)) { fprintf(stderr, "install: cannot create directory '%s': File exists\n", p); free(p); return -1; }
      if (!c) break;
      *s = c;
    }
  }
  free(p);
  return 0;
}

static int same_contents(const char *a, const char *b) {
  FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
  int same = x && y;
  while (same) {
    int c = getc(x), d = getc(y);
    if (c != d) same = 0;
    if (c == EOF || d == EOF) break;
  }
  if (x) fclose(x);
  if (y) fclose(y);
  return same;
}

static int copy_file(const char *src, const char *dst) {
  struct stat ss, ds;
  if (stat(src, &ss) != 0) { fprintf(stderr, "install: cannot stat '%s': %s\n", src, strerror(errno)); return -1; }
  if (S_ISDIR(ss.st_mode)) { fprintf(stderr, "install: omitting directory '%s'\n", src); return -1; }
  if (stat(dst, &ds) == 0) {
    if (ds.st_dev == ss.st_dev && ds.st_ino == ss.st_ino) { fprintf(stderr, "install: '%s' and '%s' are the same file\n", src, dst); return -1; }
    if (compare && S_ISREG(ds.st_mode) && (ds.st_mode & 07777) == mode && ds.st_size == ss.st_size && same_contents(src, dst)) return 0;
  }
  int in = open(src, O_RDONLY);
  if (in < 0) { fprintf(stderr, "install: cannot open '%s' for reading: %s\n", src, strerror(errno)); return -1; }
  unlink(dst); /* a new file, as install does: running programs keep the old one */
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (out < 0) { fprintf(stderr, "install: cannot create regular file '%s': %s\n", dst, strerror(errno)); close(in); return -1; }
  char buf[65536];
  long n;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, n) != n) { fprintf(stderr, "install: error writing '%s': %s\n", dst, strerror(errno)); close(in); close(out); return -1; }
  if (n < 0) { fprintf(stderr, "install: error reading '%s': %s\n", src, strerror(errno)); close(in); close(out); return -1; }
  close(in);
  if (close(out) != 0) { fprintf(stderr, "install: error writing '%s': %s\n", dst, strerror(errno)); return -1; }
  if (verbose) printf("'%s' -> '%s'\n", src, dst);
  if (preserve) {
    struct timespec ts[2] = {{ss.st_atime, 0}, {ss.st_mtime, 0}};
#ifdef __wasm__
    long r = __syscall4(SYS_utimensat, AT_FDCWD, dst, ts, 0);
    if (r < 0 && r != -ENOSYS) fprintf(stderr, "install: cannot set times of '%s': %s\n", dst, strerror(-r));
#else
    if (utimensat(AT_FDCWD, dst, ts, 0) != 0) fprintf(stderr, "install: cannot set times of '%s': %s\n", dst, strerror(errno));
#endif
  }
  return set_attrs(dst);
}

static char *join(const char *dir, const char *src) {
  const char *b = strrchr(src, '/');
  b = b ? b + 1 : src;
  char *p = malloc(strlen(dir) + strlen(b) + 2);
  sprintf(p, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", b);
  return p;
}

int main(int argc, char **argv) {
  int i = 1;
  const char *target = NULL, *mode_s = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (a[1] == '-') {
      if (!strcmp(a, "--directory")) mkdirs_mode = 1;
      else if (!strcmp(a, "--preserve-timestamps")) preserve = 1;
      else if (!strcmp(a, "--verbose")) verbose = 1;
      else if (!strcmp(a, "--compare")) compare = 1;
      else if (!strcmp(a, "--no-target-directory")) no_target_dir = 1;
      else if (!strcmp(a, "--strip")) ;
      else if (!strncmp(a, "--mode=", 7)) mode_s = a + 7;
      else if (!strncmp(a, "--owner=", 8)) owner = a + 8;
      else if (!strncmp(a, "--group=", 8)) group = a + 8;
      else if (!strncmp(a, "--target-directory=", 19)) target = a + 19;
      else { fprintf(stderr, "install: unrecognized option '%s'\n", a); return 1; }
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      switch (*f) {
      case 'c': case 's': break;
      case 'C': compare = 1; break;
      case 'd': mkdirs_mode = 1; break;
      case 'D': leading = 1; break;
      case 'p': preserve = 1; break;
      case 'v': verbose = 1; break;
      case 'T': no_target_dir = 1; break;
      case 'm': case 'o': case 'g': case 't': case 'S': {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "install: option requires an argument -- '%c'\n", *f); return 1; }
        if (*f == 'm') mode_s = v; else if (*f == 'o') owner = v; else if (*f == 'g') group = v; else if (*f == 't') target = v;
        f += strlen(f) - 1;
        break;
      }
      default: fprintf(stderr, "install: invalid option -- '%c'\nusage: install [-cCDpsTv] [-m mode] [-o owner] [-g group] src... dest\n", *f); return 1;
      }
    }
  }
  if (mode_s && parse_mode(mode_s, 0, &mode) != 0) /* symbolic modes start from 0, as in GNU install */ { fprintf(stderr, "install: invalid mode '%s'\n", mode_s); return 1; }
  if (owner && (uid = lookup_id("/etc/passwd", owner)) == -2) { fprintf(stderr, "install: invalid user '%s'\n", owner); return 1; }
  if (group && (gid = lookup_id("/etc/group", group)) == -2) { fprintf(stderr, "install: invalid group '%s'\n", group); return 1; }
  setvbuf(stdout, NULL, _IOLBF, 0);
  int n = argc - i;
  if (mkdirs_mode) {
    if (n < 1) { fprintf(stderr, "install: missing operand\n"); return 1; }
    for (; i < argc; i++) {
      if (make_dirs(argv[i], 1) != 0) { rc = 1; continue; }
      if (set_attrs(argv[i]) != 0) rc = 1;
    }
    return rc;
  }
  if (target) {
    if (no_target_dir) { fprintf(stderr, "install: cannot combine --target-directory (-t) and --no-target-directory (-T)\n"); return 1; }
    if (n < 1) { fprintf(stderr, "install: missing file operand\n"); return 1; }
    if (leading && make_dirs(target, 1) != 0) return 1;
    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "install: target '%s' is not a directory\n", target); return 1; }
    for (; i < argc; i++) { char *d = join(target, argv[i]); if (copy_file(argv[i], d) != 0) rc = 1; free(d); }
    return rc;
  }
  if (n < 1) { fprintf(stderr, "install: missing file operand\n"); return 1; }
  if (n < 2) { fprintf(stderr, "install: missing destination file operand after '%s'\n", argv[i]); return 1; }
  const char *dest = argv[argc - 1];
  struct stat st;
  int isdir = !no_target_dir && stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
  if (n == 2 && !isdir) {
    if (leading) {
      char *d = strdup(dest), *slash = strrchr(d, '/');
      if (slash && slash != d) { *slash = 0; if (make_dirs(d, 1) != 0) return 1; }
      free(d);
    }
    return copy_file(argv[i], dest) != 0;
  }
  if (!isdir) {
    if (no_target_dir) { fprintf(stderr, "install: extra operand '%s'\n", argv[i + 2]); return 1; }
    fprintf(stderr, "install: target '%s' is not a directory\n", dest);
    return 1;
  }
  for (; i < argc - 1; i++) { char *d = join(dest, argv[i]); if (copy_file(argv[i], d) != 0) rc = 1; free(d); }
  return rc;
}
