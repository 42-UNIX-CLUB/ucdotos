// SPDX-License-Identifier: GPL-2.0-or-later
/* rm [-rf] file... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
static int recursive, force;
static int rm(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    if (force && errno == ENOENT) return 0;
    fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
    return 1;
  }
  if (S_ISDIR(st.st_mode)) {
    if (!recursive) { fprintf(stderr, "rm: cannot remove '%s': Is a directory\n", path); return 1; }
    int rc = 0;
    DIR *d = opendir(path);
    if (d) {
      /* collect names first: removing while reading would skip entries */
      char **names = NULL;
      int n = 0;
      struct dirent *e;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names = realloc(names, (n + 1) * sizeof *names);
        names[n] = malloc(strlen(path) + strlen(e->d_name) + 2);
        sprintf(names[n++], "%s/%s", path, e->d_name);
      }
      closedir(d);
      for (int i = 0; i < n; i++) { rc |= rm(names[i]); free(names[i]); }
      free(names);
    }
    if (rmdir(path) != 0) { fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno)); return 1; }
    return rc;
  }
  if (unlink(path) != 0) { fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno)); return 1; }
  return 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'r' || *f == 'R') recursive = 1;
      else if (*f == 'f') force = 1;
      else if (*f != 'v' && *f != 'i') { fprintf(stderr, "usage: rm [-rf] file...\n"); return 1; }
    }
  }
  if (i >= argc && !force) { fprintf(stderr, "usage: rm [-rf] file...\n"); return 1; }
  for (; i < argc; i++) {
    const char *b = strrchr(argv[i], '/');
    b = b ? b + 1 : argv[i];
    if (!strcmp(b, ".") || !strcmp(b, "..") || !strcmp(argv[i], "/")) { fprintf(stderr, "rm: refusing to remove '%s'\n", argv[i]); rc = 1; continue; }
    rc |= rm(argv[i]);
  }
  return rc;
}
