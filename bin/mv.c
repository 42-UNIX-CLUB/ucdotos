// SPDX-License-Identifier: GPL-2.0-or-later
/* mv src... dst: rename(2) through SYS_renameat */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
static int do_rename(const char *a, const char *b) { return __sysret(__syscall4(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b)); }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (strspn(argv[i] + 1, "fvin") != strlen(argv[i] + 1)) { fprintf(stderr, "usage: mv src... dst\n"); return 1; }
  }
  if (argc - i < 2) { fprintf(stderr, "usage: mv src... dst\n"); return 1; }
  const char *dst = argv[argc - 1];
  int todir = is_dir(dst);
  if (argc - i > 2 && !todir) { fprintf(stderr, "mv: target '%s' is not a directory\n", dst); return 1; }
  for (; i < argc - 1; i++) {
    char *src = argv[i], *t = NULL;
    size_t n = strlen(src);
    while (n > 1 && src[n - 1] == '/') src[--n] = 0;
    if (todir) {
      const char *b = strrchr(src, '/');
      b = b ? b + 1 : src;
      t = malloc(strlen(dst) + strlen(b) + 2);
      sprintf(t, "%s/%s", dst, b);
    }
    if (do_rename(src, t ? t : dst) != 0) { fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", src, t ? t : dst, strerror(errno)); rc = 1; }
    free(t);
  }
  return rc;
}
