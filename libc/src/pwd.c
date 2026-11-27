// SPDX-License-Identifier: GPL-2.0-or-later
/* /etc/passwd and /etc/group, read the way glibc's "files" source reads them: one entry
 * per line, fields separated by ':', lines that do not parse are skipped. The non-reentrant
 * functions share one static entry per database, as on Linux. There is no /etc/shadow:
 * UC.OS has no passwords (see bin/su.c). */
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define LINE_MAX_ 1024
#define MEM_MAX 64

/* split a line into at most n fields at ':'; returns the number found */
static int split(char *line, char **f, int n) {
  int k = 0;
  char *nl = strchr(line, '\n');
  if (nl) *nl = 0;
  f[k++] = line;
  for (char *p = line; *p && k < n; p++) if (*p == ':') { *p = 0; f[k++] = p + 1; }
  return k;
}
static int num(const char *s, unsigned *out) {
  if (!*s) return -1;
  unsigned v = 0;
  for (; *s; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (unsigned)(*s - '0'); }
  *out = v;
  return 0;
}

/* ---- passwd ---- */
static FILE *pwf;
static char pwline[LINE_MAX_];
static struct passwd pwent;
static int parse_pw(char *line, struct passwd *pw) {
  char *f[7];
  unsigned uid, gid;
  if (line[0] == '#' || split(line, f, 7) != 7 || num(f[2], &uid) || num(f[3], &gid)) return -1;
  pw->pw_name = f[0]; pw->pw_passwd = f[1]; pw->pw_uid = uid; pw->pw_gid = gid;
  pw->pw_gecos = f[4]; pw->pw_dir = f[5]; pw->pw_shell = f[6];
  return 0;
}
void setpwent(void) { if (pwf) rewind(pwf); }
void endpwent(void) { if (pwf) fclose(pwf); pwf = NULL; }
struct passwd *getpwent(void) {
  if (!pwf && !(pwf = fopen("/etc/passwd", "r"))) return NULL;
  while (fgets(pwline, sizeof pwline, pwf)) if (!parse_pw(pwline, &pwent)) return &pwent;
  return NULL;
}
/* find by name (name != NULL) or by uid; the entry lives in buf */
static int find_pw(const char *name, uid_t uid, struct passwd *pw, char *buf, size_t len) {
  FILE *f = fopen("/etc/passwd", "r");
  if (!f) return errno == ENOENT ? 0 : -1;
  char line[LINE_MAX_];
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line) + 1;
    char tmp[LINE_MAX_];
    memcpy(tmp, line, n);
    struct passwd p;
    if (parse_pw(tmp, &p)) continue;
    if (name ? strcmp(p.pw_name, name) : p.pw_uid != uid) continue;
    if (n > len) { fclose(f); errno = ERANGE; return -1; }
    memcpy(buf, line, n);
    parse_pw(buf, pw);
    found = 1;
    break;
  }
  fclose(f);
  return found;
}
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t len, struct passwd **out) {
  int r = find_pw(name, 0, pw, buf, len);
  *out = r > 0 ? pw : NULL;
  return r < 0 ? errno : 0;
}
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t len, struct passwd **out) {
  int r = find_pw(NULL, uid, pw, buf, len);
  *out = r > 0 ? pw : NULL;
  return r < 0 ? errno : 0;
}
struct passwd *getpwnam(const char *name) { struct passwd *o; getpwnam_r(name, &pwent, pwline, sizeof pwline, &o); return o; }
struct passwd *getpwuid(uid_t uid) { struct passwd *o; getpwuid_r(uid, &pwent, pwline, sizeof pwline, &o); return o; }

/* ---- group ---- */
static FILE *grf;
static char grline[LINE_MAX_];
static char *grmem[MEM_MAX + 1];
static struct group grent;
/* members go into mem (cap entries plus the NULL) */
static int parse_gr(char *line, struct group *g, char **mem, int cap) {
  char *f[4];
  unsigned gid;
  if (line[0] == '#' || split(line, f, 4) != 4 || num(f[2], &gid)) return -1;
  g->gr_name = f[0]; g->gr_passwd = f[1]; g->gr_gid = gid; g->gr_mem = mem;
  int k = 0;
  for (char *p = f[3]; *p && k < cap;) {
    mem[k++] = p;
    char *c = strchr(p, ',');
    if (!c) break;
    *c = 0; p = c + 1;
  }
  mem[k] = NULL;
  return 0;
}
void setgrent(void) { if (grf) rewind(grf); }
void endgrent(void) { if (grf) fclose(grf); grf = NULL; }
struct group *getgrent(void) {
  if (!grf && !(grf = fopen("/etc/group", "r"))) return NULL;
  while (fgets(grline, sizeof grline, grf)) if (!parse_gr(grline, &grent, grmem, MEM_MAX)) return &grent;
  return NULL;
}
/* by name or gid; the line and the member pointers live in buf (pointers first, aligned) */
static int find_gr(const char *name, gid_t gid, struct group *g, char *buf, size_t len) {
  FILE *f = fopen("/etc/group", "r");
  if (!f) return errno == ENOENT ? 0 : -1;
  char line[LINE_MAX_];
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line) + 1;
    char tmp[LINE_MAX_], *m[MEM_MAX + 1];
    memcpy(tmp, line, n);
    struct group t;
    if (parse_gr(tmp, &t, m, MEM_MAX)) continue;
    if (name ? strcmp(t.gr_name, name) : t.gr_gid != gid) continue;
    int nm = 0;
    while (m[nm]) nm++;
    size_t ptrs = (size_t)(nm + 1) * sizeof(char *);
    size_t pad = (sizeof(char *) - ((size_t)buf % sizeof(char *))) % sizeof(char *);
    if (pad + ptrs + n > len) { fclose(f); errno = ERANGE; return -1; }
    char **mem = (char **)(buf + pad);
    char *text = buf + pad + ptrs;
    memcpy(text, line, n);
    parse_gr(text, g, mem, nm);
    found = 1;
    break;
  }
  fclose(f);
  return found;
}
int getgrnam_r(const char *name, struct group *g, char *buf, size_t len, struct group **out) {
  int r = find_gr(name, 0, g, buf, len);
  *out = r > 0 ? g : NULL;
  return r < 0 ? errno : 0;
}
int getgrgid_r(gid_t gid, struct group *g, char *buf, size_t len, struct group **out) {
  int r = find_gr(NULL, gid, g, buf, len);
  *out = r > 0 ? g : NULL;
  return r < 0 ? errno : 0;
}
static char grbuf[LINE_MAX_ + (MEM_MAX + 2) * sizeof(char *)];
struct group *getgrnam(const char *name) { struct group *o; getgrnam_r(name, &grent, grbuf, sizeof grbuf, &o); return o; }
struct group *getgrgid(gid_t gid) { struct group *o; getgrgid_r(gid, &grent, grbuf, sizeof grbuf, &o); return o; }

/* the user's groups: `group` first, then every group that lists the user as a member */
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups) {
  int n = 0, cap = *ngroups;
  if (n < cap) groups[n] = group;
  n++;
  FILE *f = fopen("/etc/group", "r");
  if (f) {
    char line[LINE_MAX_], *m[MEM_MAX + 1];
    struct group g;
    while (fgets(line, sizeof line, f)) {
      if (parse_gr(line, &g, m, MEM_MAX) || g.gr_gid == group) continue;
      for (int k = 0; m[k]; k++) {
        if (strcmp(m[k], user)) continue;
        int dup = 0;
        for (int j = 0; j < n && j < cap; j++) if (groups[j] == g.gr_gid) dup = 1;
        if (!dup) { if (n < cap) groups[n] = g.gr_gid; n++; }
        break;
      }
    }
    fclose(f);
  }
  *ngroups = n;
  return n > cap ? -1 : n;
}
int initgroups(const char *user, gid_t group) {
  gid_t g[16];
  int n = 16;
  if (getgrouplist(user, group, g, &n) < 0) n = 16; /* the kernel keeps at most 16 */
  return setgroups((size_t)n, g);
}
