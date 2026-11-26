// SPDX-License-Identifier: GPL-2.0-or-later
/* chmod [-R] MODE file...   MODE: octal (755) or symbolic (u+x, go-w, a=r, +x, u+s, +t, ...) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
static int recursive;
static const char *spec;
/* Apply a symbolic or octal mode to old; -1 if spec is malformed. */
static long apply(const char *s, unsigned old, int isdir) {
  if (*s >= '0' && *s <= '7') {
    char *end;
    long v = strtol(s, &end, 8);
    return *end || v > 07777 ? -1 : v;
  }
  unsigned m = old & 07777, um = umask(0);
  umask(um);
  while (*s) {
    unsigned who = 0;
    for (; *s && strchr("ugoa", *s); s++) who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
    int implicit = !who;
    if (!who) who = 07777;
    if (!*s || !strchr("+-=", *s)) return -1;
    while (*s && strchr("+-=", *s)) {
      char op = *s++;
      unsigned bits = 0;
      for (; *s && strchr("rwxXst", *s); s++) {
        if (*s == 'r') bits |= 0444; else if (*s == 'w') bits |= 0222; else if (*s == 'x') bits |= 0111;
        else if (*s == 'X') { if (isdir || (old & 0111)) bits |= 0111; }
        else if (*s == 's') bits |= 06000; else if (*s == 't') bits |= 01000;
      }
      bits &= who;
      if (implicit) bits &= ~um | 07000;
      if (op == '+') m |= bits;
      else if (op == '-') m &= ~bits;
      else m = (m & ~(who & 0777)) | bits;
    }
    if (*s == ',') s++;
    else if (*s) return -1;
  }
  return m;
}
static int change(const char *path) {
  struct stat st;
  if (stat(path, &st)) { fprintf(stderr, "chmod: cannot access '%s': %s\n", path, strerror(errno)); return 1; }
  long m = apply(spec, st.st_mode, S_ISDIR(st.st_mode));
  if (m < 0) { fprintf(stderr, "chmod: invalid mode: '%s'\n", spec); exit(1); }
  int rc = 0;
  if (chmod(path, m)) { fprintf(stderr, "chmod: changing permissions of '%s': %s\n", path, strerror(errno)); rc = 1; }
  if (recursive && S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_type == DT_LNK) continue;
      char *p = malloc(strlen(path) + strlen(e->d_name) + 2);
      sprintf(p, "%s/%s", path, e->d_name);
      rc |= change(p);
      free(p);
    }
    if (d) closedir(d);
  }
  return rc;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  if (i < argc && !strcmp(argv[i], "-R")) { recursive = 1; i++; }
  if (argc - i < 2) { fprintf(stderr, "usage: chmod [-R] MODE file...\n"); return 1; }
  spec = argv[i++];
  for (; i < argc; i++) rc |= change(argv[i]);
  return rc;
}
