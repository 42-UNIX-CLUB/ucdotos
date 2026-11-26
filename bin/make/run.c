// SPDX-License-Identifier: GPL-2.0-or-later
/* make: deciding what is out of date and running recipes, one at a time or
 * up to -j N at once. */
#include "make.h"

enum { R_OK, R_FAIL, R_RUN };
int commands_started;
int job_slots = 1;
int question_out;      /* -q: something needs remaking */
static int stop_now;   /* an error without -k: start nothing new */
static int any_error;
static int makefiles_phase;
static char **deferred_msgs;
static int ndeferred;
void flush_deferred_msgs(void) {
  for (int i = 0; i < ndeferred; i++) { fputs(deferred_msgs[i], stderr); free(deferred_msgs[i]); }
  ndeferred = 0;
  fflush(stderr);
}

/* recipe line flags */
enum { L_SILENT = 1, L_IGNORE = 2, L_RECURSE = 4 };

typedef struct job {
  file *f;
  char **lines;
  unsigned char *flags;
  int *lineno;
  int n, cur;
  char **env;
  int pid;
  int failed;
  struct job *next;
} job;
static job *running;
static int njobs;
static file **made_order;
static int nmade, capmade;

static void free_env(char **env) {
  if (!env) return;
  for (char **e = env; *e; e++) free(*e);
  free(env);
}

/* ---- environment for children ---- */
static int valid_env_name(const char *s) {
  if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
  for (s++; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
  return 1;
}
static void env_put(strlist *l, htab *seen, const char *name, const char *value) {
  int isnew;
  void **slot = hput(seen, name, strlen(name), &isnew);
  char *e = xconcat(name, "=", value);
  if (!isnew && *slot) {
    int idx = (int)(intptr_t)*slot - 1;
    free(l->v[idx]);
    l->v[idx] = e;
    return;
  }
  sl_add(l, e);
  *slot = (void *)(intptr_t)l->n;
}
static void consider(strlist *l, htab *seen, htab *done, const char *name) {
  int isnew;
  hput(done, name, strlen(name), &isnew);
  if (!isnew) return;
  var *v = var_lookup(name, strlen(name));
  if (!v) return;
  if (!strcmp(name, "MAKEFLAGS") || !strcmp(name, "MAKELEVEL") || !strcmp(name, "SHELL")) return;
  int ex;
  int e = v->exp;
  if (e == EXP_DEFAULT) { /* a target-specific value keeps the global export setting */
    var *g = var_lookup_global(name, strlen(name));
    if (g && g != v) {
      e = g->exp;
      if (e == EXP_DEFAULT && (g->origin == O_ENV || g->origin == O_ENV_OVERRIDE || g->origin == O_COMMAND)) e = EXP_YES;
    }
  }
  if (e == EXP_YES) ex = 1;
  else if (e == EXP_NO) ex = 0;
  else if (v->exp == EXP_YES) ex = 1;
  else if (v->exp == EXP_NO) ex = 0;
  else if (v->origin == O_DEFAULT || v->origin == O_AUTOMATIC) ex = 0;
  else if (v->origin == O_ENV || v->origin == O_ENV_OVERRIDE || v->origin == O_COMMAND) ex = 1;
  else ex = export_all && valid_env_name(name);
  if (!ex) return;
  if (v->origin != O_ENV && !valid_env_name(name)) return;
  if (v->expanding) {
    const char *ev = getenv(name);
    if (ev) env_put(l, seen, name, ev);
    return;
  }
  char *val = expand_var_named(name);
  env_put(l, seen, name, val);
  free(val);
}
char **build_env(file *f) {
  strlist l = {0};
  htab seen = {0}, done = {0};
  scope *save = cur_scope;
  file *save_auto = auto_file;
  if (f) cur_scope = f->sc;
  auto_file = f;
  if (f) for (scope *s = f->sc; s; s = s->next)
    for (unsigned i = 0; i < s->set->h.cap; i++) if (s->set->h.t[i].key && s->set->h.t[i].val) consider(&l, &seen, &done, s->set->h.t[i].key);
  for (unsigned i = 0; i < globals.h.cap; i++) if (globals.h.t[i].key && globals.h.t[i].val) consider(&l, &seen, &done, globals.h.t[i].key);
  /* SHELL: the value from our own environment, unless exported on purpose */
  var *sv = var_lookup("SHELL", 5);
  if (sv && sv->exp == EXP_YES) { char *v = var_value(sv); env_put(&l, &seen, "SHELL", v); free(v); }
  else { const char *e = getenv("SHELL"); if (e) env_put(&l, &seen, "SHELL", e); }
  var *mf = var_lookup("MAKEFLAGS", 9);
  if (mf && mf->exp != EXP_NO) { char *v = var_value(mf); env_put(&l, &seen, "MAKEFLAGS", v); free(v); }
  char lv[16];
  snprintf(lv, sizeof lv, "%d", makelevel + 1);
  env_put(&l, &seen, "MAKELEVEL", lv);
  cur_scope = save;
  auto_file = save_auto;
  for (unsigned i = 0; i < seen.cap; i++) free(seen.t[i].key);
  free(seen.t);
  for (unsigned i = 0; i < done.cap; i++) free(done.t[i].key);
  free(done.t);
  if (!l.v) sl_add(&l, 0), l.n = 0;
  return l.v;
}

/* ---- messages ---- */
static void recipe_error(file *f, int lineno, const char *what, int ignored) {
  const char *cf = f->cmds && f->cmds->file ? f->cmds->file : "";
  char where[512];
  if (!strcmp(cf, "<builtin>")) lineno = 0;
  if (lineno > 0) snprintf(where, sizeof where, "%s:%d: ", cf, lineno);
  else snprintf(where, sizeof where, "%s: ", cf);
  fflush(stdout);
  char line[1024];
  if (ignored) snprintf(line, sizeof line, "%s: [%s%s] %s (ignored)\n", prefix_str(), where, f->name, what);
  else snprintf(line, sizeof line, "%s: *** [%s%s] %s\n", prefix_str(), where, f->name, what);
  if (makefiles_phase && !ignored) { /* shown after the "No such file" line, as GNU Make does */
    deferred_msgs = xrealloc(deferred_msgs, (ndeferred + 1) * sizeof *deferred_msgs);
    deferred_msgs[ndeferred++] = xstrdup(line);
    return;
  }
  fputs(line, stderr);
  fflush(stderr);
}

static int is_precious(file *f) {
  if (f->precious) return 1;
  file *p = file_lookup(".PRECIOUS");
  if (!p) return 0;
  size_t n = strlen(f->name);
  for (dep *d = p->deps; d; d = d->next) {
    if (!d->f) continue;
    const char *pct;
    char *u = unescape_percent(d->f->name, &pct);
    int m = pct && pattern_match(u, pct, f->name, n, 0, 0);
    free(u);
    if (m) return 1;
  }
  return 0;
}

static void delete_target(file *f, int on_error) {
  if (f->phony || is_precious(f)) return;
  (void)on_error;
  const char *nm = f->name;
  sysstat st;
  if (sys_stat(nm, &st) != 0 || !st.exists || st.is_dir) return;
  if (st.mtime == f->mtime_before) return; /* not touched by the recipe */
  fflush(stdout);
  fprintf(stderr, "%s: *** Deleting file '%s'\n", prefix_str(), nm);
  fflush(stderr);
  sys_unlink(nm);
  stat_forget(nm);
}

static void reap(int block);
/* ---- running one command line ---- */
static const char *sh_builtins[] = {".", ":", "alias", "bg", "break", "case", "cd", "command", "continue", "eval", "exec", "exit",
  "export", "fc", "fg", "for", "getopts", "hash", "if", "jobs", "login", "logout", "read", "readonly", "return", "set",
  "shift", "test", "times", "trap", "type", "ulimit", "umask", "unalias", "unset", "until", "wait", "while", "[", "{", "!",
  "echo", "printf", "pwd", "true", "false", "local", "source", 0};

static char *shell_path(void) {
  var *v = var_lookup("SHELL", 5);
  char *s = v ? var_value(v) : xstrdup("/bin/sh");
  if (!*s) { free(s); s = xstrdup("/bin/sh"); }
  return s;
}

/* Split a simple command into words without a shell, when nothing in it needs one. */
static char **simple_argv(const char *line) {
  for (const char *p = line; *p; p++)
    if (strchr("#;\"'*?[]&|<>(){}$`^~!\\\n=%", *p)) return 0;
  strlist l = {0};
  const char *p = line;
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) sl_add(&l, xstrndup(w, len));
  if (!l.n) { free(l.v); return 0; }
  for (int i = 0; sh_builtins[i]; i++) if (!strcmp(l.v[0], sh_builtins[i])) { sl_free(&l); return 0; }
  return l.v;
}

/* start one line; returns pid, or -1 with *status set when it finished at once */
static int spawn_line(const char *line, char **env, int *status) {
  char *sh = shell_path();
  int fast = 0;
  char **argv = 0;
  var *fv = var_lookup(".SHELLFLAGS", 11);
  char *flags = fv ? var_value(fv) : xstrdup("-c");
  if (!strcmp(sh, "/bin/sh") && (!strcmp(flags, "-c") || !strcmp(flags, "-ec")) && !oneshell) {
    argv = simple_argv(line);
    fast = argv != 0;
  }
  strlist al = {0};
  if (!fast) {
    sl_add(&al, xstrdup(sh));
    const char *p = flags;
    size_t len;
    const char *w;
    while ((w = next_word(&p, &len))) sl_add(&al, xstrndup(w, len));
    sl_add(&al, xstrdup(line));
    argv = al.v;
  }
  fflush(stdout);
  fflush(stderr);
  int pid;
  if (fast) {
    var *pv = var_lookup("PATH", 4);
    char *path = pv ? var_value(pv) : 0;
    pid = sys_spawn_path(argv, env, path);
    while (pid == -EAGAIN && njobs > 0) { reap(1); pid = sys_spawn_path(argv, env, path); }
    free(path);
    if (pid < 0) {
      fprintf(stderr, "%s: %s: %s\n", prefix_str(), argv[0], strerror(-pid));
      fflush(stderr);
      *status = 127 << 8;
    }
    for (char **a = argv; *a; a++) free(*a);
    free(argv);
  } else {
    pid = sys_spawn(argv, env, -1);
    while (pid == -EAGAIN && njobs > 0) { reap(1); pid = sys_spawn(argv, env, -1); }
    if (pid < 0) {
      fprintf(stderr, "%s: %s: %s\n", prefix_str(), sh, strerror(-pid));
      fflush(stderr);
      *status = 127 << 8;
    }
    sl_free(&al);
  }
  free(sh);
  free(flags);
  return pid < 0 ? -1 : pid;
}

/* ---- jobs ---- */
static void job_free(job *j) {
  for (int i = 0; i < j->n; i++) free(j->lines[i]);
  free(j->lines); free(j->flags); free(j->lineno);
  free_env(j->env);
  free(j);
}

static void finish_file(file *f, int failed);

static int line_has_make(const char *s) { return strstr(s, "$(MAKE)") || strstr(s, "${MAKE}"); }

/* Expand the whole recipe first, as GNU Make does, then cut it into command lines. */
static job *make_job(file *f) {
  job *j = xcalloc(1, sizeof *j);
  j->f = f;
  cmds *c = f->cmds;
  scope *save = cur_scope;
  file *save_auto = auto_file;
  const char *save_file = read_file_name;
  int save_line = read_line;
  cur_scope = f->sc;
  auto_file = f;
  read_file_name = c->file;
  int cap = 0;
  if (oneshell && c->n > 0) {
    buf all = {0};
    unsigned char fl = 0;
    for (int i = 0; i < c->n; i++) {
      read_line = c->lineno[i];
      char *e = expand(c->lines[i]);
      const char *p = e;
      while (*p == '@' || *p == '-' || *p == '+' || is_blank(*p)) {
        if (i == 0) { if (*p == '@') fl |= L_SILENT; if (*p == '-') fl |= L_IGNORE; if (*p == '+') fl |= L_RECURSE; }
        p++;
      }
      if (line_has_make(c->lines[i])) fl |= L_RECURSE;
      if (i) bputc(&all, '\n');
      bputs(&all, p);
      free(e);
    }
    j->lines = xmalloc(sizeof *j->lines);
    j->flags = xmalloc(1);
    j->lineno = xmalloc(sizeof(int));
    j->lines[0] = bdetach(&all);
    if (!j->lines[0]) j->lines[0] = xstrdup("");
    j->flags[0] = fl;
    j->lineno[0] = c->lineno[0];
    j->n = 1;
  } else {
    for (int i = 0; i < c->n; i++) {
      read_line = c->lineno[i];
      unsigned char base = 0;
      const char *raw = c->lines[i];
      const char *rp = raw;
      while (*rp == '@' || *rp == '-' || *rp == '+' || is_blank(*rp)) {
        if (*rp == '@') base |= L_SILENT;
        if (*rp == '-') base |= L_IGNORE;
        if (*rp == '+') base |= L_RECURSE;
        rp++;
      }
      if (line_has_make(raw)) base |= L_RECURSE;
      char *e = expand(rp);
      /* cut at newlines that are not escaped */
      char *p = e;
      for (;;) {
        char *q = p;
        while (*q && !(*q == '\n' && !(q > p && q[-1] == '\\'))) q++;
        int last = *q == 0;
        *q = 0;
        unsigned char fl = base;
        char *s = p;
        while (*s == '@' || *s == '-' || *s == '+' || is_blank(*s)) {
          if (*s == '@') fl |= L_SILENT;
          if (*s == '-') fl |= L_IGNORE;
          if (*s == '+') fl |= L_RECURSE;
          s++;
        }
        if (j->n >= cap) {
          cap = cap ? cap * 2 : 4;
          j->lines = xrealloc(j->lines, cap * sizeof *j->lines);
          j->flags = xrealloc(j->flags, cap);
          j->lineno = xrealloc(j->lineno, cap * sizeof(int));
        }
        j->lines[j->n] = xstrdup(s);
        j->flags[j->n] = fl;
        j->lineno[j->n] = c->lineno[i];
        j->n++;
        if (last) break;
        p = q + 1;
      }
      free(e);
    }
  }
  j->env = build_env(f);
  cur_scope = save;
  auto_file = save_auto;
  read_file_name = save_file;
  read_line = save_line;
  return j;
}

/* Start the next line of J. Returns 1 if a child is running, 0 if the job is over. */
static int job_step(job *j) {
  file *f = j->f;
  while (j->cur < j->n) {
    int i = j->cur++;
    const char *line = j->lines[i];
    unsigned char fl = j->flags[i];
    int silent = (fl & L_SILENT) || opt_s || all_silent || f->silent;
    int recurse = fl & L_RECURSE;
    if (!*skip_ws(line)) continue;
    commands_started++;
    if ((opt_n || opt_t || opt_q) && !recurse) {
      if (opt_n) {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
      }
      continue;
    }
    if (!silent || opt_trace || (opt_n && recurse)) {
      fputs(line, stdout);
      fputc('\n', stdout);
      fflush(stdout);
    }
    if (!*skip_ws(line)) continue;
    int status = 0;
    int pid = spawn_line(line, j->env, &status);
    if (pid < 0) {
      /* could not start: treat as an exit status */
      int ignore = (fl & L_IGNORE) || opt_i || all_ignore || f->ignore;
      char what[64];
      snprintf(what, sizeof what, "Error %d", (status >> 8) & 0xff);
      recipe_error(f, j->lineno[i], what, ignore);
      if (!ignore) { j->failed = 1; return 0; }
      continue;
    }
    j->pid = pid;
    return 1;
  }
  return 0;
}


/* A child of J ended with STATUS. Returns 1 if J goes on with another line. */
static int job_child_done(job *j, int status) {
  int ex, code, sig;
  sys_wait_status(status, &ex, &code, &sig);
  j->pid = 0;
  int i = j->cur - 1;
  file *f = j->f;
  if (!ex || code) {
    int ignore = (j->flags[i] & L_IGNORE) || opt_i || all_ignore || f->ignore;
    char what[96];
    if (ex) snprintf(what, sizeof what, "Error %d", code);
    else {
      const char *sn = sig == 2 ? "Interrupt" : sig == 9 ? "Killed" : sig == 15 ? "Terminated" : sig == 11 ? "Segmentation fault" :
                       sig == 6 ? "Aborted" : sig == 13 ? "Broken pipe" : sig == 1 ? "Hangup" : sig == 3 ? "Quit" : 0;
      if (sn) snprintf(what, sizeof what, "%s", sn);
      else snprintf(what, sizeof what, "Signal %d", sig);
    }
    recipe_error(f, j->lineno[i], what, ignore);
    if (!ignore) { j->failed = 1; return 0; }
  }
  return job_step(j);
}

static void job_done(job *j) {
  file *f = j->f;
  if (j->failed) {
    if (delete_on_error || sys_interrupted()) delete_target(f, 1);
  }
  finish_file(f, j->failed);
  job_free(j);
}

static void reap(int block) {
  for (;;) {
    int status = 0;
    int pid = sys_wait_any(&status, block);
    if (pid <= 0) return;
    job **pp = &running;
    while (*pp && (*pp)->pid != pid) pp = &(*pp)->next;
    if (!*pp) continue; /* not ours (a $(shell) child, say) */
    job *j = *pp;
    int more = job_child_done(j, status);
    if (!more) {
      *pp = j->next;
      njobs--;
      if (j->failed && !opt_k) {
        if (!stop_now && njobs > 0) {
          fflush(stdout);
          fprintf(stderr, "%s: *** Waiting for unfinished jobs....\n", prefix_str());
          fflush(stderr);
        }
        stop_now = 1;
      }
      if (j->failed) any_error = 1;
      job_done(j);
    }
    return;
  }
}

/* run J to the end when only one job at a time is allowed */
static job *sync_job;
static void run_job_sync(job *j) {
  sync_job = j;
  int more = job_step(j);
  while (more) {
    int st = 0;
    sys_waitpid(j->pid, &st);
    more = job_child_done(j, st);
  }
  sync_job = 0;
  if (j->failed) any_error = 1;
  job_done(j);
}

static void start_job(file *f) {
  f->state = ST_RUNNING;
  for (int i = 0; i < f->nalso; i++) if (f->also[i]->state != ST_DONE) f->also[i]->state = ST_RUNNING;
  f->mtime_before = file_mtime(f) == MT_MISSING ? MT_MISSING : f->mtime;
  {
    sysstat st;
    f->mtime_before = (sys_stat(f->name, &st) == 0 && st.exists) ? st.mtime : MT_MISSING;
  }
  if (opt_trace) {
    buf why = {0};
    if (f->phony) bputs(&why, "target is .PHONY");
    else if (f->mtime_before == MT_MISSING) bputs(&why, "target does not exist");
    else {
      file *sa = auto_file;
      auto_file = f;
      char *q = expand_var_named("?");
      auto_file = sa;
      if (*q) bputs(&why, q);
      else bputs(&why, opt_B ? "always-make" : "target is out of date");
      free(q);
    }
    const char *cf = f->cmds->file ? f->cmds->file : "";
    if (!strcmp(cf, "<builtin>")) printf("%s: update target '%s' due to: %s\n", cf, f->name, bstr(&why));
    else printf("%s:%d: update target '%s' due to: %s\n", cf, f->cmds->line, f->name, bstr(&why));
    fflush(stdout);
    bfree(&why);
  }
  job *j = make_job(f);
  if (job_slots == 1 || notparallel) {
    run_job_sync(j);
    return;
  }
  while (job_slots > 0 && njobs >= job_slots) reap(1);
  if (job_step(j)) {
    j->next = running;
    running = j;
    njobs++;
  } else {
    if (j->failed) {
      any_error = 1;
      if (!opt_k) stop_now = 1;
    }
    job_done(j);
  }
}

/* ---- the update walk ---- */
static void mark_done(file *f, int failed) {
  f->state = ST_DONE;
  f->status = failed;
}

static void finish_file(file *f, int failed) {
  if (nmade >= capmade) { capmade = capmade ? capmade * 2 : 64; made_order = xrealloc(made_order, capmade * sizeof *made_order); }
  made_order[nmade++] = f;
  f->ran = 1;
  f->changed = 1;
  file *head = f->dchead ? f->dchead : f;
  if (opt_n || opt_q) {
    head->mtime = f->mtime = MT_NEW;
  } else {
    if (f->vpath) { free(f->vpath); f->vpath = 0; }
    stat_forget(f->name);
    head->mtime = f->mtime = MT_UNKNOWN;
  }
  mark_done(f, failed);
  for (int i = 0; i < f->nalso; i++) {
    file *o = f->also[i];
    o->ran = 1; o->changed = 1;
    stat_forget(o->name);
    if (o->state != ST_DONE) {
      o->mtime = (opt_n || opt_q) ? MT_NEW : MT_UNKNOWN;
      mark_done(o, failed);
    }
  }
}

static int update_file(file *f, file *parent, int depth);

/* is a missing intermediate file needed by a target whose time is TMT? */
static int intermediate_needed(file *f, mtime_t tmt, int depth) {
  if (depth > 64) return 1;
  if (f->phony) return 1;
  if (!f->being_made) {
    f->being_made = 1;
    if (!f->sc) build_scope(f);
  }
  for (dep *d = f->deps; d; d = d->next) {
    if (!d->f || d->oo) continue;
    file *g = d->f;
    if (g->intermediate && !g->phony && file_mtime(g) == MT_MISSING) {
      if (!g->cmds && !g->tried_impl) { g->tried_impl = 1; implicit_search(g, 0); }
      if (intermediate_needed(g, tmt, depth + 1)) return 1;
      continue;
    }
    int r = update_file(g, f, depth + 1);
    if (r == R_RUN) return 1;
    mtime_t m = file_mtime(g);
    if (m > tmt || m == MT_MISSING || g->phony) return 1;
  }
  return 0;
}

static void second_expand(file *f) {
  dep *nd = 0, **tail = &nd;
  dep *d = f->deps;
  /* the dependency list so far, for $^ during the expansion */
  file *save_auto = auto_file;
  int save2 = auto_2nd;
  scope *save_sc = cur_scope;
  auto_file = f;
  auto_2nd = 1;
  cur_scope = f->sc;
  f->deps = 0;
  f->deptail = &f->deps;
  while (d) {
    dep *next = d->next;
    if (!d->need2) {
      d->next = 0;
      *tail = d; tail = &d->next;
      dep_append(f, dep_new(d->f)); /* visible to $^ while expanding */
      d = next;
      continue;
    }
    char *e = expand(d->name);
    strlist names = {0};
    const char *p = e;
    size_t len;
    const char *w;
    while ((w = next_word(&p, &len))) sl_add(&names, xstrndup(w, len));
    for (int i = 0; i < names.n; i++) {
      char *nm = names.v[i];
      if (d->wait == 2 && f->stem) { /* static pattern: % now */
        const char *pct;
        char *u = unescape_percent(nm, &pct);
        if (pct) {
          buf b = {0};
          pattern_subst(&b, u, pct, f->stem, strlen(f->stem));
          free(nm);
          nm = bdetach(&b);
        }
        free(u);
      }
      if (!strcmp(nm, ".WAIT")) continue;
      file *df = file_enter(nm);
      df->mentioned = 1;
      dep *x = dep_new(df);
      x->oo = d->oo;
      *tail = x; tail = &x->next;
      dep_append(f, dep_new(df));
    }
    sl_free(&names);
    free(e);
    d = next;
  }
  f->deps = nd;
  f->deptail = tail;
  auto_file = save_auto;
  auto_2nd = save2;
  cur_scope = save_sc;
}

static int needs_remake(file *f, int has_normal_deps) {
  if (f->phony) return 1;
  if (opt_B && !(makefiles_phase && file_mtime(f) != MT_MISSING)) return 1;
  mtime_t mt = file_mtime(f);
  if (mt == MT_MISSING) return 1;
  if (f->dc && !has_normal_deps && f->cmds) return 1;
  for (dep *d = f->deps; d; d = d->next) {
    if (!d->f || d->oo) continue;
    file *g = d->f;
    if (g->skipped || d->deferred) continue;
    if (g->phony) return 1;
    mtime_t dm = file_mtime(g);
    if (dm > mt) return 1;
    if (dm == MT_MISSING) return 1;
    if (g->changed && (opt_n || opt_q || opt_t) && g->ran) return 1;
  }
  return 0;
}

static void no_rule(file *f, file *parent) {
  if (f->dontcare) return;
  fflush(stdout);
  if (parent)
    fprintf(stderr, "%s: *** No rule to make target '%s', needed by '%s'.%s\n", prefix_str(), f->name, parent->name, opt_k ? "" : "  Stop.");
  else
    fprintf(stderr, "%s: *** No rule to make target '%s'.%s\n", prefix_str(), f->name, opt_k ? "" : "  Stop.");
  fflush(stderr);
  if (!opt_k) stop_now = 2;
}

static int update_one(file *f, file *parent, int depth) {
  if (f->state == ST_DONE) return f->status ? R_FAIL : R_OK;
  if (f->state == ST_RUNNING) return R_RUN;
  if (f->state == ST_VISIT) {
    fflush(stdout);
    fprintf(stderr, "%s: Circular %s <- %s dependency dropped.\n", prefix_str(), parent ? parent->name : "", f->name);
    fflush(stderr);
    return -1;
  }
  if (!f->being_made) {
    f->being_made = 1;
    f->parent = parent;
    build_scope(f);
    if (!f->phony && !f->cmds && !f->tried_impl) {
      f->tried_impl = 1;
      implicit_search(f, depth);
    }
    if (!f->cmds && !f->is_target && !f->phony) {
      file *df = file_lookup(".DEFAULT");
      if (df && df->cmds) f->cmds = df->cmds;
    }
    if (f->needs2 || (f->deps && f->irule && f->irule->need2)) second_expand(f);
    else {
      int any = 0;
      for (dep *d = f->deps; d; d = d->next) if (d->need2) any = 1;
      if (any) second_expand(f);
    }
    for (dep *d = f->deps; d; d = d->next) if (d->f) d->f->mentioned = 1;
    file_mtime(f);
  }
  if (f->oldfile && file_mtime(f) != MT_MISSING) { mark_done(f, 0); return R_OK; }
  f->state = ST_VISIT;
  int run = 0, failed = 0, has_normal = 0, all_done_prefix = 1;
  int serial = job_slots == 1 || notparallel;
  mtime_t mt = file_mtime(f);
  int k = 0;
  if (f->phase == 0) {
    /* first everything except missing intermediate files */
    for (dep *d = f->deps; d; d = d->next, k++) {
      if (!d->oo) has_normal = 1;
      if (k < f->dcursor) continue;
      if (!d->f) continue;
      if (d->wait && run) break;
      file *g = d->f;
      if (stop_now && !opt_k) break;
      if (g->intermediate && !g->phony && g->state != ST_DONE && file_mtime(g) == MT_MISSING) {
        if (!g->being_made) {
          g->being_made = 1;
          g->parent = f;
          build_scope(g);
          if (!g->cmds && !g->tried_impl) { g->tried_impl = 1; implicit_search(g, depth + 1); }
        }
        d->deferred = 1;
        if (all_done_prefix) f->dcursor = k + 1;
        continue;
      }
      int r = update_file(g, f, depth + 1);
      if (r == -1) {
        /* circular: drop this prerequisite */
        d->f = 0;
        if (all_done_prefix) f->dcursor = k + 1;
        continue;
      }
      if (r == R_RUN) {
        run = 1;
        all_done_prefix = 0;
        if (serial) break;
        continue;
      }
      if (r == R_FAIL) {
        failed = 1;
        all_done_prefix = 0; /* keep looking at it, so the failure is seen again */
        if (!opt_k) break;
      }
      if (all_done_prefix) f->dcursor = k + 1;
    }
    if (run) { f->state = ST_NEW; return R_RUN; }
    if (failed) goto failed_deps;
    if (stop_now && !opt_k) { mark_done(f, 1); return R_FAIL; }
    int must = needs_remake(f, has_normal);
    if (!must) {
      for (dep *d = f->deps; d && !must; d = d->next)
        if (d->deferred && d->f && !d->oo && intermediate_needed(d->f, mt, depth + 1)) must = 1;
    }
    f->must = must;
    f->phase = 1;
  }
  for (dep *d = f->deps; d; d = d->next) if (!d->oo) has_normal = 1;
  if (f->phase == 1) {
    k = 0;
    for (dep *d = f->deps; d; d = d->next, k++) {
      if (k < f->dcursor2) continue;
      if (!d->deferred || !d->f) { if (all_done_prefix) f->dcursor2 = k + 1; continue; }
      file *g = d->f;
      if (!f->must) {
        if (g->state != ST_DONE) { g->skipped = 1; mark_done(g, 0); }
        if (all_done_prefix) f->dcursor2 = k + 1;
        continue;
      }
      if (stop_now && !opt_k) break;
      int r = update_file(g, f, depth + 1);
      if (r == -1) { d->f = 0; if (all_done_prefix) f->dcursor2 = k + 1; continue; }
      if (r == R_RUN) { run = 1; all_done_prefix = 0; if (serial) break; continue; }
      if (r == R_FAIL) { failed = 1; all_done_prefix = 0; if (!opt_k) break; }
      if (all_done_prefix) f->dcursor2 = k + 1;
    }
    if (run) { f->state = ST_NEW; return R_RUN; }
    if (failed) goto failed_deps;
    f->phase = 2;
  }
  if (stop_now && !opt_k) { mark_done(f, 1); return R_FAIL; }
  if (!f->cmds) {
    if (file_mtime(f) == MT_MISSING && !f->is_target && !f->phony && !f->deps) {
      if (f->dontcare) { mark_done(f, 1); return R_FAIL; }
      no_rule(f, parent);
      any_error = 1;
      mark_done(f, 1);
      return R_FAIL;
    }
    if (f->must) {
      f->changed = 1;
      if (file_mtime(f) == MT_MISSING) f->mtime = MT_MISSING;
    }
    mark_done(f, 0);
    return R_OK;
  }
  if (!f->must) {
    mark_done(f, 0);
    return R_OK;
  }
  if (opt_q && !makefiles_phase) {
    /* lines with + still run under -q */
    int any_recurse = 0;
    for (int i = 0; i < f->cmds->n; i++) {
      const char *p = f->cmds->lines[i];
      while (*p == '@' || *p == '-' || is_blank(*p)) p++;
      if (*p == '+' || line_has_make(f->cmds->lines[i])) any_recurse = 1;
    }
    question_out = 1;
    if (!any_recurse) {
      f->changed = 1;
      f->ran = 1;
      f->mtime = MT_NEW;
      mark_done(f, 0);
      return R_OK;
    }
  }
  if (opt_t && !makefiles_phase) {
    commands_started++;
    if (!f->phony) {
      if (!opt_s && !all_silent && !f->silent) { printf("touch %s\n", f->name); fflush(stdout); }
      if (!opt_n) { sys_touch(f->name); stat_forget(f->name); }
    }
    f->ran = 1;
    f->changed = 1;
    f->mtime = MT_UNKNOWN;
    mark_done(f, 0);
    return R_OK;
  }
  int save_n = opt_n, save_q = opt_q, save_t = opt_t;
  if (makefiles_phase) { opt_n = opt_q = opt_t = 0; }
  start_job(f);
  if (makefiles_phase) { opt_n = save_n; opt_q = save_q; opt_t = save_t; }
  if (f->state == ST_DONE) return f->status ? R_FAIL : R_OK;
  return R_RUN;
failed_deps:
  mark_done(f, 1);
  if (depth == 0 && opt_k && !opt_n && !opt_q && !makefiles_phase) {
    fflush(stdout);
    fprintf(stderr, "%s: Target '%s' not remade because of errors.\n", prefix_str(), f->name);
    fflush(stderr);
  }
  return R_FAIL;
}

static int update_file(file *f, file *parent, int depth) {
  if (!(f->dc && f->dcnext && !f->dchead)) return update_one(f, parent, depth);
  if (f->grpdone) return f->grpfail ? R_FAIL : R_OK;
  int failed = 0;
  for (file *e = f; e; e = e->dcnext) {
    int r = update_one(e, parent, depth);
    if (r == R_RUN) return R_RUN;
    if (r == R_FAIL) { failed = 1; if (!opt_k) break; }
    /* the next rule sees the time stamp the previous one left */
    if (e->ran) { f->mtime = MT_UNKNOWN; for (file *x = f->dcnext; x; x = x->dcnext) x->mtime = MT_UNKNOWN; }
  }
  f->grpdone = 1;
  f->grpfail = failed;
  if (f->ran == 0) for (file *e = f->dcnext; e; e = e->dcnext) if (e->ran) { f->ran = 1; f->changed = 1; }
  return failed ? R_FAIL : R_OK;
}

int update_goals(file **goals, int n, int mf_mode) {
  makefiles_phase = mf_mode;
  int *started = xcalloc(n, sizeof *started);
  int *reported = xcalloc(n, sizeof *reported);
  int *res = xcalloc(n, sizeof *res);
  for (int i = 0; i < n; i++) res[i] = -2;
  for (;;) {
    int pending = 0;
    for (int i = 0; i < n; i++) {
      if (res[i] == R_OK || res[i] == R_FAIL) continue;
      if (stop_now && !opt_k) { res[i] = R_FAIL; continue; }
      int before = commands_started;
      int r = update_file(goals[i], 0, 0);
      if (commands_started != before) started[i] = 1;
      res[i] = r;
      if (r == R_RUN) { pending = 1; if (job_slots == 1) break; continue; }
      if (!reported[i]) {
        reported[i] = 1;
        file *g = goals[i];
        if (r == R_OK && !started[i] && !mf_mode && !opt_q && !opt_s && !all_silent) {
          int nothing = g->phony || !g->cmds;
          if (g->dc && g->dcnext) nothing = 0;
          fflush(stderr);
          if (nothing) printf("%s: Nothing to be done for '%s'.\n", prefix_str(), g->name);
          else printf("%s: '%s' is up to date.\n", prefix_str(), g->name);
          fflush(stdout);
        }
      }
      if (r == R_FAIL && !opt_k && !mf_mode) { stop_now = stop_now ? stop_now : 1; }
    }
    if (!pending && njobs == 0) break;
    if (njobs > 0) reap(1);
    else if (!pending) break;
  }
  while (njobs > 0) reap(1);
  int fail = 0;
  for (int i = 0; i < n; i++) if (res[i] == R_FAIL) fail = 1;
  free(started); free(reported); free(res);
  if (any_error && !mf_mode) fail = 1;
  return fail;
}

int run_jobs_until_idle(void) {
  while (njobs > 0) reap(1);
  return 0;
}

void delete_intermediates(void) {
  if (all_secondary) return;
  int any = 0;
  for (int i = 0; i < nmade; i++) {
    file *f = made_order[i];
    if (!f->in_db) continue;
    if (!f->intermediate || f->secondary || f->notintermediate || f->phony || !f->ran) continue;
    if (f->mtime_before != MT_MISSING) continue;
    if (is_precious(f)) continue;
    sysstat st;
    if (!opt_n && (sys_stat(f->name, &st) != 0 || !st.exists)) continue;
    if (!opt_s && !all_silent) {
      if (!any) fputs("rm ", stdout);
      else fputc(' ', stdout);
      fputs(f->name, stdout);
    }
    any = 1;
    if (!opt_n) sys_unlink(f->name);
  }
  if (any && !opt_s && !all_silent) { fputc('\n', stdout); fflush(stdout); }
}

/* Ctrl-C (native build): delete what was being made, as GNU Make does */
void handle_interrupt(int sig) {
  job *list[2] = {sync_job, running};
  for (int k = 0; k < 2; k++)
    for (job *j = list[k]; j; j = k ? j->next : 0) {
      if (j->pid > 0 && k) sys_kill(j->pid, sig);
      delete_target(j->f, 0);
      int i = j->cur > 0 ? j->cur - 1 : 0;
      recipe_error(j->f, j->n ? j->lineno[i] : 0, sig == 2 ? "Interrupt" : sig == 15 ? "Terminated" : "Hangup", 0);
      if (!k) break;
    }
}

void die_cleanup(void) {
  while (njobs > 0) reap(1);
}
int is_running_any(void) { return njobs; }
