// SPDX-License-Identifier: GPL-2.0-or-later
/* Builtin commands. They run inside the shell process; output goes through
 * outs/outf so that $(...) capture and redirections see it. */
#include "sh.h"

extern int exit_pending, exit_code, exec_keep, loop_depth;

static int b_true(int argc, char **argv) { return 0; }
static int b_false(int argc, char **argv) { return 1; }

/* backslash escapes for echo -e and printf; returns 1 if \c (stop output) was seen */
static int unescape_to(const char *s, char *o, int *olen) {
  int n = 0;
  for (; *s; s++) {
    if (*s != '\\' || !s[1]) { o[n++] = *s; continue; }
    s++;
    switch (*s) {
    case 'n': o[n++] = '\n'; break;
    case 't': o[n++] = '\t'; break;
    case 'r': o[n++] = '\r'; break;
    case 'a': o[n++] = '\a'; break;
    case 'b': o[n++] = '\b'; break;
    case 'f': o[n++] = '\f'; break;
    case 'v': o[n++] = '\v'; break;
    case 'e': case 'E': o[n++] = 27; break;
    case '\\': o[n++] = '\\'; break;
    case 'c': *olen = n; return 1;
    case 'x': {
      int v = 0, k = 0;
      while (k < 2 && isxdigit((unsigned char)s[1])) { s++; v = v * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (tolower(*s) - 'a' + 10)); k++; }
      if (k) o[n++] = v; else { o[n++] = '\\'; o[n++] = 'x'; }
      break;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
      int v = 0, k = 0, max = *s == '0' ? 4 : 3;
      s--;
      while (k < max && s[1] >= '0' && s[1] <= '7') { s++; v = v * 8 + (*s - '0'); k++; }
      o[n++] = v;
      break;
    }
    default: o[n++] = '\\'; o[n++] = *s;
    }
  }
  *olen = n;
  return 0;
}

static int b_echo(int argc, char **argv) {
  int nl = 1, esc = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *f = argv[i] + 1;
    if (strspn(f, "neE") != strlen(f)) break;
    for (; *f; f++) { if (*f == 'n') nl = 0; else if (*f == 'e') esc = 1; else esc = 0; }
  }
  size_t total = 2;
  for (int j = i; j < argc; j++) total += strlen(argv[j]) + 1;
  char *buf = malloc(total), *o = buf;
  int stop = 0;
  for (; i < argc && !stop; i++) {
    if (esc) { int n; stop = unescape_to(argv[i], o, &n); o += n; }
    else { strcpy(o, argv[i]); o += strlen(argv[i]); }
    if (i + 1 < argc && !stop) *o++ = ' ';
  }
  if (nl && !stop) *o++ = '\n';
  sh_write(1, buf, o - buf);
  free(buf);
  return 0;
}

static int b_printf(int argc, char **argv) {
  if (argc < 2) { outs(2, "printf: usage: printf format [arguments]\n"); return 2; }
  const char *fmt = argv[1];
  int ai = 2, rc = 0;
  size_t cap = 256, len = 0;
  char *out = malloc(cap);
#define PUT(s, n) do { size_t _n = (n); while (len + _n + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + len, (s), _n); len += _n; } while (0)
  do {
    int consumed = 0;
    for (const char *f = fmt; *f; f++) {
      if (*f == '\\') {
        char tmp[8], e[3] = {'\\', f[1], 0};
        int n;
        if (!f[1]) { PUT("\\", 1); continue; }
        if (f[1] >= '0' && f[1] <= '7') { /* octal: up to 3 digits */
          int v = 0, k = 0;
          while (k < 3 && f[1] >= '0' && f[1] <= '7') { f++; v = v * 8 + (*f - '0'); k++; }
          char c = v;
          PUT(&c, 1);
          continue;
        }
        unescape_to(e, tmp, &n);
        PUT(tmp, n);
        f++;
        continue;
      }
      if (*f != '%') { PUT(f, 1); continue; }
      if (f[1] == '%') { PUT("%", 1); f++; continue; }
      char spec[32];
      int sn = 0;
      spec[sn++] = '%';
      f++;
      while (*f && strchr("-+ #0", *f) && sn < 20) spec[sn++] = *f++;
      while (*f && (isdigit((unsigned char)*f) || *f == '.') && sn < 28) spec[sn++] = *f++;
      if (!*f) break;
      char conv = *f;
      const char *arg = ai < argc ? argv[ai++] : NULL;
      consumed = 1;
      char buf[512];
      int n = 0;
      if (conv == 's' || conv == 'b') {
        char *tmp = NULL;
        const char *s = arg ? arg : "";
        if (conv == 'b') { int bl; tmp = malloc(strlen(s) + 1); unescape_to(s, tmp, &bl); tmp[bl] = 0; s = tmp; }
        spec[sn++] = 's'; spec[sn] = 0;
        int need = snprintf(NULL, 0, spec, s);
        char *big = malloc(need + 1);
        snprintf(big, need + 1, spec, s);
        PUT(big, need);
        free(big); free(tmp);
        continue;
      }
      if (conv == 'c') { if (arg && *arg) PUT(arg, 1); continue; }
      if (strchr("diouxX", conv)) {
        long long v = 0;
        if (arg) {
          char *e;
          if ((arg[0] == '\'' || arg[0] == '"') && arg[1]) v = (unsigned char)arg[1];
          else { v = strtoll(arg, &e, 0); if (*e) { outf(2, "printf: %s: invalid number\n", arg); rc = 1; } }
        }
        spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = conv == 'i' ? 'd' : conv; spec[sn] = 0;
        n = snprintf(buf, sizeof buf, spec, v);
      } else if (strchr("feEgG", conv)) {
        spec[sn++] = 'f'; spec[sn] = 0;
        n = snprintf(buf, sizeof buf, spec, arg ? strtod(arg, NULL) : 0.0);
      } else { outf(2, "printf: %%%c: invalid directive\n", conv); rc = 1; continue; }
      if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
      PUT(buf, n);
    }
    if (!consumed) break;
  } while (ai < argc);
  sh_write(1, out, len);
  free(out);
  return rc;
}

static int b_cd(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : getvar("HOME");
  int print = 0;
  if (!dir) { outs(2, "cd: HOME not set\n"); return 1; }
  if (!strcmp(dir, "-")) { dir = getvar("OLDPWD"); print = 1; if (!dir) { outs(2, "cd: OLDPWD not set\n"); return 1; } }
  char old[512];
  if (!getcwd(old, sizeof old)) old[0] = 0;
  if (chdir(dir) < 0) { outf(2, "cd: %s: %s\n", dir, strerror(errno)); return 1; }
  char cwd[512];
  if (getcwd(cwd, sizeof cwd)) setvar("PWD", cwd, 0);
  setvar("OLDPWD", old, 0);
  if (print) outf(1, "%s\n", cwd);
  return 0;
}
static int b_pwd(int argc, char **argv) {
  char cwd[512];
  if (!getcwd(cwd, sizeof cwd)) { outf(2, "pwd: %s\n", strerror(errno)); return 1; }
  outf(1, "%s\n", cwd);
  return 0;
}
static int b_exit(int argc, char **argv) {
  exit_code = argc > 1 ? atoi(argv[1]) & 255 : last_status;
  exit_pending = 1;
  return exit_code;
}
static int b_return(int argc, char **argv) {
  int v = argc > 1 ? atoi(argv[1]) & 255 : last_status;
  retflag = 1;
  last_status = v;
  return v;
}
static int b_break(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1;
  if (!loop_depth) { outf(2, "%s: only meaningful in a loop\n", argv[0]); return 0; }
  if (n < 1) n = 1;
  if (n > loop_depth) n = loop_depth;
  if (argv[0][0] == 'b') brk = n; else cont = n;
  return 0;
}
static int b_shift(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1;
  if (n > frame->argc) { outs(2, "shift: shift count out of range\n"); return 1; }
  frame->argc -= n;
  frame->argv += n;
  return 0;
}

/* print a value so that it can be read back by the shell */
static void put_quoted(const char *v) {
  if (*v && strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./:,+=@%") == strlen(v)) { outs(1, v); return; }
  outs(1, "'");
  for (; *v; v++) { if (*v == '\'') outs(1, "'\\''"); else sh_write(1, v, 1); }
  outs(1, "'");
}

static int b_export(int argc, char **argv) {
  int i = 1;
  if (i < argc && !strcmp(argv[i], "-p")) i++;
  if (i >= argc) { list_vars(1); return 0; }
  int rc = 0;
  for (; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
    if (!valid_name(argv[i], n)) { outf(2, "export: `%s': not a valid identifier\n", argv[i]); rc = 1; continue; }
    if (eq) { char *name = strndup(argv[i], n); setvar(name, eq + 1, 1); free(name); }
    else export_var(argv[i]);
  }
  return rc;
}
static int b_unset(int argc, char **argv) {
  int funcs = 0, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) { if (!strcmp(argv[i], "-f")) funcs = 1; else if (!strcmp(argv[i], "-v")) funcs = 0; }
  for (; i < argc; i++) {
    if (funcs) unsetfunc(argv[i]);
    else if (getvar(argv[i])) unsetvar(argv[i]);
    else unsetfunc(argv[i]);
  }
  return 0;
}
static int b_local(int argc, char **argv) {
  if (!in_func) { outs(2, "local: can only be used in a function\n"); return 1; }
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    char *name = eq ? strndup(argv[i], eq - argv[i]) : strdup(argv[i]);
    local_var(name);
    if (eq) setvar(name, eq + 1, 0);
    free(name);
  }
  return 0;
}
static int b_set(int argc, char **argv) {
  if (argc == 1) { list_vars(0); return 0; }
  int i = 1;
  for (; i < argc; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
    for (char *f = a + 1; *f; f++) {
      if (*f == 'x') opt_x = a[0] == '-';
      else { outf(2, "set: -%c: option not supported\n", *f); return 2; }
    }
  }
  if (i < argc || (argc > 1 && !strcmp(argv[argc - 1], "--"))) {
    int n = argc - i;
    char **v = calloc(n + 1, sizeof *v);
    for (int k = 0; k < n; k++) v[k] = strdup(argv[i + k]);
    frame->argc = n;
    frame->argv = v; /* the old vector is left alone: a caller may still hold it */
  }
  return 0;
}
static int b_alias(int argc, char **argv) {
  if (argc == 1) { list_aliases(); return 0; }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) { char *name = strndup(argv[i], eq - argv[i]); setalias(name, eq + 1); free(name); }
    else {
      char *v = getalias(argv[i]);
      if (v) { outf(1, "alias %s=", argv[i]); put_quoted(v); outs(1, "\n"); }
      else { outf(2, "alias: %s: not found\n", argv[i]); rc = 1; }
    }
  }
  return rc;
}
static int b_unalias(int argc, char **argv) {
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) { unalias_all(); continue; }
    if (unalias(argv[i]) < 0) { outf(2, "unalias: %s: not found\n", argv[i]); rc = 1; }
  }
  return rc;
}
static int b_source(int argc, char **argv) {
  if (argc < 2) { outf(2, "%s: filename argument required\n", argv[0]); return 2; }
  char *path = strchr(argv[1], '/') ? strdup(argv[1]) : find_in_path(argv[1]);
  if (!path) path = strdup(argv[1]);
  size_t len;
  char *text = read_file(path, &len);
  if (!text) { outf(2, "%s: %s: %s\n", argv[0], argv[1], strerror(errno)); free(path); return 1; }
  struct frame f, *saved = frame;
  if (argc > 2) { f.argc = argc - 2; f.argv = argv + 2; f.locals = NULL; f.up = frame->up; frame = &f; }
  int st = run_text(text, argv[1]);
  if (argc > 2) { saved->locals = f.locals ? f.locals : saved->locals; frame = saved; }
  if (retflag && !in_func) retflag = 0;
  free(text);
  free(path);
  return st;
}
static int b_eval(int argc, char **argv) {
  size_t n = 1;
  for (int i = 1; i < argc; i++) n += strlen(argv[i]) + 1;
  char *s = malloc(n);
  s[0] = 0;
  for (int i = 1; i < argc; i++) { strcat(s, argv[i]); if (i + 1 < argc) strcat(s, " "); }
  int st = run_text(s, "eval");
  free(s);
  return st;
}

static int b_read(int argc, char **argv) {
  int raw = 0, i = 1;
  const char *prompt = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-r")) raw = 1;
    else if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else { outf(2, "read: %s: invalid option\n", argv[i]); return 2; }
  }
  if (prompt) outs(2, prompt);
  size_t cap = 128, len = 0;
  char *line = malloc(cap);
  unsigned char *lit = malloc(cap); /* 1 = escaped by a backslash: never a separator */
  int got_eof = 1;
  char c;
  while (read(0, &c, 1) == 1) {
    if (c == '\n') { got_eof = 0; break; }
    int esc = 0;
    if (c == '\\' && !raw) {
      if (read(0, &c, 1) != 1) break;
      if (c == '\n') continue;
      esc = 1;
    }
    if (len + 2 > cap) { cap *= 2; line = realloc(line, cap); lit = realloc(lit, cap); }
    lit[len] = esc;
    line[len++] = c;
  }
  line[len] = 0;
  const char *ifs = getvar("IFS");
  if (!ifs) ifs = " \t\n";
  int nvars = argc - i;
  char *def[] = {"REPLY"};
  char **vars = nvars ? argv + i : def;
  if (!nvars) nvars = 1;
#define ISSEP(k) (!lit[k] && strchr(ifs, line[k]) && line[k])
#define ISWS(k) (ISSEP(k) && isspace((unsigned char)line[k]))
  size_t k = 0;
  if (nvars == 1 && !argv[i]) { setvar("REPLY", line, 0); } /* REPLY keeps the line as is */
  else {
    for (int v = 0; v < nvars; v++) {
      while (k < len && ISWS(k)) k++;
      size_t start = k, end;
      if (v == nvars - 1) {
        end = len;
        while (end > start && ISWS(end - 1)) end--;
      } else {
        while (k < len && !ISSEP(k)) k++;
        end = k;
        if (k < len) { k++; while (k < len && ISWS(k)) k++; }
      }
      char *val = strndup(line + start, end - start);
      setvar(vars[v], val, 0);
      free(val);
    }
  }
  free(line);
  free(lit);
  return got_eof;
}

/* ---- test / [ ---- */
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
static int b_test(int argc, char **argv) {
  if (!strcmp(argv[0], "[")) {
    if (strcmp(argv[argc - 1], "]")) { outs(2, "[: missing `]'\n"); return 2; }
    argc--;
  }
  targ = argv + 1; tn = argc - 1; tpos = 0; terr = 0;
  if (!tn) return 1;
  int v;
  if (tn == 1) v = targ[0][0] != 0;
  else if (tn == 2 && !strcmp(targ[0], "!")) v = !targ[1][0];
  else v = t_or();
  if (!terr && tpos < tn) { outf(2, "test: %s: unexpected argument\n", targ[tpos]); terr = 1; }
  return terr ? 2 : !v;
}

static int b_type(int argc, char **argv) {
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    char *n = argv[i], *a, *p;
    if ((a = getalias(n))) outf(1, "%s is aliased to `%s'\n", n, a);
    else if (is_special_word(n) || !strcmp(n, "if") || !strcmp(n, "while") || !strcmp(n, "for") || !strcmp(n, "case") ||
             !strcmp(n, "until") || !strcmp(n, "{") || !strcmp(n, "!") || !strcmp(n, "in"))
      outf(1, "%s is a shell keyword\n", n);
    else if (getfunc(n)) outf(1, "%s is a function\n", n);
    else if (find_builtin(n)) outf(1, "%s is a shell builtin\n", n);
    else if ((p = find_in_path(n))) { outf(1, "%s is %s\n", n, p); free(p); }
    else { outf(2, "type: %s: not found\n", n); rc = 1; }
  }
  return rc;
}
static int b_history(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "-c")) { for (int i = 0; i < nhist; i++) free(hist[i]); nhist = 0; return 0; }
  int start = 0;
  if (argc > 1) { int n = atoi(argv[1]); if (n > 0 && n < nhist) start = nhist - n; }
  for (int i = start; i < nhist; i++) outf(1, "%5d  %s\n", i + 1, hist[i]);
  return 0;
}
static int b_exec(int argc, char **argv) {
  if (argc == 1) { exec_keep = 1; return 0; } /* exec >file: the redirections stay */
  outf(2, "sh: exec: cannot replace the shell with %s: this system has no exec (run it without exec)\n", argv[1]);
  return 126;
}
static int b_wait(int argc, char **argv) {
  if (argc == 1) return wait_jobs();
  int st = 0;
  for (int i = 1; i < argc; i++) {
    if (waitpid(atoi(argv[i]), &st, 0) < 0) { outf(2, "wait: pid %s is not a child of this shell\n", argv[i]); st = 127 << 8; }
  }
  reap_jobs(0);
  return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
static int b_jobs(int argc, char **argv) { reap_jobs(1); return 0; }
static int b_umask(int argc, char **argv) { if (argc == 1) outs(1, "0022\n"); return 0; }
static int b_help(int argc, char **argv);

static const struct { const char *name; builtin_fn fn; } builtins[] = {
  {":", b_true}, {".", b_source}, {"[", b_test}, {"alias", b_alias}, {"break", b_break}, {"cd", b_cd},
  {"continue", b_break}, {"echo", b_echo}, {"eval", b_eval}, {"exec", b_exec}, {"exit", b_exit},
  {"export", b_export}, {"false", b_false}, {"help", b_help}, {"history", b_history}, {"jobs", b_jobs},
  {"local", b_local}, {"printf", b_printf}, {"pwd", b_pwd}, {"read", b_read}, {"return", b_return},
  {"set", b_set}, {"shift", b_shift}, {"source", b_source}, {"test", b_test}, {"true", b_true},
  {"type", b_type}, {"umask", b_umask}, {"unalias", b_unalias}, {"unset", b_unset}, {"wait", b_wait},
};
#define NBUILTINS (int)(sizeof builtins / sizeof builtins[0])
static int b_help(int argc, char **argv) {
  outs(1, "sh, the UC.OS shell. Builtin commands:\n");
  for (int i = 0; i < NBUILTINS; i++) outf(1, "  %-10s%s", builtins[i].name, i % 6 == 5 ? "\n" : "");
  outs(1, "\nSee man sh for the full description.\n");
  return 0;
}
builtin_fn find_builtin(const char *name) {
  for (int i = 0; i < NBUILTINS; i++) if (!strcmp(builtins[i].name, name)) return builtins[i].fn;
  return NULL;
}
const char *builtin_name(int i) { return i < NBUILTINS ? builtins[i].name : NULL; }
