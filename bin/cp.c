// SPDX-License-Identifier: GPL-2.0-or-later
/* cp [-r] [-f] src... dst */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
static int recursive;
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) { fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno)); return 1; }
  struct stat st;
  fstat(in, &st);
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
  if (out < 0) { fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno)); close(in); return 1; }
  static char buf[65536];
  long n;
  int rc = 0;
  while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, n) != n) { fprintf(stderr, "cp: error writing '%s': %s\n", dst, strerror(errno)); rc = 1; break; }
  close(in);
  close(out);
  return rc;
}
static int copy(const char *src, const char *dst) {
  struct stat st;
  if (stat(src, &st) != 0) { fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno)); return 1; }
  if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
  if (!recursive) { fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src); return 1; }
  if (!is_dir(dst) && mkdir(dst, 0755) != 0) { fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno)); return 1; }
  DIR *d = opendir(src);
  if (!d) return 1;
  int rc = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char *s = malloc(strlen(src) + strlen(e->d_name) + 2), *t = malloc(strlen(dst) + strlen(e->d_name) + 2);
    sprintf(s, "%s/%s", src, e->d_name);
    sprintf(t, "%s/%s", dst, e->d_name);
    rc |= copy(s, t);
    free(s); free(t);
  }
  closedir(d);
  return rc;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'r' || *f == 'R' || *f == 'a') recursive = 1;
      else if (!strchr("fvip", *f)) { fprintf(stderr, "usage: cp [-r] src... dst\n"); return 1; }
    }
  }
  if (argc - i < 2) { fprintf(stderr, "usage: cp [-r] src... dst\n"); return 1; }
  const char *dst = argv[argc - 1];
  int todir = is_dir(dst);
  if (argc - i > 2 && !todir) { fprintf(stderr, "cp: target '%s' is not a directory\n", dst); return 1; }
  for (; i < argc - 1; i++) {
    if (todir) {
      char *src = argv[i];
      size_t n = strlen(src);
      while (n > 1 && src[n - 1] == '/') src[--n] = 0;
      const char *b = strrchr(src, '/');
      b = b ? b + 1 : src;
      char *t = malloc(strlen(dst) + strlen(b) + 2);
      sprintf(t, "%s/%s", dst, b);
      rc |= copy(src, t);
      free(t);
    } else rc |= copy(argv[i], dst);
  }
  return rc;
}
