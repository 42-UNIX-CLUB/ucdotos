// SPDX-License-Identifier: GPL-2.0-or-later
/* ln [-sfn] target [name]   ln [-sfn] target... directory */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
static int sym, force, nodir;
static int make(const char *target, const char *name) {
  if (force) unlink(name);
  int r = sym ? symlink(target, name) : link(target, name);
  if (r) fprintf(stderr, "ln: cannot create %s link '%s' to '%s': %s\n", sym ? "symbolic" : "hard", name, target, strerror(errno));
  return r != 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 's') sym = 1;
      else if (*f == 'f') force = 1;
      else if (*f == 'n' || *f == 'T') nodir = 1;
      else if (*f != 'v') { fprintf(stderr, "usage: ln [-sfn] target [name]\n"); return 1; }
    }
  }
  int n = argc - i;
  if (n < 1) { fprintf(stderr, "usage: ln [-sfn] target [name]\n"); return 1; }
  struct stat st;
  const char *last = argv[argc - 1];
  int todir = n >= 2 && !nodir && stat(last, &st) == 0 && S_ISDIR(st.st_mode);
  if (n > 2 && !todir) { fprintf(stderr, "ln: target '%s' is not a directory\n", last); return 1; }
  if (n >= 2 && !todir) return make(argv[i], last);
  const char *dir = n == 1 ? "." : last;
  for (int k = i; k < (n == 1 ? argc : argc - 1); k++) {
    const char *b = strrchr(argv[k], '/');
    b = b ? b + 1 : argv[k];
    char *name = malloc(strlen(dir) + strlen(b) + 2);
    sprintf(name, "%s/%s", dir, b);
    rc |= make(argv[k], name);
    free(name);
  }
  return rc;
}
