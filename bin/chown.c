// SPDX-License-Identifier: GPL-2.0-or-later
/* chown [-R] [-h] OWNER[:GROUP] file...    chgrp [-R] [-h] GROUP file...
 * One program: it acts as chgrp when called by that name (bin/chgrp.c includes this file).
 * OWNER and GROUP are names (from /etc/passwd, /etc/group) or numbers. -h changes a
 * symbolic link itself; without it a link given on the command line is followed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#ifndef CHOWN_AS_CHGRP
#define CHOWN_AS_CHGRP 0
#endif
static int recursive, nofollow, rc;
static uid_t uid = (uid_t)-1;
static gid_t gid = (gid_t)-1;
static const char *me = CHOWN_AS_CHGRP ? "chgrp" : "chown";
static int number(const char *s, unsigned *out) {
  char *e;
  if (!*s) return -1;
  unsigned long v = strtoul(s, &e, 10);
  if (*e) return -1;
  *out = (unsigned)v;
  return 0;
}
static int lookup_user(const char *s) {
  struct passwd *p = getpwnam(s);
  unsigned v;
  if (p) uid = p->pw_uid; else if (!number(s, &v)) uid = v; else { fprintf(stderr, "%s: invalid user: '%s'\n", me, s); return -1; }
  return 0;
}
static int lookup_group(const char *s) {
  struct group *g = getgrnam(s);
  unsigned v;
  if (g) gid = g->gr_gid; else if (!number(s, &v)) gid = v; else { fprintf(stderr, "%s: invalid group: '%s'\n", me, s); return -1; }
  return 0;
}
static void change(const char *path, int top) {
  int flags = (nofollow || !top) ? AT_SYMLINK_NOFOLLOW : 0;
  if (fchownat(AT_FDCWD, path, uid, gid, flags)) {
    fprintf(stderr, "%s: changing ownership of '%s': %s\n", me, path, strerror(errno));
    rc = 1;
  }
  struct stat st;
  if (!recursive || lstat(path, &st) || !S_ISDIR(st.st_mode)) return;
  DIR *d = opendir(path);
  if (!d) { fprintf(stderr, "%s: cannot read directory '%s': %s\n", me, path, strerror(errno)); rc = 1; return; }
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    size_t n = strlen(path) + strlen(e->d_name) + 2;
    char *sub = malloc(n);
    snprintf(sub, n, "%s/%s", path, e->d_name);
    change(sub, 0);
    free(sub);
  }
  closedir(d);
}
int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (const char *o = argv[i] + 1; *o; o++) {
      if (*o == 'R') recursive = 1;
      else if (*o == 'h') nofollow = 1;
      else goto usage;
    }
  }
  if (argc - i < 2) goto usage;
  char *spec = argv[i++];
  if (CHOWN_AS_CHGRP) { if (lookup_group(spec)) return 1; }
  else {
    char *colon = strchr(spec, ':');
    if (!colon) colon = strchr(spec, '.'); /* the old form OWNER.GROUP */
    if (colon) *colon = 0;
    if (*spec && lookup_user(spec)) return 1;
    if (colon && colon[1] && lookup_group(colon + 1)) return 1;
    if (colon && !colon[1] && uid != (uid_t)-1) { struct passwd *p = getpwuid(uid); if (p) gid = p->pw_gid; } /* OWNER: means the owner's login group */
  }
  for (; i < argc; i++) change(argv[i], 1);
  return rc;
usage:
  fprintf(stderr, CHOWN_AS_CHGRP ? "usage: chgrp [-R] [-h] GROUP file...\n" : "usage: chown [-R] [-h] OWNER[:GROUP] file...\n");
  return 1;
}
