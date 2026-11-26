// SPDX-License-Identifier: GPL-2.0-or-later
/* find [-H | -L | -P] [path...] [expression]
 * Walks each path depth first and evaluates the expression for every file.
 * Tests: -name -iname -path -ipath -wholename -regex -iregex -type -size
 * -empty -newer -anewer -cnewer -mtime -atime -ctime -mmin -amin -cmin -perm
 * -user -group -uid -gid -nouser -nogroup -links -inum -samefile -readable
 * -writable -executable -true -false. Options: -maxdepth -mindepth -depth
 * -xdev -mount -daystart -regextype -follow -noleaf. Actions: -print -print0
 * -printf -fprint -ls -exec ... ; -exec ... {} + -execdir -delete -prune -quit.
 * Operators: ( ) ! -not -a -and -o -or ,
 * Without an action, -print is implied. -ok is not supported (no prompt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __wasm__
#include <sys/syscall.h>
#else
#include <spawn.h>
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_IFBLK
#define S_IFBLK 0060000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == S_IFLNK)
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#endif

enum { N_AND, N_OR, N_NOT, N_COMMA, N_TRUE, N_FALSE, N_NAME, N_PATH, N_REGEX, N_TYPE, N_SIZE, N_EMPTY, N_NEWER,
       N_TIME, N_PERM, N_UID, N_GID, N_NOUSER, N_NOGROUP, N_LINKS, N_INUM, N_SAMEFILE, N_ACCESS,
       N_PRINT, N_PRINT0, N_PRINTF, N_LS, N_EXEC, N_DELETE, N_PRUNE, N_QUIT };
struct node {
  int type;
  struct node *a, *b;
  char *s;          /* pattern, format */
  int flags;        /* fnmatch flags, regex: icase, type: set of letters as bits */
  regex_t re;
  int cmp;          /* -1 less, 0 equal, 1 more */
  long long n;      /* number */
  long long unit;   /* -size unit, -time seconds per unit */
  int which;        /* time: 'm' 'a' 'c' */
  unsigned mode; int permkind;
  FILE *out;
  /* exec */
  char **argv; int argc; int plus, dir;
  char **acc; int nacc, capacc; long accsize;
};

static int follow = 'P', depth_first, xdev, rc;
static long maxdepth = -1, mindepth = 0;
static time_t now;
static int regex_type = 'e'; /* e emacs (GNU find's default), b POSIX basic, x POSIX extended */
static char **av;
static int ac, ip;
static int saw_action, saw_test;
static int quitting;
static char *start_cwd;

static void die(const char *fmt, const char *arg) {
  fprintf(stderr, "find: ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  exit(1);
}
static struct node *mk(int type) { struct node *n = calloc(1, sizeof *n); n->type = type; return n; }
static const char *need_arg(const char *opt) {
  if (ip >= ac) die("missing argument to '%s'", opt);
  return av[ip++];
}
static void numarg(struct node *n, const char *s, const char *opt) {
  n->cmp = 0;
  if (*s == '+') { n->cmp = 1; s++; } else if (*s == '-') { n->cmp = -1; s++; }
  char *e;
  if (!isdigit((unsigned char)*s)) die("invalid argument to %s", opt);
  n->n = strtoll(s, &e, 10);
  if (*e) die("invalid argument to %s", opt);
}
static int cmpnum(struct node *n, long long v) { return n->cmp > 0 ? v > n->n : n->cmp < 0 ? v < n->n : v == n->n; }

static unsigned parse_symbolic(const char *s, int *ok) {
  unsigned m = 0;
  const char *p = s;
  *ok = 1;
  if (isdigit((unsigned char)*p)) {
    char *e;
    m = strtoul(p, &e, 8);
    if (*e || m > 07777) *ok = 0;
    return m;
  }
  while (*p) {
    unsigned who = 0;
    while (*p && strchr("ugoa", *p)) { who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777; p++; }
    if (!who) who = 07777;
    if (!*p || !strchr("+-=", *p)) { *ok = 0; return 0; }
    while (*p && strchr("+-=", *p)) {
      int op = *p++;
      unsigned bits = 0;
      while (*p && strchr("rwxXst", *p)) {
        switch (*p++) {
        case 'r': bits |= 0444; break;
        case 'w': bits |= 0222; break;
        case 'x': case 'X': bits |= 0111; break;
        case 's': bits |= 06000; break;
        case 't': bits |= 01000; break;
        }
      }
      bits &= who;
      if (op == '+') m |= bits; else if (op == '-') m &= ~bits; else m = (m & ~who) | bits;
    }
    if (*p == ',') p++;
    else if (*p) { *ok = 0; return 0; }
  }
  return m;
}

static long lookup_id(const char *file, const char *name, int *found) {
  char *e;
  long v = strtol(name, &e, 10);
  *found = 1;
  if (*name && !*e) return v;
  FILE *f = fopen(file, "r");
  char line[1024];
  size_t nl = strlen(name);
  while (f && fgets(line, sizeof line, f)) {
    if (!strncmp(line, name, nl) && line[nl] == ':') {
      char *c = strchr(line + nl + 1, ':');
      if (c) { fclose(f); return atol(c + 1); }
    }
  }
  if (f) fclose(f);
  *found = 0;
  return -1;
}
static const char *name_of(const char *file, unsigned id) {
  static char buf[2][64];
  static int k;
  FILE *f = fopen(file, "r");
  char line[1024];
  while (f && fgets(line, sizeof line, f)) {
    char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    if (c2 && (unsigned)atol(c2 + 1) == id) {
      *c1 = 0;
      k ^= 1;
      snprintf(buf[k], sizeof buf[k], "%s", line);
      fclose(f);
      return buf[k];
    }
  }
  if (f) fclose(f);
  return NULL;
}

/* emacs regular expressions (GNU find's default) to POSIX basic: + ? | ( ) differ */
static char *emacs_to_bre(const char *s) {
  char *o = malloc(strlen(s) * 2 + 1), *w = o;
  int br = 0;
  for (; *s; s++) {
    if (br) { *w++ = *s; if (*s == ']' && br > 1) br = 0; else br = 2; continue; }
    if (*s == '[') { *w++ = *s; br = 1; if (s[1] == '^') *w++ = *++s; if (s[1] == ']') { *w++ = *++s; br = 2; } continue; }
    if (*s == '+' || *s == '?') { *w++ = '\\'; *w++ = *s; continue; }
    if (*s == '\\' && (s[1] == '+' || s[1] == '?')) { *w++ = *++s; continue; }
    *w++ = *s;
    if (*s == '\\' && s[1]) *w++ = *++s;
  }
  *w = 0;
  return o;
}

static int is_global_option(const char *t) {
  return !strcmp(t, "-maxdepth") || !strcmp(t, "-mindepth") || !strcmp(t, "-depth") || !strcmp(t, "-d") ||
         !strcmp(t, "-xdev") || !strcmp(t, "-mount") || !strcmp(t, "-noleaf") || !strcmp(t, "-ignore_readdir_race") ||
         !strcmp(t, "-noignore_readdir_race");
}

static struct node *parse_expr(void);
static struct node *parse_primary(void) {
  if (ip >= ac) die("%s", "expected an expression");
  const char *t = av[ip++];
  struct node *n;
  if (!strcmp(t, "(")) {
    n = parse_expr();
    if (ip >= ac || strcmp(av[ip], ")")) die("%s", "invalid expression; I was expecting to find a ')' somewhere but did not see one.");
    ip++;
    return n;
  }
  if (!strcmp(t, "!") || !strcmp(t, "-not")) {
    if (ip >= ac) die("invalid expression; '%s' needs something after it", t);
    n = mk(N_NOT);
    n->a = parse_primary();
    return n;
  }
  if (is_global_option(t) && saw_test)
    fprintf(stderr, "find: warning: you have specified the global option %s after the argument %s, but global options are not positional, i.e., %s affects tests specified before it as well as those specified after it.  Please specify global options before other arguments.\n", t, av[ip - 2], t);
  if (!strcmp(t, "-maxdepth") || !strcmp(t, "-mindepth")) {
    const char *v = need_arg(t);
    char *e;
    long d = strtol(v, &e, 10);
    if (*e || !*v || d < 0) { fprintf(stderr, "find: Expected a positive decimal integer argument to %s, but got '%s'\n", t, v); exit(1); }
    if (t[2] == 'a') maxdepth = d; else mindepth = d;
    return mk(N_TRUE);
  }
  if (!strcmp(t, "-depth") || !strcmp(t, "-d")) { depth_first = 1; return mk(N_TRUE); }
  if (!strcmp(t, "-xdev") || !strcmp(t, "-mount")) { xdev = 1; return mk(N_TRUE); }
  if (!strcmp(t, "-noleaf") || !strcmp(t, "-ignore_readdir_race") || !strcmp(t, "-noignore_readdir_race") || !strcmp(t, "-nowarn") || !strcmp(t, "-warn")) return mk(N_TRUE);
  if (!strcmp(t, "-follow")) { follow = 'L'; return mk(N_TRUE); }
  if (!strcmp(t, "-daystart")) {
    struct tm tm;
    time_t t0 = time(NULL);
    gmtime_r(&t0, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    now = timegm(&tm) + 86400;
    return mk(N_TRUE);
  }
  if (!strcmp(t, "-regextype")) {
    const char *v = need_arg(t);
    if (!strcmp(v, "posix-extended") || !strcmp(v, "egrep") || !strcmp(v, "posix-egrep") || !strcmp(v, "awk") || !strcmp(v, "gnu-awk") || !strcmp(v, "posix-awk")) regex_type = 'x';
    else if (!strcmp(v, "posix-basic") || !strcmp(v, "grep") || !strcmp(v, "sed")) regex_type = 'b';
    else if (!strcmp(v, "emacs") || !strcmp(v, "findutils-default")) regex_type = 'e';
    else die("Unknown regular expression type '%s'", v);
    return mk(N_TRUE);
  }
  saw_test = 1;
  if (!strcmp(t, "-true")) return mk(N_TRUE);
  if (!strcmp(t, "-false")) return mk(N_FALSE);
  if (!strcmp(t, "-name") || !strcmp(t, "-iname") || !strcmp(t, "-path") || !strcmp(t, "-ipath") || !strcmp(t, "-wholename") || !strcmp(t, "-iwholename")) {
    n = mk(t[1] == 'n' || t[2] == 'n' ? N_NAME : N_PATH);
    n->s = (char *)need_arg(t);
    n->flags = t[1] == 'i' ? FNM_CASEFOLD : 0;
    return n;
  }
  if (!strcmp(t, "-regex") || !strcmp(t, "-iregex")) {
    n = mk(N_REGEX);
    const char *v = need_arg(t);
    char *pat = regex_type == 'e' ? emacs_to_bre(v) : strdup(v);
    /* the whole path must match: anchor both ends */
    char *full = malloc(strlen(pat) + 8);
    sprintf(full, regex_type == 'x' ? "^(%s)$" : "^\\(%s\\)$", pat);
    int e = regcomp(&n->re, full, (regex_type == 'x' ? REG_EXTENDED : 0) | (t[1] == 'i' ? REG_ICASE : 0) | REG_NOSUB);
    if (e) { char m[128]; regerror(e, &n->re, m, sizeof m); die("invalid regular expression: %s", m); }
    free(pat); free(full);
    return n;
  }
  if (!strcmp(t, "-type") || !strcmp(t, "-xtype")) {
    n = mk(N_TYPE);
    const char *v = need_arg(t);
    for (const char *p = v; *p; p++) {
      if (*p == ',') continue;
      if (!strchr("bcdpflsD", *p)) die("Unknown argument to -type: %s", v);
      n->flags |= 1 << (strchr("bcdpflsD", *p) - "bcdpflsD");
      if (p[1] && p[1] != ',') die("Must separate multiple arguments to -type using: ','%s", "");
    }
    if (!*v) die("Arguments to -type should contain at least one letter%s", "");
    return n;
  }
  if (!strcmp(t, "-size")) {
    n = mk(N_SIZE);
    char *v = strdup(need_arg(t));
    size_t l = strlen(v);
    n->unit = 512;
    if (l && strchr("bcwkMG", v[l - 1])) {
      char u = v[--l];
      v[l] = 0;
      n->unit = u == 'b' ? 512 : u == 'c' ? 1 : u == 'w' ? 2 : u == 'k' ? 1024 : u == 'M' ? 1048576 : 1073741824;
    }
    numarg(n, v, "-size");
    return n;
  }
  if (!strcmp(t, "-empty")) return mk(N_EMPTY);
  if (!strcmp(t, "-newer") || !strcmp(t, "-anewer") || !strcmp(t, "-cnewer") || !strncmp(t, "-newer", 6)) {
    n = mk(N_NEWER);
    const char *v = need_arg(t);
    struct stat st;
    int which = 'm', ref = 'm';
    if (t[1] == 'a') which = 'a';
    else if (t[1] == 'c') which = 'c';
    else if (strlen(t) == 8) { which = t[6]; ref = t[7]; if (!strchr("amc", which) || !strchr("amc", ref)) die("invalid predicate '%s'", t); }
    if ((follow == 'P' ? lstat(v, &st) : stat(v, &st)) != 0) { fprintf(stderr, "find: '%s': %s\n", v, strerror(errno)); exit(1); }
    n->which = which;
    n->n = ref == 'a' ? st.st_atime : ref == 'c' ? st.st_ctime : st.st_mtime;
    return n;
  }
  if (!strcmp(t, "-mtime") || !strcmp(t, "-atime") || !strcmp(t, "-ctime") || !strcmp(t, "-mmin") || !strcmp(t, "-amin") || !strcmp(t, "-cmin")) {
    n = mk(N_TIME);
    n->which = t[1];
    n->unit = t[2] == 't' ? 86400 : 60;
    numarg(n, need_arg(t), t);
    return n;
  }
  if (!strcmp(t, "-perm")) {
    n = mk(N_PERM);
    const char *v = need_arg(t);
    n->permkind = 0;
    if (*v == '-' || *v == '/') n->permkind = *v++;
    else if (*v == '+' && isdigit((unsigned char)v[1])) n->permkind = '/', v++;
    int ok;
    n->mode = parse_symbolic(v, &ok);
    if (!ok) die("invalid mode '%s'", v);
    return n;
  }
  if (!strcmp(t, "-user") || !strcmp(t, "-group")) {
    n = mk(t[1] == 'u' ? N_UID : N_GID);
    const char *v = need_arg(t);
    int found;
    n->n = lookup_id(t[1] == 'u' ? "/etc/passwd" : "/etc/group", v, &found);
    if (!found) die(t[1] == 'u' ? "'%s' is not the name of a known user" : "'%s' is not the name of an existing group", v);
    n->cmp = 0;
    return n;
  }
  if (!strcmp(t, "-uid") || !strcmp(t, "-gid")) { n = mk(t[1] == 'u' ? N_UID : N_GID); numarg(n, need_arg(t), t); return n; }
  if (!strcmp(t, "-nouser")) return mk(N_NOUSER);
  if (!strcmp(t, "-nogroup")) return mk(N_NOGROUP);
  if (!strcmp(t, "-links")) { n = mk(N_LINKS); numarg(n, need_arg(t), t); return n; }
  if (!strcmp(t, "-inum")) { n = mk(N_INUM); numarg(n, need_arg(t), t); return n; }
  if (!strcmp(t, "-samefile")) {
    n = mk(N_SAMEFILE);
    const char *v = need_arg(t);
    struct stat st;
    if (stat(v, &st) != 0) { fprintf(stderr, "find: '%s': %s\n", v, strerror(errno)); exit(1); }
    n->n = st.st_ino; n->unit = st.st_dev;
    return n;
  }
  if (!strcmp(t, "-readable") || !strcmp(t, "-writable") || !strcmp(t, "-executable")) { n = mk(N_ACCESS); n->flags = t[1] == 'r' ? R_OK : t[1] == 'w' ? W_OK : X_OK; return n; }
  saw_test = 0;
  if (!strcmp(t, "-print")) { saw_action = 1; n = mk(N_PRINT); n->out = stdout; return n; }
  if (!strcmp(t, "-print0")) { saw_action = 1; n = mk(N_PRINT0); n->out = stdout; return n; }
  if (!strcmp(t, "-printf")) { saw_action = 1; n = mk(N_PRINTF); n->s = (char *)need_arg(t); n->out = stdout; return n; }
  if (!strcmp(t, "-fprint") || !strcmp(t, "-fprint0") || !strcmp(t, "-fprintf")) {
    saw_action = 1;
    const char *file = need_arg(t);
    n = mk(t[7] == '0' ? N_PRINT0 : t[7] == 'f' ? N_PRINTF : N_PRINT);
    if (n->type == N_PRINTF) n->s = (char *)need_arg(t);
    n->out = !strcmp(file, "/dev/stdout") ? stdout : !strcmp(file, "/dev/stderr") ? stderr : fopen(file, "w");
    if (!n->out) { fprintf(stderr, "find: '%s': %s\n", file, strerror(errno)); exit(1); }
    return n;
  }
  if (!strcmp(t, "-ls")) { saw_action = 1; return mk(N_LS); }
  if (!strcmp(t, "-delete")) { saw_action = 1; depth_first = 1; return mk(N_DELETE); }
  if (!strcmp(t, "-prune")) return mk(N_PRUNE);
  if (!strcmp(t, "-quit")) return mk(N_QUIT);
  if (!strcmp(t, "-exec") || !strcmp(t, "-execdir")) {
    saw_action = 1;
    n = mk(N_EXEC);
    n->dir = t[5] == 'd';
    int start = ip;
    while (ip < ac && strcmp(av[ip], ";") && !(!strcmp(av[ip], "+") && ip > start && !strcmp(av[ip - 1], "{}"))) ip++;
    if (ip >= ac) die("missing argument to '%s'", t);
    n->plus = !strcmp(av[ip], "+");
    n->argc = ip - start;
    if (n->argc == 0) die("missing argument to '%s'", t);
    n->argv = av + start;
    ip++;
    if (n->plus) {
      int braces = 0;
      for (int k = 0; k < n->argc; k++) if (strstr(n->argv[k], "{}")) braces++;
      if (braces > 1 || strcmp(n->argv[n->argc - 1], "{}")) die("%s", "only one instance of {} is supported with -exec ... +");
      n->argc--; /* the trailing {} */
    }
    return n;
  }
  if (!strcmp(t, "-ok") || !strcmp(t, "-okdir")) die("%s is not supported (it needs to ask on a terminal)", t);
  if (t[0] == '-') die("unknown predicate '%s'", t);
  die("paths must precede expression: '%s'", t);
  return NULL;
}
static struct node *parse_and(void) {
  struct node *l = parse_primary();
  for (;;) {
    if (ip >= ac) return l;
    const char *t = av[ip];
    if (!strcmp(t, "-o") || !strcmp(t, "-or") || !strcmp(t, ",") || !strcmp(t, ")")) return l;
    if (!strcmp(t, "-a") || !strcmp(t, "-and")) { ip++; if (ip >= ac) die("invalid expression; you have used a binary operator '%s' with nothing after it.", t); }
    struct node *n = mk(N_AND);
    n->a = l;
    n->b = parse_primary();
    l = n;
  }
}
static struct node *parse_or(void) {
  struct node *l = parse_and();
  while (ip < ac && (!strcmp(av[ip], "-o") || !strcmp(av[ip], "-or"))) {
    ip++;
    if (ip >= ac) die("invalid expression; you have used a binary operator '%s' with nothing after it.", av[ip - 1]);
    struct node *n = mk(N_OR);
    n->a = l;
    n->b = parse_and();
    l = n;
  }
  return l;
}
static struct node *parse_expr(void) {
  struct node *l = parse_or();
  while (ip < ac && !strcmp(av[ip], ",")) {
    ip++;
    struct node *n = mk(N_COMMA);
    n->a = l;
    n->b = parse_or();
    l = n;
  }
  return l;
}

/* ---- running commands ---- */
static int run(char **argv, const char *dir) {
  fflush(stdout);
  if (dir && chdir(dir) != 0) { fprintf(stderr, "find: cannot change directory to '%s': %s\n", dir, strerror(errno)); return -1; }
#ifdef __wasm__
  pid_t pid = spawnp(argv[0], argv, NULL);
  int err = errno;
#else
  extern char **environ;
  pid_t pid;
  int err = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
  if (err) pid = -1;
#endif
  if (dir && chdir(start_cwd) != 0) { fprintf(stderr, "find: cannot go back to '%s': %s\n", start_cwd, strerror(errno)); exit(1); }
  if (pid < 0) { fprintf(stderr, "find: '%s': %s\n", argv[0], strerror(err)); return -1; }
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
  if (WIFSIGNALED(st)) return -1;
  return WEXITSTATUS(st);
}
static char *subst(const char *arg, const char *path) {
  size_t cnt = 0, pl = strlen(path);
  for (const char *p = strstr(arg, "{}"); p; p = strstr(p + 2, "{}")) cnt++;
  char *o = malloc(strlen(arg) + cnt * pl + 1), *w = o;
  while (*arg) {
    if (arg[0] == '{' && arg[1] == '}') { memcpy(w, path, pl); w += pl; arg += 2; }
    else *w++ = *arg++;
  }
  *w = 0;
  return o;
}
static void flush_plus(struct node *n) {
  if (!n->nacc) return;
  char **argv = malloc((n->argc + n->nacc + 1) * sizeof *argv);
  int k = 0;
  for (int i = 0; i < n->argc; i++) argv[k++] = n->argv[i];
  for (int i = 0; i < n->nacc; i++) argv[k++] = n->acc[i];
  argv[k] = NULL;
  if (run(argv, NULL) != 0) rc = 1;
  for (int i = 0; i < n->nacc; i++) free(n->acc[i]);
  n->nacc = 0;
  n->accsize = 0;
  free(argv);
}
static void flush_all(struct node *n) {
  if (!n) return;
  if (n->type == N_EXEC && n->plus) flush_plus(n);
  flush_all(n->a);
  flush_all(n->b);
}

/* ---- evaluation ---- */
struct file { const char *path; const char *name; const char *start; int depth; struct stat st; int st_ok; int prune; };

static char type_letter(mode_t m) {
  switch (m & S_IFMT) {
  case S_IFDIR: return 'd';
  case S_IFCHR: return 'c';
  case S_IFBLK: return 'b';
  case S_IFIFO: return 'p';
  case S_IFLNK: return 'l';
  case S_IFSOCK: return 's';
  default: return 'f';
  }
}
static void mode_string(mode_t m, char *s) {
  s[0] = type_letter(m) == 'f' ? '-' : type_letter(m);
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s[i + 1] = m & (0400 >> i) ? rwx[i] : '-';
  if (m & 04000) s[3] = s[3] == 'x' ? 's' : 'S';
  if (m & 02000) s[6] = s[6] == 'x' ? 's' : 'S';
  if (m & 01000) s[9] = s[9] == 'x' ? 't' : 'T';
  s[10] = 0;
}
static long rdlink(const char *p, char *buf, size_t n) {
#ifdef __wasm__
  return __sysret(__syscall4(78, AT_FDCWD, p, buf, n));
#else
  return readlink(p, buf, n);
#endif
}

static void put_escape(const char **pp, FILE *out) {
  const char *p = *pp;
  switch (*p) {
  case 'n': putc('\n', out); break;
  case 't': putc('\t', out); break;
  case '0': putc(0, out); break;
  case 'a': putc(7, out); break;
  case 'b': putc(8, out); break;
  case 'f': putc(12, out); break;
  case 'r': putc(13, out); break;
  case 'v': putc(11, out); break;
  case '\\': putc('\\', out); break;
  case 'c': fflush(out); quitting = 2; break;
  default:
    if (*p >= '0' && *p <= '7') {
      int v = 0, k = 0;
      while (k < 3 && *p >= '0' && *p <= '7') v = v * 8 + (*p++ - '0'), k++;
      putc(v, out);
      *pp = p - 1;
      return;
    }
    putc('\\', out); putc(*p, out);
  }
}
static void do_printf(struct node *n, struct file *f) {
  FILE *out = n->out;
  for (const char *p = n->s; *p; p++) {
    if (*p == '\\' && p[1]) { p++; put_escape(&p, out); if (quitting == 2) return; continue; }
    if (*p != '%' || !p[1]) { putc(*p, out); continue; }
    /* flags and width, applied to the text of the directive */
    char spec[32];
    int sl = 0;
    spec[sl++] = '%';
    p++;
    while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
    while (*p && (isdigit((unsigned char)*p) || *p == '.') && sl < 28) spec[sl++] = *p++;
    spec[sl++] = 's';
    spec[sl] = 0;
    char buf[4096];
    buf[0] = 0;
    const struct stat *st = &f->st;
    switch (*p) {
    case '%': strcpy(buf, "%"); break;
    case 'p': snprintf(buf, sizeof buf, "%s", f->path); break;
    case 'f': {
      const char *b = f->name;
      snprintf(buf, sizeof buf, "%s", b);
      break;
    }
    case 'h': {
      const char *sl2 = strrchr(f->path, '/');
      if (!sl2) strcpy(buf, ".");
      else if (sl2 == f->path) strcpy(buf, "/");
      else snprintf(buf, sizeof buf, "%.*s", (int)(sl2 - f->path), f->path);
      if (f->depth == 0 && sl2 && sl2[1] == 0) snprintf(buf, sizeof buf, "%s", f->path);
      break;
    }
    case 'P': {
      size_t l = strlen(f->start);
      const char *r = f->path + (strncmp(f->path, f->start, l) ? 0 : l);
      while (*r == '/' && f->depth > 0) r++;
      snprintf(buf, sizeof buf, "%s", f->depth == 0 ? "" : r);
      break;
    }
    case 'H': snprintf(buf, sizeof buf, "%s", f->start); break;
    case 'Z': break; /* the SELinux context: there is none */
    case 'd': snprintf(buf, sizeof buf, "%d", f->depth); break;
    case 's': snprintf(buf, sizeof buf, "%lld", (long long)st->st_size); break;
    case 'k': snprintf(buf, sizeof buf, "%lld", ((long long)st->st_blocks + 1) / 2); break;
    case 'b': snprintf(buf, sizeof buf, "%lld", (long long)st->st_blocks); break;
    case 'm': snprintf(buf, sizeof buf, "%o", (unsigned)(st->st_mode & 07777)); break;
    case 'M': mode_string(st->st_mode, buf); break;
    case 'n': snprintf(buf, sizeof buf, "%lu", (unsigned long)st->st_nlink); break;
    case 'i': snprintf(buf, sizeof buf, "%llu", (unsigned long long)st->st_ino); break;
    case 'U': snprintf(buf, sizeof buf, "%u", (unsigned)st->st_uid); break;
    case 'G': snprintf(buf, sizeof buf, "%u", (unsigned)st->st_gid); break;
    case 'u': { const char *nm = name_of("/etc/passwd", st->st_uid); if (nm) snprintf(buf, sizeof buf, "%s", nm); else snprintf(buf, sizeof buf, "%u", (unsigned)st->st_uid); break; }
    case 'g': { const char *nm = name_of("/etc/group", st->st_gid); if (nm) snprintf(buf, sizeof buf, "%s", nm); else snprintf(buf, sizeof buf, "%u", (unsigned)st->st_gid); break; }
    case 'y': buf[0] = type_letter(st->st_mode); buf[1] = 0; break;
    case 'Y': { struct stat t; buf[0] = stat(f->path, &t) == 0 ? type_letter(t.st_mode) : 'N'; buf[1] = 0; break; }
    case 'l': if (S_ISLNK(st->st_mode)) { long r = rdlink(f->path, buf, sizeof buf - 1); buf[r > 0 ? r : 0] = 0; } break;
    case 'a': case 'c': case 't': case 'A': case 'C': case 'T': {
      int k = tolower((unsigned char)*p);
      time_t tv = k == 'a' ? st->st_atime : k == 'c' ? st->st_ctime : st->st_mtime;
      long nsec = 0;
#ifndef __wasm__
      nsec = k == 'a' ? st->st_atim.tv_nsec : k == 'c' ? st->st_ctim.tv_nsec : st->st_mtim.tv_nsec;
#endif
      struct tm tm;
      localtime_r(&tv, &tm);
      if (islower((unsigned char)*p)) {
        char tb[64];
        strftime(tb, sizeof tb, "%a %b %e %H:%M:%S", &tm);
        snprintf(buf, sizeof buf, "%s.%010ld %d", tb, nsec * 10, tm.tm_year + 1900);
        break;
      }
      p++;
      if (*p == '@') snprintf(buf, sizeof buf, "%lld.%010ld", (long long)tv, nsec * 10);
      else if (*p == '+') { strftime(buf, sizeof buf, "%Y-%m-%d+%H:%M:%S", &tm); snprintf(buf + strlen(buf), sizeof buf - strlen(buf), ".%010ld", nsec * 10); }
      else if (*p == 'S') { snprintf(buf, sizeof buf, "%02d.%010ld", tm.tm_sec, nsec * 10); }
      else if (*p) { char fmt[3] = {'%', *p, 0}; strftime(buf, sizeof buf, fmt, &tm); }
      else p--;
      break;
    }
    default:
      /* an unknown directive is printed as it is, as GNU find does */
      buf[0] = '%'; buf[1] = *p; buf[2] = 0;
    }
    fprintf(out, spec, buf);
    if (!*p) break;
  }
}

static void do_ls(struct file *f) {
  char m[12];
  mode_string(f->st.st_mode, m);
  const char *u = name_of("/etc/passwd", f->st.st_uid), *g = name_of("/etc/group", f->st.st_gid);
  char ub[16], gb[16];
  if (!u) { snprintf(ub, sizeof ub, "%u", (unsigned)f->st.st_uid); u = ub; }
  if (!g) { snprintf(gb, sizeof gb, "%u", (unsigned)f->st.st_gid); g = gb; }
  struct tm tm;
  time_t t = f->st.st_mtime;
  localtime_r(&t, &tm);
  char tb[32];
  if (now - t > 182 * 86400 || t > now) strftime(tb, sizeof tb, "%b %e  %Y", &tm);
  else strftime(tb, sizeof tb, "%b %e %H:%M", &tm);
  printf("%9llu %6lld %s %3lu %-8s %-8s %8lld %s %s", (unsigned long long)f->st.st_ino, ((long long)f->st.st_blocks + 1) / 2, m,
         (unsigned long)f->st.st_nlink, u, g, (long long)f->st.st_size, tb, f->path);
  if (S_ISLNK(f->st.st_mode)) { char l[4096]; long r = rdlink(f->path, l, sizeof l - 1); if (r >= 0) { l[r] = 0; printf(" -> %s", l); } }
  putchar('\n');
}

static int is_empty_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d) return 0;
  struct dirent *e;
  int empty = 1;
  while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = 0; break; }
  closedir(d);
  return empty;
}

static int eval(struct node *n, struct file *f) {
  const struct stat *st = &f->st;
  switch (n->type) {
  case N_AND: return eval(n->a, f) && !quitting && eval(n->b, f);
  case N_OR: return eval(n->a, f) || (!quitting && eval(n->b, f));
  case N_NOT: return !eval(n->a, f);
  case N_COMMA: eval(n->a, f); return quitting ? 0 : eval(n->b, f);
  case N_TRUE: return 1;
  case N_FALSE: return 0;
  case N_NAME: {
    /* the last component, without trailing slashes */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", f->name);
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    return fnmatch(n->s, tmp, n->flags) == 0;
  }
  case N_PATH: return fnmatch(n->s, f->path, n->flags) == 0;
  case N_REGEX: return regexec(&n->re, f->path, 0, NULL, 0) == 0;
  case N_TYPE: {
    if (!f->st_ok) return 0;
    char c = type_letter(st->st_mode);
    return (n->flags >> (strchr("bcdpflsD", c) - "bcdpflsD")) & 1;
  }
  case N_SIZE: {
    long long sz = st->st_size;
    long long units = n->unit == 1 ? sz : (sz + n->unit - 1) / n->unit;
    return cmpnum(n, units);
  }
  case N_EMPTY:
    if (S_ISDIR(st->st_mode)) return is_empty_dir(f->path);
    return S_ISREG(st->st_mode) && st->st_size == 0;
  case N_NEWER: {
    time_t t = n->which == 'a' ? st->st_atime : n->which == 'c' ? st->st_ctime : st->st_mtime;
    return t > n->n;
  }
  case N_TIME: {
    time_t t = n->which == 'a' ? st->st_atime : n->which == 'c' ? st->st_ctime : st->st_mtime;
    long long age = now - t;
    long long units;
    if (n->unit == 86400) units = age < 0 ? -1 - (-age - 1) / 86400 : age / 86400;
    else units = age <= 0 ? age / 60 : (age + 59) / 60; /* GNU rounds minutes up */
    return cmpnum(n, units);
  }
  case N_PERM: {
    unsigned m = st->st_mode & 07777;
    if (n->permkind == '-') return (m & n->mode) == n->mode;
    if (n->permkind == '/') return n->mode == 0 || (m & n->mode) != 0;
    return m == n->mode;
  }
  case N_UID: return cmpnum(n, st->st_uid);
  case N_GID: return cmpnum(n, st->st_gid);
  case N_NOUSER: return !name_of("/etc/passwd", st->st_uid);
  case N_NOGROUP: return !name_of("/etc/group", st->st_gid);
  case N_LINKS: return cmpnum(n, st->st_nlink);
  case N_INUM: return cmpnum(n, st->st_ino);
  case N_SAMEFILE: return (long long)st->st_ino == n->n && (long long)st->st_dev == n->unit;
  case N_ACCESS: return access(f->path, n->flags) == 0;
  case N_PRINT: fputs(f->path, n->out); putc('\n', n->out); return 1;
  case N_PRINT0: fputs(f->path, n->out); putc(0, n->out); return 1;
  case N_PRINTF: do_printf(n, f); if (quitting == 2) quitting = 1; return 1;
  case N_LS: do_ls(f); return 1;
  case N_PRUNE: if (!depth_first) f->prune = 1; return 1;
  case N_QUIT: quitting = 1; return 1;
  case N_DELETE: {
    if (!strcmp(f->path, ".")) return 1;
    int r = S_ISDIR(st->st_mode) ? rmdir(f->path) : unlink(f->path);
    if (r != 0) { fprintf(stderr, "find: cannot delete '%s': %s\n", f->path, strerror(errno)); rc = 1; return 0; }
    return 1;
  }
  case N_EXEC: {
    char *dirpath = NULL;
    char base[4096];
    const char *target = f->path;
    if (n->dir) {
      const char *sl = strrchr(f->path, '/');
      if (sl) {
        dirpath = strndup(f->path, sl == f->path ? 1 : (size_t)(sl - f->path));
        snprintf(base, sizeof base, "./%s", sl + 1);
      } else snprintf(base, sizeof base, "./%s", f->path);
      target = base;
    }
    if (n->plus) {
      long len = strlen(target) + 1 + sizeof(char *);
      if (n->nacc && n->accsize + len > 6144) flush_plus(n); /* stay under the argument limit of spawn */
      if (n->nacc == n->capacc) n->acc = realloc(n->acc, (n->capacc = n->capacc * 2 + 16) * sizeof *n->acc);
      n->acc[n->nacc++] = strdup(target);
      n->accsize += len;
      free(dirpath);
      return 1;
    }
    char **argv = malloc((n->argc + 1) * sizeof *argv);
    for (int i = 0; i < n->argc; i++) argv[i] = subst(n->argv[i], target);
    argv[n->argc] = NULL;
    int r = run(argv, dirpath);
    for (int i = 0; i < n->argc; i++) free(argv[i]);
    free(argv);
    free(dirpath);
    return r == 0;
  }
  }
  return 0;
}

static struct node *root;
static void visit(const char *path, const char *name, const char *start, int depth, unsigned long long dev, int top) {
  if (quitting) return;
  struct file f = {path, name, start, depth, {0}, 0, 0};
  int fl = follow == 'L' || (follow == 'H' && top);
  int r = fl ? stat(path, &f.st) : lstat(path, &f.st);
  if (r != 0 && fl && errno == ENOENT) r = lstat(path, &f.st); /* a dangling link is still listed */
  if (r != 0) { fprintf(stderr, "find: '%s': %s\n", path, strerror(errno)); rc = 1; return; }
  f.st_ok = 1;
  if (top) dev = f.st.st_dev;
  int isdir = S_ISDIR(f.st.st_mode);
  int in_range = depth >= mindepth;
  if (!depth_first && in_range) eval(root, &f);
  if (isdir && !f.prune && !quitting && (maxdepth < 0 || depth < maxdepth) && !(xdev && f.st.st_dev != dev)) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "find: '%s': %s\n", path, strerror(errno)); rc = 1; }
    else {
      /* read the names first, so that -delete or -exec changing the directory does not disturb the walk */
      char **names = NULL;
      int nn = 0, cap = 0;
      struct dirent *e;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (nn == cap) names = realloc(names, (cap = cap * 2 + 32) * sizeof *names);
        names[nn++] = strdup(e->d_name);
      }
      closedir(d);
      size_t pl = strlen(path);
      for (int k = 0; k < nn; k++) {
        char *sub = malloc(pl + strlen(names[k]) + 2);
        if (pl && path[pl - 1] == '/') sprintf(sub, "%s%s", path, names[k]);
        else sprintf(sub, "%s/%s", path, names[k]);
        visit(sub, sub + strlen(sub) - strlen(names[k]), start, depth + 1, dev, 0);
        free(sub);
        free(names[k]);
      }
      free(names);
    }
  }
  if (depth_first && in_range && !quitting) eval(root, &f);
}

int main(int argc, char **argv) {
  int i = 1;
  now = time(NULL);
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "-H") || !strcmp(argv[i], "-L") || !strcmp(argv[i], "-P")) follow = argv[i][1];
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else if (!strncmp(argv[i], "-O", 2) || !strncmp(argv[i], "-D", 2)) ;
    else break;
  }
  int pstart = i;
  while (i < argc && !(argv[i][0] == '-' && argv[i][1]) && strcmp(argv[i], "(") && strcmp(argv[i], "!")) i++;
  int npaths = i - pstart;
  av = argv + i;
  ac = argc - i;
  if (ac > 0) root = parse_expr();
  if (ip < ac) die("unexpected extra predicate '%s'", av[ip]);
  if (!saw_action) {
    struct node *p = mk(N_PRINT);
    p->out = stdout;
    if (root) { struct node *a = mk(N_AND); a->a = root; a->b = p; root = a; }
    else root = p;
  }
  char cwd[4096];
  start_cwd = getcwd(cwd, sizeof cwd) ? strdup(cwd) : strdup(".");
  if (!isatty(1)) setvbuf(stdout, NULL, _IOFBF, 65536);
  if (npaths == 0) visit(".", ".", ".", 0, 0, 1);
  for (int k = 0; k < npaths && !quitting; k++) {
    const char *p = argv[pstart + k];
    if (!*p) { fprintf(stderr, "find: '': No such file or directory\n"); rc = 1; continue; }
    const char *name = p + strlen(p);
    while (name > p && name[-1] == '/') name--;
    while (name > p && name[-1] != '/') name--;
    if (!*name) name = p;
    visit(p, name, p, 0, 0, 1);
  }
  flush_all(root);
  if (fflush(stdout) != 0) { fprintf(stderr, "find: write error: %s\n", strerror(errno)); return 1; }
  return rc;
}
