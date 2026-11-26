// SPDX-License-Identifier: GPL-2.0-or-later
/* test and [: the shell's builtin, and also the /bin/test program (bin/test.c
 * includes this file). Errors go through outs/outf, which each side provides. */
#include "sh.h"

static char **targ;
static int tn, tpos, terr;
static int t_or(void);
static int is_binop(const char *s) {
  static const char *ops[] = {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", 0};
  for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return 1;
  return 0;
}
static int is_num(const char *s) {
  if (*s == '-' || *s == '+') s++;
  if (!*s) return 0;
  for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
  return 1;
}
static int t_binary(const char *a, const char *op, const char *b) {
  if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
  if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
  if (!strcmp(op, "<")) return strcmp(a, b) < 0;
  if (!strcmp(op, ">")) return strcmp(a, b) > 0;
  if (op[1] == 'n' && op[2] == 't') { struct stat x, y; return stat(a, &x) == 0 && (stat(b, &y) != 0 || x.st_mtime > y.st_mtime); }
  if (op[1] == 'o' && op[2] == 't') { struct stat x, y; return stat(b, &y) == 0 && (stat(a, &x) != 0 || x.st_mtime < y.st_mtime); }
  if (!is_num(a) || !is_num(b)) { outf(2, "test: integer expression expected: %s\n", is_num(a) ? b : a); terr = 1; return 0; }
  long long x = strtoll(a, NULL, 10), y = strtoll(b, NULL, 10);
  if (!strcmp(op, "-eq")) return x == y;
  if (!strcmp(op, "-ne")) return x != y;
  if (!strcmp(op, "-lt")) return x < y;
  if (!strcmp(op, "-le")) return x <= y;
  if (!strcmp(op, "-gt")) return x > y;
  return x >= y;
}
static int t_unary(char op, const char *a) {
  struct stat st;
  switch (op) {
  case 'z': return !*a;
  case 'n': return *a != 0;
  case 't': return __syscall3(SYS_ioctl, atoi(a), TCGETS, &(struct termios){0}) == 0;
  }
  if (stat(a, &st) < 0) return 0;
  switch (op) {
  case 'e': case 'r': case 'w': return 1;
  case 'f': return S_ISREG(st.st_mode);
  case 'd': return S_ISDIR(st.st_mode);
  case 'c': return S_ISCHR(st.st_mode);
  case 'p': return S_ISFIFO(st.st_mode);
  case 'S': return (st.st_mode & S_IFMT) == S_IFSOCK;
  case 's': return st.st_size > 0;
  case 'x': return S_ISDIR(st.st_mode) || (st.st_mode & 0111) || (S_ISREG(st.st_mode) && !(st.st_mode & 0777));
  case 'h': case 'L': return 0;
  }
  return 0;
}
static int t_primary(void) {
  if (tpos >= tn) { terr = 1; return 0; }
  char *a = targ[tpos];
  if (tpos + 2 < tn && is_binop(targ[tpos + 1])) { tpos += 3; return t_binary(a, targ[tpos - 2], targ[tpos - 1]); }
  if (!strcmp(a, "!")) { tpos++; return !t_primary(); }
  if (!strcmp(a, "(")) {
    tpos++;
    int v = t_or();
    if (tpos < tn && !strcmp(targ[tpos], ")")) tpos++; else terr = 1;
    return v;
  }
  if (a[0] == '-' && a[1] && !a[2] && strchr("zntefdcpSsxrwhL", a[1]) && tpos + 1 < tn) { tpos += 2; return t_unary(a[1], targ[tpos - 1]); }
  tpos++;
  return *a != 0;
}
static int t_and(void) {
  int v = t_primary();
  while (tpos < tn && !strcmp(targ[tpos], "-a")) { tpos++; int r = t_primary(); v = v && r; }
  return v;
}
static int t_or(void) {
  int v = t_and();
  while (tpos < tn && !strcmp(targ[tpos], "-o")) { tpos++; int r = t_and(); v = v || r; }
  return v;
}
static int t_unop(const char *a) { return a[0] == '-' && a[1] && !a[2] && strchr("zntefdcpSsxrwhL", a[1]); }
/* POSIX's rules for up to four arguments; -1: use the general grammar */
static int t_posix(char **a, int n) {
  int r;
  switch (n) {
  case 0: return 0;
  case 1: return a[0][0] != 0;
  case 2:
    if (!strcmp(a[0], "!")) return !a[1][0];
    if (t_unop(a[0])) return t_unary(a[0][1], a[1]);
    return -1;
  case 3:
    if (is_binop(a[1])) return t_binary(a[0], a[1], a[2]);
    if (!strcmp(a[1], "-a")) return a[0][0] && a[2][0];
    if (!strcmp(a[1], "-o")) return a[0][0] || a[2][0];
    if (!strcmp(a[0], "!")) return (r = t_posix(a + 1, 2)) < 0 ? -1 : !r;
    if (!strcmp(a[0], "(") && !strcmp(a[2], ")")) return a[1][0] != 0;
    return -1;
  case 4:
    if (!strcmp(a[0], "!")) return (r = t_posix(a + 1, 3)) < 0 ? -1 : !r;
    if (!strcmp(a[0], "(") && !strcmp(a[3], ")")) return t_posix(a + 1, 2);
    return -1;
  }
  return -1;
}
int sh_test(int argc, char **argv) {
  const char *base = strrchr(argv[0], '/');
  if (!strcmp(base ? base + 1 : argv[0], "[")) {
    if (strcmp(argv[argc - 1], "]")) { outs(2, "[: missing `]'\n"); return 2; }
    argc--;
  }
  targ = argv + 1; tn = argc - 1; tpos = 0; terr = 0;
  int v = tn <= 4 ? t_posix(targ, tn) : -1;
  if (v < 0) {
    v = t_or();
    if (!terr && tpos < tn) { outf(2, "test: %s: unexpected argument\n", targ[tpos]); terr = 1; }
  }
  return terr ? 2 : !v;
}

