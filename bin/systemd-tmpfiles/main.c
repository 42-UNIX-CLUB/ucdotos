// SPDX-License-Identifier: GPL-2.0-or-later
/* systemd-tmpfiles: create the files and directories that tmpfiles.d(5) lines name.
 * The line types Debian packages were found to use (docs/SERVICES.md: d D x L L+ C f p)
 * and a few close relatives are handled; the rest is accepted and does nothing:
 *   d D v q Q   make the directory (D: emptied by --remove)
 *   f F w       create (F: truncate) a file and write the argument; w: write if it exists
 *   L L+        symlink (only when the kernel has symlinks; else a warning)
 *   C           copy the argument (or /usr/share/factory/PATH) if PATH is missing
 *   r R         remove (R: with contents), with --remove
 *   p           FIFO: needs mknod, which the kernel on main lacks: a warning
 *   x X e z Z t T a h   ages, modes, owners, attributes: nothing to do on UC.OS
 * Lines from a configuration file inside the Debian root apply inside that root. */
#include "../../libsvc/svc.h"
#include "../../libsvc/os.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int do_create, do_remove, do_boot, nerr;
static char prefixes[8][200], excludes[8][200];
static int nprefix, nexclude;
static const char *const dirs[] = {"/etc/tmpfiles.d", "/run/tmpfiles.d", "/usr/local/lib/tmpfiles.d", "/usr/lib/tmpfiles.d", 0};
static char roots[2][200];
static int nroots = 1;

static void warn(const char *f, int line, const char *msg, const char *arg) { fprintf(stderr, "%s:%d: %s%s\n", f, line, msg, arg ? arg : ""); }

static int wanted(const char *p) {
  for (int i = 0; i < nexclude; i++) if (!strncmp(p, excludes[i], strlen(excludes[i]))) return 0;
  if (!nprefix) return 1;
  for (int i = 0; i < nprefix; i++) if (!strncmp(p, prefixes[i], strlen(prefixes[i]))) return 1;
  return 0;
}
static void rmrf(const char *p, int depth, int self) {
  DIR *d = opendir(p);
  if (d && depth < 16) {
    struct dirent *de;
    while ((de = readdir(d))) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      char c[600];
      snprintf(c, sizeof c, "%s/%s", p, de->d_name);
      struct stat st;
      if (!svc_is_symlink(c) && stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(c, depth + 1, 1);
      else unlink(c);
    }
    closedir(d);
  }
  if (self) { if (rmdir(p) < 0) unlink(p); }
}
static int copy_file(const char *from, const char *to) {
  int in = open(from, O_RDONLY | O_CLOEXEC);
  if (in < 0) return -1;
  int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (out < 0) { close(in); return -1; }
  char b[4096];
  ssize_t r;
  while ((r = read(in, b, sizeof b)) > 0) if (write(out, b, r) != r) break;
  close(in); close(out);
  return 0;
}

static void apply(const char *file, int line, int root, char *type, const char *path0, const char *arg) {
  int boot_only = strchr(type, '!') != 0, ignore = strchr(type, '-') != 0, plus = strchr(type, '+') != 0;
  if (boot_only && !do_boot) return;
  char path[400];
  snprintf(path, sizeof path, "%s%s", roots[root], path0);
  if (!wanted(path0)) return;
  struct stat st;
  switch (type[0]) {
  case 'd': case 'D': case 'v': case 'q': case 'Q':
    if (do_create && svc_mkdir_p(path) < 0 && !ignore) { warn(file, line, "cannot create directory ", path); nerr++; }
    if (do_remove && type[0] == 'D') rmrf(path, 0, 0);
    break;
  case 'f': case 'F': case 'w': {
    if (!do_create) break;
    int exists = stat(path, &st) == 0;
    if (type[0] == 'w' && !exists) break;
    if (type[0] == 'f' && exists) break;
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (type[0] == 'w' && plus ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) { if (!ignore) { warn(file, line, "cannot create file ", path); nerr++; } break; }
    if (arg && *arg && strcmp(arg, "-")) {
      char *x = svc_specifiers(arg, 0, 0);
      char *o = x, *w = x;
      for (; *o; o++) { if (*o == '\\' && o[1] == 'n') { *w++ = '\n'; o++; } else *w++ = *o; } /* the common escape */
      *w = 0;
      if (write(fd, x, strlen(x)) < 0) {}
      free(x);
    }
    close(fd);
    break;
  }
  case 'L': {
    if (!do_create) break;
    char target[400];
    if (arg && *arg && strcmp(arg, "-")) snprintf(target, sizeof target, "%s", arg);
    else snprintf(target, sizeof target, "/usr/share/factory%s", path0);
    if (svc_readlink(path, (char[300]){0}, 300) >= 0 && !plus) break;
    if (plus) { if (svc_is_symlink(path)) unlink(path); else rmrf(path, 0, 1); }
    int r = svc_symlink(target, path);
    if (r == -ENOSYS) { warn(file, line, "symlinks are not available on this kernel; not created: ", path); break; }
    if (r < 0 && r != -EEXIST && !ignore) { warn(file, line, "cannot create symlink ", path); nerr++; }
    break;
  }
  case 'C': {
    if (!do_create || stat(path, &st) == 0) break;
    char src[400];
    if (arg && *arg && strcmp(arg, "-")) snprintf(src, sizeof src, "%s%s", roots[root], arg);
    else snprintf(src, sizeof src, "%s/usr/share/factory%s", roots[root], path0);
    if (stat(src, &st) < 0) { if (!ignore) warn(file, line, "nothing to copy from ", src); break; }
    if (S_ISDIR(st.st_mode)) {
      svc_mkdir_p(path);
      DIR *d = opendir(src);
      struct dirent *de;
      while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char a[700], b[700];
        snprintf(a, sizeof a, "%s/%s", src, de->d_name);
        snprintf(b, sizeof b, "%s/%s", path, de->d_name);
        copy_file(a, b);
      }
      if (d) closedir(d);
    } else copy_file(src, path);
    break;
  }
  case 'r': case 'R':
    if (do_remove) { if (type[0] == 'R') rmrf(path, 0, 1); else if (rmdir(path) < 0) unlink(path); }
    break;
  case 'p':
    if (do_create && stat(path, &st) < 0) warn(file, line, "FIFOs cannot be made on this kernel; not created: ", path);
    break;
  case 'x': case 'X': case 'e': case 'z': case 'Z': case 't': case 'T': case 'a': case 'A': case 'h': case 'H': case 'b': case 'c':
    break; /* nothing to do: no ages, modes, owners, attributes or device nodes here */
  default:
    warn(file, line, "unknown line type ", type);
    nerr++;
  }
}

static void run_file(const char *path, int root) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "Failed to open '%s': No such file or directory\n", path); nerr++; return; }
  char line[1024];
  int ln = 0;
  while (fgets(line, sizeof line, f)) {
    ln++;
    char *x = svc_specifiers(line, 0, 0);
    char **w;
    int n = svc_words(x, &w);
    free(x);
    if (n >= 2 && w[0][0] != '#') apply(path, ln, root, w[0], w[1], n >= 7 ? w[6] : 0);
    svc_free_words(w, n < 0 ? 0 : n);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  const char *cfg[64];
  int ncfg = 0;
  roots[0][0] = 0;
  struct stat st;
  if (stat("/debian", &st) == 0 && S_ISDIR(st.st_mode)) { strcpy(roots[1], "/debian"); nroots = 2; }
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--create")) do_create = 1;
    else if (!strcmp(a, "--remove")) do_remove = 1;
    else if (!strcmp(a, "--boot")) do_boot = 1;
    else if (!strcmp(a, "--clean") || !strcmp(a, "--purge") || !strcmp(a, "--graceful") || !strcmp(a, "--user")) {}
    else if (!strncmp(a, "--prefix=", 9) && nprefix < 8) snprintf(prefixes[nprefix++], 200, "%s", a + 9);
    else if (!strcmp(a, "--prefix") && i + 1 < argc && nprefix < 8) snprintf(prefixes[nprefix++], 200, "%s", argv[++i]);
    else if (!strncmp(a, "--exclude-prefix=", 17) && nexclude < 8) snprintf(excludes[nexclude++], 200, "%s", a + 17);
    else if (!strcmp(a, "--exclude-prefix") && i + 1 < argc && nexclude < 8) snprintf(excludes[nexclude++], 200, "%s", argv[++i]);
    else if (!strncmp(a, "--root=", 7)) { snprintf(roots[0], 200, "%s", strcmp(a + 7, "/") ? a + 7 : ""); nroots = 1; }
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { printf("systemd-tmpfiles [--create] [--remove] [--boot] [--prefix=P] [--exclude-prefix=P] [--root=R] [CONFIG...]\n"); return 0; }
    else if (a[0] == '-' && a[1]) { fprintf(stderr, "systemd-tmpfiles: unrecognized option '%s'\n", a); return 1; }
    else if (ncfg < 64) cfg[ncfg++] = a;
  }
  if (!do_create && !do_remove) { fprintf(stderr, "You need to specify at least one of --clean, --create, --remove, or --purge.\n"); return 1; }
  if (ncfg) {
    for (int i = 0; i < ncfg; i++) {
      if (cfg[i][0] == '/') { run_file(cfg[i], 0); continue; }
      int done = 0;
      for (int r = 0; r < nroots && !done; r++)
        for (int d = 0; dirs[d] && !done; d++) {
          char p[500];
          snprintf(p, sizeof p, "%s%s/%s", roots[r], dirs[d], cfg[i]);
          if (stat(p, &st) == 0) { run_file(p, r); done = 1; }
        }
      if (!done) { fprintf(stderr, "Failed to find configuration file %s\n", cfg[i]); nerr++; }
    }
  } else {
    for (int r = 0; r < nroots; r++) {
      char *seen[256];
      int ns = 0;
      for (int d = 0; dirs[d]; d++) {
        char dir[400];
        snprintf(dir, sizeof dir, "%s%s", roots[r], dirs[d]);
        DIR *dd = opendir(dir);
        struct dirent *de;
        while (dd && (de = readdir(dd))) {
          size_t l = strlen(de->d_name);
          if (l < 6 || strcmp(de->d_name + l - 5, ".conf")) continue;
          int dup = 0;
          for (int k = 0; k < ns; k++) if (!strcmp(seen[k], de->d_name)) dup = 1;
          if (dup || ns >= 256) continue;
          seen[ns++] = strdup(de->d_name);
          char p[700];
          snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
          run_file(p, r);
        }
        if (dd) closedir(dd);
      }
      for (int k = 0; k < ns; k++) free(seen[k]);
    }
  }
  return nerr ? 1 : 0;
}
