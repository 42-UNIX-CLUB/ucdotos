// SPDX-License-Identifier: GPL-2.0-or-later
/* make: options, MAKEFLAGS, start-up, remaking makefiles and the goals. */
#include "make.h"
#include <unistd.h>

const char *progname = "make";
int makelevel;
int opt_n, opt_s, opt_k, opt_i, opt_B, opt_q, opt_t, opt_r, opt_R, opt_e, opt_p, opt_d, opt_w, opt_no_w, opt_trace,
    opt_warn_undef, opt_j_given, opt_print_dir, opt_no_silent;
static int opt_l_given;
static char *opt_l_val, *opt_O_val, *opt_debug_val;
static int opt_C_given;
char *make_cwd;
strlist include_dirs;
char *make_path;
int restarts;
static strlist cdirs, makefile_args, goal_args, cmdline_vars, evals, old_files, new_files;
static int entered;
static char **orig_argv;
static char *start_dir;
extern int question_out;
extern const char *builtin_vars, *builtin_rules;
int reading_builtin;

/* ---- messages ---- */
char *prefix_str(void) {
  static char b[128];
  if (makelevel == 0) snprintf(b, sizeof b, "%s", progname);
  else snprintf(b, sizeof b, "%s[%d]", progname, makelevel);
  return b;
}
static void vmsg(FILE *f, const char *file, int line, const char *pre, const char *fmt, va_list ap) {
  fflush(stdout);
  if (file && *file) {
    if (line > 0) fprintf(f, "%s:%d: ", file, line);
    else fprintf(f, "%s: ", file);
  } else fprintf(f, "%s: ", prefix_str());
  fputs(pre, f);
  vfprintf(f, fmt, ap);
  fflush(f);
}
void fatal_at(const char *file, int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vmsg(stderr, file, line, "*** ", fmt, ap);
  va_end(ap);
  fputs(".  Stop.\n", stderr);
  make_exit(2);
}
void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vmsg(stderr, 0, 0, "*** ", fmt, ap);
  va_end(ap);
  fputs(".  Stop.\n", stderr);
  make_exit(2);
}
void error_at(const char *file, int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vmsg(stderr, file, line, "*** ", fmt, ap);
  va_end(ap);
  fputs(".\n", stderr);
}
void warn_at(const char *file, int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vmsg(stderr, file, line, "", fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}
void msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("%s: ", prefix_str());
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}
void errmsg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fflush(stdout);
  fprintf(stderr, "%s: ", prefix_str());
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}
void enter_dir_msg(int leaving) {
  if (!opt_print_dir) return;
  if (!leaving) entered = 1;
  else if (!entered) return;
  printf("%s: %s directory '%s'\n", prefix_str(), leaving ? "Leaving" : "Entering", make_cwd);
  fflush(stdout);
}
void make_exit(int code) {
  die_cleanup();
  fflush(stdout);
  enter_dir_msg(1);
  fflush(stdout);
  fflush(stderr);
  exit(code);
}

static void usage(FILE *f) {
  fprintf(f, "Usage: %s [options] [target] ...\n", progname);
  fputs("Options:\n"
        "  -B, --always-make            remake every target\n"
        "  -C DIR, --directory=DIR      change to DIR first\n"
        "  -d, --debug[=FLAGS]          print debugging information\n"
        "  -e, --environment-overrides  the environment wins over makefiles\n"
        "  -E TEXT, --eval=TEXT         read TEXT as a makefile line\n"
        "  -f FILE, --file=FILE         read FILE as the makefile\n"
        "  -h, --help                   show this help\n"
        "  -i, --ignore-errors          ignore errors from recipes\n"
        "  -I DIR, --include-dir=DIR    look for included makefiles in DIR\n"
        "  -j [N], --jobs[=N]           run N recipes at once (no N: no limit)\n"
        "  -k, --keep-going             go on with other targets after an error\n"
        "  -l [N], --load-average[=N]   accepted, ignored\n"
        "  -n, --just-print, --dry-run  print recipes, run nothing\n"
        "  -o FILE, --old-file=FILE     treat FILE as very old\n"
        "  -O[TYPE], --output-sync      accepted, ignored\n"
        "  -p, --print-data-base        print the rules and variables\n"
        "  -q, --question               run nothing; status 1 if out of date\n"
        "  -r, --no-builtin-rules       no built-in rules\n"
        "  -R, --no-builtin-variables   no built-in variables\n"
        "  -s, --silent, --quiet        do not echo recipes\n"
        "  -S, --no-keep-going, --stop  turn off -k\n"
        "  -t, --touch                  touch targets instead of remaking them\n"
        "  --trace                      say why each target is remade\n"
        "  -v, --version                print the version\n"
        "  -w, --print-directory        print the working directory\n"
        "  --no-print-directory         do not print it\n"
        "  -W FILE, --what-if=FILE      treat FILE as just modified\n"
        "  --warn-undefined-variables   warn about undefined variables\n", f);
}

/* ---- options ---- */
struct lopt { const char *name; int arg; int id; }; /* arg: 0 none, 1 required, 2 optional */
enum { L_B = 1, L_C, L_DEBUG, L_E, L_EVAL, L_F, L_H, L_I, L_INC, L_J, L_K, L_L, L_N, L_O, L_OSYNC, L_P, L_Q, L_R, L_RR,
       L_S, L_NOSILENT, L_SS, L_T, L_TRACE, L_V, L_W, L_NOW, L_WW, L_WARN, L_CHECKSYM, L_JOBSERVER, L_SHUFFLE, L_JSSTYLE };
static const struct lopt lopts[] = {
  {"always-make", 0, L_B}, {"directory", 1, L_C}, {"debug", 2, L_DEBUG}, {"environment-overrides", 0, L_E},
  {"eval", 1, L_EVAL}, {"file", 1, L_F}, {"makefile", 1, L_F}, {"help", 0, L_H}, {"ignore-errors", 0, L_I},
  {"include-dir", 1, L_INC}, {"jobs", 2, L_J}, {"keep-going", 0, L_K}, {"load-average", 2, L_L}, {"max-load", 2, L_L},
  {"just-print", 0, L_N}, {"dry-run", 0, L_N}, {"recon", 0, L_N}, {"old-file", 1, L_O}, {"assume-old", 1, L_O},
  {"output-sync", 2, L_OSYNC}, {"print-data-base", 0, L_P}, {"question", 0, L_Q}, {"no-builtin-rules", 0, L_R},
  {"no-builtin-variables", 0, L_RR}, {"silent", 0, L_S}, {"quiet", 0, L_S}, {"no-silent", 0, L_NOSILENT},
  {"no-keep-going", 0, L_SS}, {"stop", 0, L_SS}, {"touch", 0, L_T}, {"trace", 0, L_TRACE}, {"version", 0, L_V},
  {"print-directory", 0, L_W}, {"no-print-directory", 0, L_NOW}, {"what-if", 1, L_WW}, {"new-file", 1, L_WW},
  {"assume-new", 1, L_WW}, {"warn-undefined-variables", 0, L_WARN}, {"check-symlink-times", 0, L_CHECKSYM},
  {"jobserver-auth", 1, L_JOBSERVER}, {"jobserver-fds", 1, L_JOBSERVER}, {"shuffle", 2, L_SHUFFLE},
  {"jobserver-style", 1, L_JSSTYLE}, {0, 0, 0}};

static void apply_opt(int id, const char *arg, int from_env) {
  switch (id) {
  case L_B: opt_B = 1; break;
  case L_C:
    if (from_env) break;
    if (!*arg) break;
    sl_add(&cdirs, xstrdup(arg));
    opt_C_given = 1;
    break;
  case L_DEBUG: opt_d = 1; free(opt_debug_val); opt_debug_val = arg ? xstrdup(arg) : 0; break;
  case L_E: opt_e = 1; break;
  case L_EVAL: if (!from_env) sl_add(&evals, xstrdup(arg)); break;
  case L_F: if (!from_env) sl_add(&makefile_args, xstrdup(arg)); break;
  case L_H: usage(stdout); exit(0);
  case L_I: opt_i = 1; break;
  case L_INC:
    for (int i = 0; i < include_dirs.n; i++) if (!strcmp(include_dirs.v[i], arg)) return;
    sl_add(&include_dirs, xstrdup(arg));
    break;
  case L_J:
    opt_j_given = 1;
    if (arg && *arg) {
      char *e;
      long n = strtol(arg, &e, 10);
      if (*e || n < 0) { fprintf(stderr, "%s: the '-j' option requires a positive integer argument\n", progname); usage(stderr); exit(2); }
      job_slots = (int)n;
    } else job_slots = 0;
    break;
  case L_K: opt_k = 1; break;
  case L_L: opt_l_given = 1; free(opt_l_val); opt_l_val = arg ? xstrdup(arg) : 0; break;
  case L_N: opt_n = 1; break;
  case L_O: if (!from_env) sl_add(&old_files, xstrdup(arg)); break;
  case L_OSYNC: free(opt_O_val); opt_O_val = xstrdup(arg && *arg ? arg : "target"); break;
  case L_P: opt_p = 1; break;
  case L_Q: opt_q = 1; break;
  case L_R: opt_r = 1; break;
  case L_RR: opt_R = 1; opt_r = 1; break;
  case L_S: opt_s = 1; break;
  case L_NOSILENT: opt_s = 0; opt_no_silent = 1; break;
  case L_SS: opt_k = 0; break;
  case L_T: opt_t = 1; break;
  case L_TRACE: opt_trace = 1; break;
  case L_V:
    printf("%s 0.1 (UC.OS make, aims at GNU Make 4.4.1 compatibility)\n"
           "License GPLv2+: GNU GPL version 2 or later <https://gnu.org/licenses/gpl.html>\n"
           "This is free software: you are free to change and redistribute it.\n", progname);
    exit(0);
  case L_W: opt_w = 1; break;
  case L_NOW: opt_no_w = 1; break;
  case L_WW: if (!from_env) sl_add(&new_files, xstrdup(arg)); break;
  case L_WARN: opt_warn_undef = 1; break;
  default: break;
  }
}

static int short_id(char c, int *arg) {
  *arg = 0;
  switch (c) {
  case 'b': case 'm': return -1;
  case 'B': return L_B;
  case 'C': *arg = 1; return L_C;
  case 'd': return L_DEBUG;
  case 'e': return L_E;
  case 'E': *arg = 1; return L_EVAL;
  case 'f': *arg = 1; return L_F;
  case 'h': return L_H;
  case 'i': return L_I;
  case 'I': *arg = 1; return L_INC;
  case 'j': *arg = 2; return L_J;
  case 'k': return L_K;
  case 'l': *arg = 2; return L_L;
  case 'L': return L_CHECKSYM;
  case 'n': return L_N;
  case 'o': *arg = 1; return L_O;
  case 'O': *arg = 3; return L_OSYNC;
  case 'p': return L_P;
  case 'q': return L_Q;
  case 'r': return L_R;
  case 'R': return L_RR;
  case 's': return L_S;
  case 'S': return L_SS;
  case 't': return L_T;
  case 'v': return L_V;
  case 'w': return L_W;
  case 'W': *arg = 1; return L_WW;
  }
  return 0;
}

static int is_assignment_arg(const char *a) {
  size_t ne, vs;
  int op;
  return find_assign(a, &ne, &vs, &op) && ne > 0;
}

/* Parse ARGV. from_env: words decoded from MAKEFLAGS. */
static void parse_args(int argc, char **argv, int from_env) {
  int noopts = 0;
  for (int i = 0; i < argc; i++) {
    char *a = argv[i];
    if (noopts || a[0] != '-' || !a[1]) {
      if (is_assignment_arg(a)) sl_add(&cmdline_vars, xstrdup(a));
      else if (!from_env) sl_add(&goal_args, xstrdup(a));
      continue;
    }
    if (!strcmp(a, "--")) { noopts = 1; continue; }
    if (a[1] == '-') {
      const char *name = a + 2;
      const char *eq = strchr(name, '=');
      size_t nl = eq ? (size_t)(eq - name) : strlen(name);
      const struct lopt *hit = 0;
      int nhit = 0;
      for (const struct lopt *o = lopts; o->name; o++) {
        if (strlen(o->name) == nl && !strncmp(o->name, name, nl)) { hit = o; nhit = 1; break; }
        if (!strncmp(o->name, name, nl)) { if (!hit || hit->id != o->id) nhit++; hit = o; }
      }
      if (!hit || nhit != 1) {
        if (from_env) continue;
        fprintf(stderr, nhit > 1 ? "%s: option '%s' is ambiguous\n" : "%s: unrecognized option '%s'\n", progname, a);
        usage(stderr);
        exit(2);
      }
      const char *arg = 0;
      if (hit->arg == 1) {
        if (eq) arg = eq + 1;
        else if (i + 1 < argc) arg = argv[++i];
        else { fprintf(stderr, "%s: option '%s' requires an argument\n", progname, a); usage(stderr); exit(2); }
      } else if (hit->arg == 2) arg = eq ? eq + 1 : 0;
      else if (eq) { fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", progname, hit->name); usage(stderr); exit(2); }
      apply_opt(hit->id, arg, from_env);
      continue;
    }
    for (char *p = a + 1; *p; p++) {
      int ak;
      int id = short_id(*p, &ak);
      if (id < 0) continue;
      if (!id) {
        if (from_env) continue;
        fprintf(stderr, "%s: invalid option -- '%c'\n", progname, *p);
        usage(stderr);
        exit(2);
      }
      if (ak == 1) {
        const char *arg;
        if (p[1]) arg = p + 1;
        else if (i + 1 < argc) arg = argv[++i];
        else { fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, *p); usage(stderr); exit(2); }
        apply_opt(id, arg, from_env);
        break;
      }
      if (ak == 2) { /* optional number: attached, or the next word if it is a number */
        const char *arg = 0;
        if (p[1]) {
          if (isdigit((unsigned char)p[1]) || p[1] == '.') { arg = p + 1; apply_opt(id, arg, from_env); break; }
        } else if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) arg = argv[++i];
        apply_opt(id, arg, from_env);
        if (arg) break;
        continue;
      }
      if (ak == 3) { apply_opt(id, p[1] ? p + 1 : 0, from_env); break; }
      apply_opt(id, 0, from_env);
    }
  }
}

/* split MAKEFLAGS into words; "\ " is an escaped space */
static void parse_makeflags(const char *s, int from_makefile) {
  strlist w = {0};
  const char *p = s;
  while (*p) {
    while (is_space(*p)) p++;
    if (!*p) break;
    buf b = {0};
    while (*p && !is_space(*p)) {
      if (*p == '\\' && p[1]) { bputc(&b, p[1]); p += 2; continue; }
      bputc(&b, *p++);
    }
    sl_add(&w, bdetach(&b));
  }
  if (!w.n) { free(w.v); return; }
  /* the first word may be single-letter flags without a dash */
  if (w.v[0][0] != '-' && !strchr(w.v[0], '=')) {
    char *nw = xconcat("-", w.v[0], 0);
    free(w.v[0]);
    w.v[0] = nw;
  }
  /* after "--" come variable definitions */
  strlist vars = {0};
  int k;
  for (k = 0; k < w.n; k++) if (!strcmp(w.v[k], "--")) break;
  for (int j = k + 1; j < w.n; j++) sl_add(&vars, xstrdup(w.v[j]));
  int saved = cmdline_vars.n;
  parse_args(k, w.v, 1);
  (void)from_makefile;
  /* variables from MAKEFLAGS come before the ones on our command line */
  for (int j = vars.n - 1; j >= 0; j--) {
    sl_add(&cmdline_vars, 0);
    memmove(&cmdline_vars.v[1], &cmdline_vars.v[0], (cmdline_vars.n - 1) * sizeof(char *));
    cmdline_vars.v[0] = vars.v[j];
    vars.v[j] = 0;
  }
  (void)saved;
  free(vars.v);
  sl_free(&w);
}

static void escape_into(buf *b, const char *s) {
  for (; *s; s++) {
    if (*s == ' ' || *s == '\t' || *s == '\\') bputc(b, '\\');
    bputc(b, *s);
  }
}
void make_flags_string(buf *out, int for_mflags) {
  buf letters = {0};
  if (opt_B) bputc(&letters, 'B');
  if (opt_d && !opt_debug_val) bputc(&letters, 'd');
  if (opt_e) bputc(&letters, 'e');
  if (opt_i) bputc(&letters, 'i');
  if (opt_k) bputc(&letters, 'k');
  if (opt_n) bputc(&letters, 'n');
  if (opt_p) bputc(&letters, 'p');
  if (opt_q) bputc(&letters, 'q');
  if (opt_r) bputc(&letters, 'r');
  if (opt_R) bputc(&letters, 'R');
  if (opt_s) bputc(&letters, 's');
  if (opt_t) bputc(&letters, 't');
  if (opt_w) bputc(&letters, 'w');
  strlist rest = {0};
  for (int i = 0; i < include_dirs.n; i++) sl_add(&rest, xconcat("-I", include_dirs.v[i], 0));
  if (opt_j_given) {
    if (job_slots == 0) sl_add(&rest, xstrdup("-j"));
    else { char t[32]; snprintf(t, sizeof t, "-j%d", job_slots); sl_add(&rest, xstrdup(t)); }
  }
  if (opt_l_given) sl_add(&rest, opt_l_val ? xconcat("-l", opt_l_val, 0) : xstrdup("-l"));
  if (opt_O_val) sl_add(&rest, xconcat("-O", opt_O_val, 0));
  if (opt_debug_val) sl_add(&rest, xconcat("--debug=", opt_debug_val, 0));
  if (opt_no_w) sl_add(&rest, xstrdup("--no-print-directory"));
  if (opt_trace) sl_add(&rest, xstrdup("--trace"));
  if (opt_warn_undef) sl_add(&rest, xstrdup("--warn-undefined-variables"));
  if (for_mflags) {
    int first = 1;
    if (letters.n) { bputc(out, '-'); bput(out, letters.s, letters.n); first = 0; }
    for (int i = 0; i < rest.n; i++) { if (!first) bputc(out, ' '); bputs(out, rest.v[i]); first = 0; }
  } else {
    if (letters.n) bput(out, letters.s, letters.n);
    for (int i = 0; i < rest.n; i++) { bputc(out, ' '); bputs(out, rest.v[i]); }
    if (cmdline_vars.n) {
      bputs(out, " --");
      for (int i = cmdline_vars.n - 1; i >= 0; i--) { bputc(out, ' '); escape_into(out, cmdline_vars.v[i]); }
    }
  }
  sl_free(&rest);
  bfree(&letters);
}
static void define_makeflags(void) {
  buf b = {0};
  make_flags_string(&b, 0);
  var *v = var_set(&globals, "MAKEFLAGS", bstr(&b), F_RECURSIVE, O_FILE);
  v->exp = EXP_YES;
  bfree(&b);
  make_flags_string(&b, 1);
  var_set(&globals, "MFLAGS", bstr(&b), F_RECURSIVE, O_FILE);
  bfree(&b);
}

/* ---- the database printout (-p) ---- */
static int cmpvar(const void *a, const void *b) { return strcmp((*(var *const *)a)->name, (*(var *const *)b)->name); }
static void print_db(void) {
  static const char *on[] = {"default", "environment", "makefile", "environment under -e", "command line", "'override' directive", "automatic"};
  printf("\n# %s data base, printed by UC.OS make\n\n# Variables\n\n", progname);
  var **vs = 0;
  int n = 0;
  for (unsigned i = 0; i < globals.h.cap; i++) {
    var *v = globals.h.t[i].val;
    if (!globals.h.t[i].key || !v) continue;
    vs = xrealloc(vs, (n + 1) * sizeof *vs);
    vs[n++] = v;
  }
  qsort(vs, n, sizeof *vs, cmpvar);
  for (int i = 0; i < n; i++) {
    printf("# %s\n%s %s %s\n", on[vs[i]->origin], vs[i]->name, vs[i]->flavor == F_SIMPLE ? ":=" : "=", vs[i]->value);
  }
  free(vs);
  printf("\n# Files\n\n");
  strlist names = {0};
  for (unsigned i = 0; i < files.cap; i++) if (files.t[i].key && files.t[i].val) sl_add(&names, xstrdup(files.t[i].key));
  qsort(names.v, names.n, sizeof *names.v, str_cmp);
  for (int i = 0; i < names.n; i++) {
    file *f = file_lookup(names.v[i]);
    if (!f->is_target && !f->cmds) { printf("# Not a target:\n"); }
    for (file *e = f; e; e = e->dcnext) {
      printf("%s:%s", f->name, f->dc ? ":" : "");
      for (dep *d = e->deps; d; d = d->next) if (d->f && !d->oo) printf(" %s", d->f->name);
      int oo = 0;
      for (dep *d = e->deps; d; d = d->next) if (d->f && d->oo) { if (!oo) printf(" |"); oo = 1; printf(" %s", d->f->name); }
      putchar('\n');
      if (f->phony) printf("#  Phony target (prerequisite of .PHONY).\n");
      if (e->cmds) for (int k = 0; k < e->cmds->n; k++) printf("\t%s\n", e->cmds->lines[k]);
      putchar('\n');
    }
  }
  sl_free(&names);
  printf("# Implicit Rules\n\n");
  for (int i = 0; i < nrules; i++) {
    rule *r = rules[i];
    for (int t = 0; t < r->nt; t++) printf("%s%s", t ? " " : "", r->targets[t]);
    printf(":%s", r->terminal ? ":" : "");
    for (dep *d = r->deps; d; d = d->next) printf(" %s%s", d->oo ? "| " : "", d->name);
    putchar('\n');
    if (r->cmds) for (int k = 0; k < r->cmds->n; k++) printf("\t%s\n", r->cmds->lines[k]);
    putchar('\n');
  }
  printf("# finished printing the data base\n");
  fflush(stdout);
}

/* ---- start-up ---- */
static void import_env(void) {
  for (char **e = environ; e && *e; e++) {
    const char *eq = strchr(*e, '=');
    if (!eq || eq == *e) continue;
    char *name = xstrndup(*e, eq - *e);
    if (!strcmp(name, "SHELL") || !strcmp(name, "MAKEFLAGS") || !strcmp(name, "MFLAGS") || !strcmp(name, "MAKELEVEL") ||
        !strcmp(name, "MAKE_RESTARTS") || !strcmp(name, "MAKEOVERRIDES") || !strcmp(name, "CURDIR")) {
      if (!strcmp(name, "MAKELEVEL")) makelevel = atoi(eq + 1);
      if (!strcmp(name, "MAKE_RESTARTS")) restarts = atoi(eq + 1);
      free(name);
      continue;
    }
    var *v = var_set(&globals, name, eq + 1, F_RECURSIVE, O_ENV);
    v->exp = EXP_YES; /* stays exported even if the makefile sets it */
    free(name);
  }
}

static void define_cmdline_vars(void) {
  for (int i = 0; i < cmdline_vars.n; i++) {
    const char *a = cmdline_vars.v[i];
    size_t ne, vs;
    int op;
    if (!find_assign(a, &ne, &vs, &op)) continue;
    size_t e = ne;
    while (e > 0 && is_blank(a[e - 1])) e--;
    char *name = xstrndup(a, e);
    var *old = var_lookup_global(name, strlen(name));
    if (old && old->origin < O_COMMAND && op == '+') { /* += on the command line appends to nothing older */
      var_set(&globals, name, "", F_RECURSIVE, O_COMMAND);
    }
    if (old && old->origin < O_COMMAND && op != '+') { free(old->value); old->value = xstrdup(""); old->vlen = 0; old->vcap = 0; old->origin = O_COMMAND; }
    var_assign(&globals, name, a + vs, op, O_COMMAND, 0);
    free(name);
  }
}

static void read_builtin(void) {
  reading_builtin = 1;
  if (!opt_R) read_text(builtin_vars, "<builtin>", 0);
  if (!opt_r) read_text(builtin_rules, "<builtin>", 0);
  reading_builtin = 0;
}

static file **collect_goals(int *n) {
  file **g = 0;
  *n = 0;
  if (goal_args.n) {
    g = xmalloc(goal_args.n * sizeof *g);
    for (int i = 0; i < goal_args.n; i++) { g[i] = file_enter(goal_args.v[i]); g[i]->is_goal = 1; }
    *n = goal_args.n;
    return g;
  }
  var *dg = var_lookup_global(".DEFAULT_GOAL", 13);
  if (dg) {
    char *v = var_value(dg);
    const char *p = v;
    size_t len;
    const char *w;
    int cnt = 0;
    while ((w = next_word(&p, &len))) {
      if (cnt++) { free(v); fatal(".DEFAULT_GOAL contains more than one target"); }
      g = xmalloc(sizeof *g);
      g[0] = file_entern(w, len);
      g[0]->is_goal = 1;
      *n = 1;
    }
    free(v);
  }
  return g;
}

/* After reading: make sure every makefile is up to date; if one was remade,
 * start again from the beginning. */
static void remake_makefiles(void) {
  int n = makefiles_read.n + nmissing_incs;
  if (!n) return;
  file **mf = xmalloc(n * sizeof *mf);
  int k = 0;
  mtime_t *before = xmalloc(n * sizeof *before);
  for (int i = 0; i < makefiles_read.n; i++) {
    file *f = file_lookup(makefiles_read.v[i]);
    if (!f) continue;
    /* a makefile that is the target of a double-colon rule without
     * prerequisites would be remade every time: skip it */
    if (f->dc && !f->deps && f->cmds) continue;
    f->dontcare = 1;
    mf[k++] = f;
  }
  for (int i = 0; i < nmissing_incs; i++) {
    file *f = file_lookup(missing_incs[i].name);
    if (!f) continue;
    int dup = 0;
    for (int j = 0; j < k; j++) if (mf[j] == f) dup = 1;
    if (dup) continue;
    f->dontcare = 1;
    mf[k++] = f;
  }
  for (int i = 0; i < k; i++) before[i] = file_mtime(mf[i]);
  int save_started = commands_started;
  update_goals(mf, k, 1);
  int remade = 0;
  for (int i = 0; i < k; i++) {
    file *f = mf[i];
    if (!f->ran) continue;
    stat_forget(f->name);
    mtime_t now = stat_mtime(f->name);
    if (now != MT_MISSING && (now != before[i] || before[i] == MT_MISSING)) remade = 1;
  }
  /* mandatory includes that still do not exist are an error */
  for (int i = 0; i < nmissing_incs; i++) {
    if (missing_incs[i].optional) continue;
    file *f = file_lookup(missing_incs[i].name);
    if (f && f->ran && stat_mtime(f->name) != MT_MISSING) continue;
    if (remade) continue;
    fflush(stdout);
    if (missing_incs[i].from)
      fprintf(stderr, "%s:%d: %s: %s\n", missing_incs[i].from, missing_incs[i].line, missing_incs[i].name, strerror(ENOENT));
    else
      fprintf(stderr, "%s: %s: %s\n", prefix_str(), missing_incs[i].name, strerror(ENOENT));
    if (f && !f->cmds && !f->is_target && !(f->state == ST_DONE && f->status && f->ran)) {
      fprintf(stderr, "%s: *** No rule to make target '%s'.  Stop.\n", prefix_str(), missing_incs[i].name);
    }
    flush_deferred_msgs();
    fflush(stderr);
    make_exit(2);
  }
  flush_deferred_msgs();
  (void)save_started;
  if (remade) {
    fflush(stdout);
    fflush(stderr);
    char rs[16];
    snprintf(rs, sizeof rs, "%d", restarts + 1);
    setenv("MAKE_RESTARTS", rs, 1);
    if (start_dir) sys_chdir(start_dir);
    if (entered) { /* the new make prints its own Entering line */ }
    if (opt_print_dir && entered) { printf("%s: Leaving directory '%s'\n", prefix_str(), make_cwd); fflush(stdout); }
    sys_reexec(orig_argv);
  }
  free(mf);
  free(before);
}

int main(int argc, char **argv) {
  orig_argv = argv;
  const char *a0 = argv[0] && *argv[0] ? argv[0] : "make";
  const char *slash = strrchr(a0, '/');
  progname = slash ? slash + 1 : a0;
  start_dir = sys_getcwd();
  /* MAKEFLAGS from the environment first, then the command line */
  const char *envmf = getenv("MAKEFLAGS");
  const char *envlvl = getenv("MAKELEVEL");
  if (envlvl) makelevel = atoi(envlvl);
  if (envmf) parse_makeflags(envmf, 0);
  parse_args(argc - 1, argv + 1, 0);
  opt_print_dir = !opt_no_w && (opt_w || (!opt_s && (opt_C_given || makelevel > 0)));
  for (int i = 0; i < cdirs.n; i++) {
    if (sys_chdir(cdirs.v[i]) != 0) {
      int e = errno;
      make_cwd = sys_getcwd();
      enter_dir_msg(0);
      fflush(stdout);
      fprintf(stderr, "%s: *** %s: %s.  Stop.\n", prefix_str(), cdirs.v[i], strerror(e));
      make_exit(2);
    }
  }
  make_cwd = sys_getcwd();
  sys_install_signals();

  /* $(MAKE): argv[0], made absolute when it is a relative path with a slash */
  if (strchr(a0, '/') && a0[0] != '/') make_path = xconcat(start_dir, "/", a0);
  else make_path = xstrdup(a0);

  import_env();
  read_builtin();
  var_define("MAKE_COMMAND", make_path, F_SIMPLE, O_DEFAULT);
  var_define("MAKE", "$(MAKE_COMMAND)", F_RECURSIVE, O_DEFAULT);
  var_define("MAKE_VERSION", "4.4.1", F_SIMPLE, O_DEFAULT);
  var_define("MAKE_HOST", "wasm32-ucos", F_SIMPLE, O_DEFAULT);
  var_define("SHELL", "/bin/sh", F_RECURSIVE, O_DEFAULT);
  var_define(".SHELLFLAGS", "-c", F_SIMPLE, O_DEFAULT);
  var_set(&globals, "CURDIR", make_cwd, F_SIMPLE, O_FILE);
  var_set(&globals, "MAKEFILE_LIST", "", F_SIMPLE, O_FILE);
  var_set(&globals, ".DEFAULT_GOAL", "", F_SIMPLE, O_FILE);
  var_set(&globals, ".RECIPEPREFIX", "", F_SIMPLE, O_FILE);
  var_set(&globals, ".FEATURES", "target-specific order-only second-expansion else-if shortest-stem undefine oneshell nocomment grouped-target notintermediate shell-export archives", F_SIMPLE, O_DEFAULT);
  var_set(&globals, ".INCLUDE_DIRS", "/usr/local/include /usr/include", F_SIMPLE, O_DEFAULT);
  {
    char lv[16];
    snprintf(lv, sizeof lv, "%d", makelevel);
    var_set(&globals, "MAKELEVEL", lv, F_SIMPLE, O_FILE);
    if (restarts) { snprintf(lv, sizeof lv, "%d", restarts); var_set(&globals, "MAKE_RESTARTS", lv, F_SIMPLE, O_FILE); }
  }
  {
    buf g = {0};
    for (int i = 0; i < goal_args.n; i++) { if (i) bputc(&g, ' '); bputs(&g, goal_args.v[i]); }
    var_set(&globals, "MAKECMDGOALS", g.s ? bstr(&g) : "", F_SIMPLE, O_FILE);
    bfree(&g);
  }
  if (!opt_r) set_suffixes_default();
  else var_define("SUFFIXES", "", F_RECURSIVE, O_DEFAULT);
  define_cmdline_vars();
  define_makeflags();
  for (int i = 0; i < old_files.n; i++) { file *f = file_enter(old_files.v[i]); f->oldfile = 1; }
  for (int i = 0; i < new_files.n; i++) { file *f = file_enter(new_files.v[i]); f->newfile = 1; }

  enter_dir_msg(0);

  for (int i = 0; i < evals.n; i++) read_text(evals.v[i], "--eval", 1);

  /* MAKEFILES from the environment: read first, never the default goal */
  {
    var *mfs = var_lookup_global("MAKEFILES", 9);
    if (mfs && mfs->origin == O_ENV && *mfs->value) {
      char *v = var_value(mfs);
      const char *p = v;
      size_t len;
      const char *w;
      while ((w = next_word(&p, &len))) {
        char *nm = xstrndup(w, len);
        var *dg = var_lookup_global(".DEFAULT_GOAL", 13);
        char *save = dg ? xstrdup(dg->value) : 0;
        read_makefile(nm, 1);
        if (save) { var_set(&globals, ".DEFAULT_GOAL", save, F_SIMPLE, O_FILE); free(save); }
        free(nm);
      }
      free(v);
    }
  }

  if (makefile_args.n) {
    for (int i = 0; i < makefile_args.n; i++) {
      const char *nm = makefile_args.v[i];
      int before = nmissing_incs;
      read_makefile(nm, 0);
      if (nmissing_incs > before) {
        /* -f FILE that does not exist: try to make it, else fail */
      }
    }
  } else {
    static const char *defs[] = {"GNUmakefile", "makefile", "Makefile", 0};
    int found = 0;
    for (int i = 0; defs[i]; i++) {
      sysstat st;
      if (sys_stat(defs[i], &st) == 0 && st.exists) { read_makefile(defs[i], 0); found = 1; break; }
    }
    (void)found;
  }

  /* MAKEFLAGS may have been changed by the makefile */
  {
    var *mf = var_lookup_global("MAKEFLAGS", 9);
    buf ours = {0};
    make_flags_string(&ours, 0);
    if (mf && strcmp(mf->value, bstr(&ours))) {
      char *v = var_value(mf);
      int old_r = opt_r, old_R = opt_R;
      int save_vars = cmdline_vars.n;
      parse_makeflags(v, 1);
      /* parse_makeflags re-adds command line variables it finds: drop the copies */
      while (cmdline_vars.n > save_vars) {
        free(cmdline_vars.v[0]);
        memmove(&cmdline_vars.v[0], &cmdline_vars.v[1], (cmdline_vars.n - 1) * sizeof(char *));
        cmdline_vars.n--;
        cmdline_vars.v[cmdline_vars.n] = 0;
      }
      free(v);
      if (opt_r && !old_r) {
        for (unsigned i = 0; i < files.cap; i++) {
          file *f = files.t[i].val;
          if (files.t[i].key && f && f->cmds && f->cmds->file && !strcmp(f->cmds->file, "<builtin>")) f->cmds = 0;
        }
      }
      if (opt_R && !old_R) {
        for (unsigned i = 0; i < globals.h.cap; i++) {
          var *gv = globals.h.t[i].val;
          if (globals.h.t[i].key && gv && gv->origin == O_DEFAULT && strcmp(gv->name, "MAKE") && strcmp(gv->name, "MAKE_COMMAND") &&
              strcmp(gv->name, "SHELL") && strcmp(gv->name, ".SHELLFLAGS") && strncmp(gv->name, "MAKE_", 5) && gv->name[0] != '.')
            globals.h.t[i].val = 0;
        }
      }
      define_makeflags();
      bfree(&ours);
      opt_print_dir = !opt_no_w && (opt_w || (!opt_s && (opt_C_given || makelevel > 0)));
    }
  }

  rules_finish();
  if (!opt_r) { /* GNU Make installs its pattern rules after the suffix rules */
    int save_prefix = recipe_prefix;
    recipe_prefix = '\t';
    reading_builtin = 1;
    read_text("(%): %\n\t$(AR) $(ARFLAGS) $@ $<\n", "<builtin>", 0);
    reading_builtin = 0;
    recipe_prefix = save_prefix;
  }
  {
    file *mf = file_lookup(".SUFFIXES");
    (void)mf;
  }

  /* .VARIABLES */
  {
    buf b = {0};
    for (unsigned i = 0; i < globals.h.cap; i++) {
      if (!globals.h.t[i].key || !globals.h.t[i].val) continue;
      if (b.n) bputc(&b, ' ');
      bputs(&b, globals.h.t[i].key);
    }
    var_set(&globals, ".VARIABLES", b.s ? bstr(&b) : "", F_SIMPLE, O_DEFAULT);
    bfree(&b);
  }

  if (!restarts || 1) remake_makefiles();

  int ng;
  file **goals = collect_goals(&ng);
  if (opt_p) print_db();
  if (!ng) {
    if (!makefiles_read.n) fatal("No targets specified and no makefile found");
    fatal("No targets");
  }
  int fail = update_goals(goals, ng, 0);
  if (!opt_q && !opt_t) delete_intermediates();
  int code = fail ? 2 : (opt_q && question_out) ? 1 : 0;
  make_exit(code);
}
