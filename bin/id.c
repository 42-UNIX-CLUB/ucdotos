// SPDX-License-Identifier: GPL-2.0-or-later
/* id [-u | -g | -G] [-n] [-r] [user]: print user and group ids. Names come
 * from /etc/passwd and /etc/group when they exist; otherwise numbers only.
 * groups= and -G list the supplementary groups too (getgroups, or /etc/group for a user). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* field `want` (0-based) of the line in a colon file whose field `key` equals s */
static int lookup(const char *file, int key, const char *s, int want, char *out, size_t n) {
  FILE *f = fopen(file, "r");
  if (!f) return 0;
  char line[1024];
  int found = 0;
  while (!found && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    char *fld[8] = {0}, *p = line;
    int nf = 0;
    while (nf < 8) { fld[nf++] = p; p = strchr(p, ':'); if (!p) break; *p++ = 0; }
    if (key < nf && want < nf && !strcmp(fld[key], s)) { snprintf(out, n, "%s", fld[want]); found = 1; }
  }
  fclose(f);
  return found;
}
static const char *uname_of(unsigned id) { static char b[64]; char k[16]; snprintf(k, sizeof k, "%u", id); return lookup("/etc/passwd", 2, k, 0, b, sizeof b) ? b : NULL; }
static const char *gname_of(unsigned id) { static char b[64]; char k[16]; snprintf(k, sizeof k, "%u", id); return lookup("/etc/group", 2, k, 0, b, sizeof b) ? b : NULL; }

/* The groups: the primary group first, then the supplementary ones, each once. For this
 * process they come from the kernel (getgroups); for a named user, from the member lists
 * in /etc/group (the groups that user would get at login). */
static int group_list(const char *user, unsigned gid, unsigned *out, int cap) {
  int n = 0;
  out[n++] = gid;
  if (!user) {
    gid_t g[64];
    int k = getgroups(64, g);
    for (int j = 0; j < k && n < cap; j++) {
      int dup = 0;
      for (int m = 0; m < n; m++) if (out[m] == (unsigned)g[j]) dup = 1;
      if (!dup) out[n++] = g[j];
    }
    return n;
  }
  FILE *f = fopen("/etc/group", "r");
  if (!f) return n;
  char line[1024];
  while (n < cap && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    char *fld[4] = {0}, *p = line;
    int nf = 0;
    while (nf < 4) { fld[nf++] = p; p = strchr(p, ':'); if (!p) break; *p++ = 0; }
    if (nf < 4) continue;
    unsigned g = strtoul(fld[2], NULL, 10);
    for (char *m = strtok(fld[3], ","); m; m = strtok(NULL, ",")) {
      if (strcmp(m, user)) continue;
      int dup = 0;
      for (int j = 0; j < n; j++) if (out[j] == g) dup = 1;
      if (!dup) out[n++] = g;
    }
  }
  fclose(f);
  return n;
}

int main(int argc, char **argv) {
  int i = 1, ou = 0, og = 0, oG = 0, names = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--user")) { ou = 1; continue; }
    if (!strcmp(a, "--group")) { og = 1; continue; }
    if (!strcmp(a, "--groups")) { oG = 1; continue; }
    if (!strcmp(a, "--name")) { names = 1; continue; }
    if (!strcmp(a, "--real")) continue;
    for (char *f = a + 1; *f; f++) {
      if (*f == 'u') ou = 1; else if (*f == 'g') og = 1; else if (*f == 'G') oG = 1;
      else if (*f == 'n') names = 1; else if (*f == 'r') ;
      else { fprintf(stderr, "id: invalid option -- '%c'\nusage: id [-u|-g|-G] [-n] [-r] [user]\n", *f); return 1; }
    }
  }
  if (ou + og + oG > 1) { fprintf(stderr, "id: cannot print \"only\" of more than one choice\n"); return 1; }
  if (names && !(ou || og || oG)) { fprintf(stderr, "id: cannot print only names or real IDs in default format\n"); return 1; }
  unsigned uid = getuid(), gid = getgid();
  if (i < argc) {
    char b[64];
    if (argc - i > 1) { fprintf(stderr, "id: extra operand '%s'\n", argv[i + 1]); return 1; }
    if (lookup("/etc/passwd", 0, argv[i], 2, b, sizeof b)) uid = atoi(b);
    else { char *e; unsigned long v = strtoul(argv[i], &e, 10); if (*e || !*argv[i] || !uname_of(v)) { fprintf(stderr, "id: '%s': no such user\n", argv[i]); return 1; } uid = v; }
    char k[16];
    snprintf(k, sizeof k, "%u", uid);
    if (lookup("/etc/passwd", 2, k, 3, b, sizeof b)) gid = atoi(b);
  }
  const char *un = uname_of(uid), *gn = gname_of(gid);
  unsigned gl[64];
  char uname_copy[64] = "";
  if (i < argc && un) snprintf(uname_copy, sizeof uname_copy, "%s", un);
  int ng = group_list(i < argc ? uname_copy : NULL, gid, gl, 64);
  if (oG) { /* every group, separated by spaces */
    for (int k = 0; k < ng; k++) {
      const char *n = gname_of(gl[k]);
      if (k) putchar(' ');
      if (names && n) fputs(n, stdout); else printf("%u", gl[k]);
    }
    putchar('\n');
    return 0;
  }
  if (ou || og) {
    unsigned v = ou ? uid : gid;
    const char *n = ou ? un : gn;
    if (names && n) puts(n);
    else if (names) { printf("%u\n", v); fprintf(stderr, "id: cannot find name for %s ID %u\n", ou ? "user" : "group", v); return 1; }
    else printf("%u\n", v);
    return 0;
  }
  printf("uid=%u", uid);
  if (un) printf("(%s)", un);
  printf(" gid=%u", gid);
  if (gn) printf("(%s)", gn);
  printf(" groups=");
  for (int k = 0; k < ng; k++) {
    const char *n = gname_of(gl[k]);
    printf("%s%u", k ? "," : "", gl[k]);
    if (n) printf("(%s)", n);
  }
  putchar('\n');
  return 0;
}
