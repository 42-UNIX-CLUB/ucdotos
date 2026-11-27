// SPDX-License-Identifier: GPL-2.0-or-later
/* sh: start-up, the read-eval loop, variables, functions, aliases, history. */
#include "sh.h"
#include <signal.h>

char *shell_name = "sh";
static struct frame top_frame;
struct frame *frame = &top_frame;
char *hist[NHIST];
int nhist;

/* ---- variables ---- */
struct var { char *name, *val; int exp, ro; struct var *next; }; /* val NULL: unset, but exported or read-only */
static struct var *vars[64];
static char **env_cache;
static int env_dirty = 1;
static unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h & 63; }
static struct var *findvar(const char *name) {
  for (struct var *v = vars[hash(name)]; v; v = v->next) if (!strcmp(v->name, name)) return v;
  return NULL;
}
char *getvar(const char *name) { struct var *v = findvar(name); return v ? v->val : NULL; }
int is_readonly(const char *name) { struct var *v = findvar(name); return v && v->ro; }

/* Subshells run inside this process, so changes to variables, functions and
 * aliases are journaled and undone when the subshell ends. */
struct jent { char *name, *val; int had, exp, ro, depth; struct named **list; void *nval; struct jent *next; };
static struct jent *journal;
static int jdepth, jrestoring;
static void jrecord(const char *name) {
  if (!jdepth || jrestoring) return;
  struct var *v = findvar(name);
  struct jent *j = calloc(1, sizeof *j);
  j->name = strdup(name);
  j->had = v != NULL;
  j->val = v && v->val ? strdup(v->val) : NULL;
  j->exp = v ? v->exp : 0;
  j->ro = v ? v->ro : 0;
  j->depth = jdepth;
  j->next = journal;
  journal = j;
}
static struct var *newvar(const char *name) {
  struct var *v = calloc(1, sizeof *v);
  v->name = strdup(name);
  unsigned h = hash(name);
  v->next = vars[h];
  vars[h] = v;
  return v;
}
static void named_restore(struct jent *j);
void vars_enter(void) { jdepth++; }
void vars_leave(void) {
  jrestoring = 1;
  while (journal && journal->depth == jdepth) {
    struct jent *j = journal;
    journal = j->next;
    if (j->list) named_restore(j);
    else {
      unsetvar(j->name);
      if (j->had) {
        struct var *v = newvar(j->name);
        v->val = j->val;
        j->val = NULL;
        v->exp = j->exp;
        v->ro = j->ro;
        if (v->exp) env_dirty = 1;
      }
    }
    free(j->name); free(j->val); free(j);
  }
  jrestoring = 0;
  jdepth--;
}
extern int opt_a, assign_error;
void setvar(const char *name, const char *val, int exp) {
  struct var *v = findvar(name);
  if (v && v->ro && !jrestoring) { outf(2, "sh: %s: is read only\n", name); assign_error = 1; return; }
  jrecord(name);
  if (opt_a) exp = 1;
  if (!v) {
    v = newvar(name);
  }
  char *nv = strdup(val);
  free(v->val);
  v->val = nv;
  if (exp) v->exp = 1;
  if (v->exp) env_dirty = 1;
}
void unsetvar(const char *name) {
  struct var *rv = findvar(name);
  if (rv && rv->ro && !jrestoring) { outf(2, "sh: unset: %s: is read only\n", name); assign_error = 1; return; }
  jrecord(name);
  for (struct var **pp = &vars[hash(name)]; *pp; pp = &(*pp)->next)
    if (!strcmp((*pp)->name, name)) {
      struct var *v = *pp;
      *pp = v->next;
      if (v->exp) env_dirty = 1;
      free(v->name); free(v->val); free(v);
      return;
    }
}
void export_var(const char *name) {
  struct var *v = findvar(name);
  jrecord(name);
  if (!v) v = newvar(name); /* exported, still unset: not passed on until it gets a value */
  v->exp = 1;
  env_dirty = 1;
}
void set_readonly(const char *name) {
  struct var *v = findvar(name);
  jrecord(name);
  if (!v) v = newvar(name);
  v->ro = 1;
}
int valid_name(const char *s, size_t n) {
  if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (size_t i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
  return 1;
}
char **build_env(void) {
  if (!env_dirty && env_cache) return env_cache;
  if (env_cache) { for (char **e = env_cache; *e; e++) free(*e); free(env_cache); }
  int n = 0;
  for (int h = 0; h < 64; h++) for (struct var *v = vars[h]; v; v = v->next) n += v->exp && v->val;
  env_cache = malloc((n + 1) * sizeof *env_cache);
  n = 0;
  for (int h = 0; h < 64; h++)
    for (struct var *v = vars[h]; v; v = v->next) {
      if (!v->exp || !v->val) continue;
      char *kv = malloc(strlen(v->name) + strlen(v->val) + 2);
      sprintf(kv, "%s=%s", v->name, v->val);
      env_cache[n++] = kv;
    }
  env_cache[n] = NULL;
  env_dirty = 0;
  return env_cache;
}
static int cmpvar(const void *a, const void *b) { return strcmp((*(struct var *const *)a)->name, (*(struct var *const *)b)->name); }
static void put_value(const char *v) {
  outs(1, "'");
  for (; *v; v++) { if (*v == '\'') outs(1, "'\\''"); else sh_write(1, v, 1); }
  outs(1, "'");
}
/* what: 0 all set variables (set), 1 exported (export -p), 2 read-only (readonly -p) */
static void list_some(int what) {
  int n = 0;
  for (int h = 0; h < 64; h++) for (struct var *v = vars[h]; v; v = v->next) n++;
  struct var **all = malloc((n + 1) * sizeof *all);
  n = 0;
  for (int h = 0; h < 64; h++)
    for (struct var *v = vars[h]; v; v = v->next)
      if (what == 1 ? v->exp : what == 2 ? v->ro : v->val != NULL) all[n++] = v;
  qsort(all, n, sizeof *all, cmpvar);
  for (int i = 0; i < n; i++) {
    if (what) outs(1, what == 1 ? "export " : "readonly ");
    outs(1, all[i]->name);
    if (all[i]->val) { outs(1, "="); put_value(all[i]->val); }
    outs(1, "\n");
  }
  free(all);
}
void list_vars(int exported_only) { list_some(exported_only ? 1 : 0); }
void list_readonly(void) { list_some(2); }

/* ---- functions and aliases: small linked lists ---- */
struct named { char *name; void *val; struct named *next; };
static struct named *funcs, *aliases;
static struct named **find_named(struct named **list, const char *name) {
  for (; *list; list = &(*list)->next) if (!strcmp((*list)->name, name)) return list;
  return list;
}
/* a subshell journals the old definition (functions: the node; aliases: a copy of the text) */
static void njrecord(struct named **list, const char *name) {
  if (!jdepth || jrestoring) return;
  struct named **p = find_named(list, name);
  struct jent *j = calloc(1, sizeof *j);
  j->list = list;
  j->name = strdup(name);
  j->had = *p != NULL;
  if (*p) j->nval = list == &aliases ? (void *)strdup((*p)->val) : (*p)->val;
  j->depth = jdepth;
  j->next = journal;
  journal = j;
}
static void named_restore(struct jent *j) {
  struct named **p = find_named(j->list, j->name);
  if (!j->had) {
    if (*p) { struct named *n = *p; *p = n->next; if (j->list == &aliases) free(n->val); free(n->name); free(n); }
    return;
  }
  if (!*p) { *p = calloc(1, sizeof **p); (*p)->name = strdup(j->name); }
  else if (j->list == &aliases) free((*p)->val);
  (*p)->val = j->nval;
}
node *getfunc(const char *name) { struct named **p = find_named(&funcs, name); return *p ? (*p)->val : NULL; }
void setfunc(const char *name, node *body) {
  njrecord(&funcs, name);
  struct named **p = find_named(&funcs, name);
  if (!*p) { *p = calloc(1, sizeof **p); (*p)->name = strdup(name); }
  /* the body points into its parse arena, which is kept alive for it */
  node *f = malloc(sizeof *f);
  memset(f, 0, sizeof *f);
  f->type = N_FUNC;
  f->a = body;
  (*p)->val = f;
}
void unsetfunc(const char *name) {
  struct named **p = find_named(&funcs, name);
  if (*p) njrecord(&funcs, name);
  p = find_named(&funcs, name);
  if (*p) { struct named *n = *p; *p = n->next; free(n->name); free(n); }
}
char *getalias(const char *name) { struct named **p = find_named(&aliases, name); return *p ? (*p)->val : NULL; }
void setalias(const char *name, const char *val) {
  njrecord(&aliases, name);
  struct named **p = find_named(&aliases, name);
  if (!*p) { *p = calloc(1, sizeof **p); (*p)->name = strdup(name); }
  else free((*p)->val);
  (*p)->val = strdup(val);
}
int unalias(const char *name) {
  struct named **p = find_named(&aliases, name);
  if (!*p) return -1;
  njrecord(&aliases, name);
  struct named *n = *p;
  *p = n->next;
  free(n->name); free(n->val); free(n);
  return 0;
}
void unalias_all(void) { while (aliases) unalias(aliases->name); }
void list_aliases(void) {
  for (struct named *a = aliases; a; a = a->next) {
    outf(1, "%s=", a->name);
    put_value(a->val);
    outs(1, "\n");
  }
}

/* ---- history ---- */
static char *hist_path(void) {
  const char *h = getvar("HOME");
  if (!h) return NULL;
  char *p = malloc(strlen(h) + 16);
  sprintf(p, "%s/.sh_history", h);
  return p;
}
static void hist_push(const char *line) {
  if (nhist && !strcmp(hist[nhist - 1], line)) return;
  if (nhist == NHIST) { free(hist[0]); memmove(hist, hist + 1, (NHIST - 1) * sizeof *hist); nhist--; }
  hist[nhist++] = strdup(line);
}
void history_add(const char *line) {
  while (*line == ' ' || *line == '\n') line++;
  if (!*line) return;
  char *l = strdup(line);
  size_t n = strlen(l);
  while (n && l[n - 1] == '\n') l[--n] = 0;
  int same = nhist && !strcmp(hist[nhist - 1], l);
  hist_push(l);
  char *p = hist_path();
  if (p && !same && !strchr(l, '\n')) {
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { write(fd, l, n); write(fd, "\n", 1); close(fd); }
  }
  free(p);
  free(l);
}
static void history_load(void) {
  char *p = hist_path();
  size_t len;
  char *text = p ? read_file(p, &len) : NULL;
  free(p);
  if (!text) return;
  for (char *s = text, *e; *s; s = e + 1) {
    e = strchr(s, '\n');
    if (!e) { if (*s) hist_push(s); break; }
    *e = 0;
    if (*s) hist_push(s);
  }
  free(text);
}

char *read_file(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); errno = EISDIR; return NULL; }
  size_t cap = 4096, n = 0;
  char *buf = malloc(cap);
  long r;
  while ((r = read(fd, buf + n, cap - n - 1)) > 0) { n += r; if (n + 1 >= cap) buf = realloc(buf, cap *= 2); }
  close(fd);
  buf[n] = 0;
  if (len) *len = n;
  return buf;
}

void sh_exit(int code) {
  code = run_exit_trap(code);
  tty_restore();
  fflush(stdout);
  exit(code);
}

/* one line of input, with its newline; NULL at end of file */
static char *read_line(int tty, const char *prompt_var, const char *fallback) {
  if (tty) {
    const char *ps = getvar(prompt_var);
    char *p = expand_prompt(ps ? ps : fallback);
    char *line = tty_readline(p);
    free(p);
    if (!line) return NULL;
    char *r = malloc(strlen(line) + 2);
    sprintf(r, "%s\n", line);
    free(line);
    return r;
  }
  if (interactive) { const char *ps = getvar(prompt_var); char *p = expand_prompt(ps ? ps : fallback); outs(2, p); free(p); }
  size_t cap = 128, n = 0;
  char *buf = malloc(cap), c;
  long r;
  while ((r = read(0, &c, 1)) == 1) {
    if (n + 2 >= cap) buf = realloc(buf, cap *= 2);
    buf[n++] = c;
    if (c == '\n') break;
  }
  if (!n) { free(buf); return NULL; }
  buf[n] = 0;
  return buf;
}

#define DEFAULT_PS1 "\\[\\e[1;38;2;184;187;38m\\]\\u@\\h\\[\\e[0;38;2;235;219;178m\\]:\\[\\e[1;38;2;131;165;152m\\]\\w\\[\\e[0;38;2;235;219;178m\\]\\$ \\[\\e[0m\\]"

static void repl(int tty) {
  char *buf = NULL;
  for (;;) {
    reap_jobs(interactive);
    char *line = read_line(tty, buf ? "PS2" : "PS1", buf ? "> " : DEFAULT_PS1);
    if (line && opt_v) outs(2, line);
    if (!line) {
      if (buf) { outs(2, "sh: syntax error: unexpected end of file\n"); last_status = 2; }
      break;
    }
    if (buf) {
      char *nb = malloc(strlen(buf) + strlen(line) + 1);
      sprintf(nb, "%s%s", buf, line);
      free(buf); free(line);
      buf = nb;
    } else buf = line;
    __syscall0(SYS_intr); /* forget a Ctrl-C from before this line */
    struct arena ar = {0};
    struct parser p;
    parser_init(&p, buf, &ar);
    char *more = NULL;
    for (;;) {
      const char *cs = p.s;
      size_t cp = p.pos;
      node *n = parse_one(&p);
      if (p.status == P_END) break;
      if (p.status == P_INCOMPLETE) { more = strdup(cs + cp); break; }
      if (p.status == P_ERROR) { outf(2, "sh: %s\n", p.errmsg); last_status = 2; break; }
      run(n);
      if (exit_pending) { if (interactive) history_add(buf); sh_exit(exit_code); }
      brk = cont = retflag = 0;
      if (intr_pending) { /* Ctrl-C: drop the rest of this command line */
        if (!interactive) sh_exit(130);
        outs(2, "\n");
        intr_pending = 0;
        last_status = 130;
        break;
      }
    }
    if (!more && interactive) history_add(buf);
    if (!p.keep) afree(&ar);
    free(buf);
    buf = more;
  }
  if (interactive && tty) outs(1, "exit\n");
  sh_exit(exit_pending ? exit_code : last_status);
}

static void source_if_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return;
  size_t len;
  char *text = read_file(path, &len);
  if (!text) return;
  run_text(text, path);
  retflag = brk = cont = intr_pending = 0;
  free(text);
  if (exit_pending) sh_exit(exit_code);
}

int main(int argc, char **argv) {
  { /* builtins write in this process: a closed pipe must not kill the shell (child_sigs in builtin.c) */
    struct sigaction old;
    if (!sigaction(SIGPIPE, NULL, &old) && old.sa_handler == SIG_IGN) sigpipe_ign_at_start = 1;
    signal(SIGPIPE, SIG_IGN);
  }
  extern char **environ;
  for (char **e = environ; e && *e; e++) {
    char *eq = strchr(*e, '=');
    if (!eq || !valid_name(*e, eq - *e)) continue;
    char *name = strndup(*e, eq - *e);
    setvar(name, eq + 1, 1);
    free(name);
  }
  if (!getvar("PATH")) setvar("PATH", "/bin", 1);
  if (!getvar("HOME")) setvar("HOME", "/home/user", 1);
  if (!getvar("USER")) setvar("USER", getuid() == 0 ? "root" : "user", 1);
  if (!getvar("SHELL")) setvar("SHELL", "/bin/sh", 1);
  char cwd[512];
  if (getcwd(cwd, sizeof cwd)) setvar("PWD", cwd, 1);
  setvar("PS2", "> ", 0);
  shell_name = argv[0] && argv[0][0] ? argv[0] : "sh";
  /* pid 1 is started as "init": present ourselves as sh */
  if (!strcmp(shell_name, "init")) shell_name = "sh";

  shell_start();
  int force_i = 0, read_stdin = 0;
  const char *cmd = NULL;
  int i = parse_args(argc, argv, &cmd, &force_i, &read_stdin);
  if (i < 0) return 2;
  if (cmd) {
    if (i < argc) shell_name = argv[i++];
    top_frame.argc = argc - i;
    top_frame.argv = argv + i;
    run_text(cmd, NULL);
    sh_exit(exit_pending ? exit_code : intr_pending ? 130 : last_status);
  }
  if (i < argc && !read_stdin) {
    size_t len;
    char *text = read_file(argv[i], &len);
    if (!text) { outf(2, "sh: %s: %s\n", argv[i], strerror(errno)); return 127; }
    shell_name = argv[i];
    top_frame.argc = argc - i - 1;
    top_frame.argv = argv + i + 1;
    run_text(text, argv[i]);
    sh_exit(exit_pending ? exit_code : intr_pending ? 130 : last_status);
  }
  int tty = tty_init();
  interactive = tty || force_i;
  top_frame.argc = argc - i;
  top_frame.argv = argv + i;
  if (interactive) {
    if (tty) {
      __syscall0(SYS_setsid);
      pid_t me = getpid();
      __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
      if (!getvar("TERM")) setvar("TERM", "xterm-256color", 1);
    }
    jobs_init(tty);
    if (!getvar("PS1")) setvar("PS1", DEFAULT_PS1, 0);
    source_if_exists("/etc/profile");
    const char *home = getvar("HOME");
    if (home) {
      char *rc = malloc(strlen(home) + 8);
      sprintf(rc, "%s/.shrc", home);
      source_if_exists(rc);
      free(rc);
    }
    history_load();
  }
  repl(tty);
  return 0;
}
