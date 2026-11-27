// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared by su and sudo (bin/su.c, bin/sudo.c): the refusal message and the switch. */
#ifndef SUID_H
#define SUID_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
extern char **environ;
static void refuse(const char *me) {
  fprintf(stderr, "%s: this session is not authorised for administration\n", me);
}
/* groups, then group, then user: the order in which privileges can still be dropped */
static int become(const struct passwd *pw, const char *me) {
  if (initgroups(pw->pw_name, pw->pw_gid) && geteuid() == 0) { fprintf(stderr, "%s: cannot set groups: %s\n", me, strerror(errno)); return -1; }
  if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid)) { fprintf(stderr, "%s: cannot set group %u: %s\n", me, pw->pw_gid, strerror(errno)); return -1; }
  if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid)) { fprintf(stderr, "%s: cannot set user %u: %s\n", me, pw->pw_uid, strerror(errno)); return -1; }
  return 0;
}
/* drop every variable except the named ones (a login shell starts clean) */
static void clearenv_keep(const char *a, const char *b, const char *c, const char *d, const char *e, const char *f) {
  const char *keep[] = {a, b, c, d, e, f};
  static char *kept[7];
  int n = 0;
  for (int k = 0; k < 6; k++) {
    const char *v = getenv(keep[k]);
    if (!v) continue;
    size_t l = strlen(keep[k]) + strlen(v) + 2;
    kept[n] = malloc(l);
    snprintf(kept[n++], l, "%s=%s", keep[k], v);
  }
  kept[n] = NULL;
  environ = kept;
}
#endif
