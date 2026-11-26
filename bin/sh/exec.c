// SPDX-License-Identifier: GPL-2.0-or-later
/* Execution without fork: external programs are started with spawn(); builtins,
 * functions and compound commands run inside the shell, with redirections
 * applied to the shell's own descriptors and undone afterwards. */
#include "sh.h"
#include <poll.h>

int last_status, last_bg_pid, interactive, opt_x, brk, cont, retflag, in_func, loop_depth;
int exit_pending, exit_code, exec_keep;
extern int expand_error;
#define STOP (brk || cont || retflag || exit_pending)

/* ---- output, and $(...) capture ---- */
struct cap { int rfd; char *buf; size_t len, cap; };
static struct cap caps[16];
static int ncaps;
static void cap_append(struct cap *c, const char *s, size_t n) {
  if (c->len + n + 1 > c->cap) { while (c->len + n + 1 > c->cap) c->cap = c->cap ? c->cap * 2 : 4096; c->buf = realloc(c->buf, c->cap); }
  memcpy(c->buf + c->len, s, n);
  c->len += n;
}
/* pull whatever is waiting in the capture pipes, so writers never block on them */
static void drain_caps(int timeout_ms) {
  struct pollfd pf[16];
  for (int i = 0; i < ncaps; i++) { pf[i].fd = caps[i].rfd; pf[i].events = POLLIN; pf[i].revents = 0; }
  if (poll(pf, ncaps, timeout_ms) <= 0) return;
  for (int i = 0; i < ncaps; i++) {
    if (!(pf[i].revents & POLLIN)) continue;
    char tmp[4096];
    for (;;) {
      long r = read(caps[i].rfd, tmp, sizeof tmp);
      if (r <= 0) break;
      cap_append(&caps[i], tmp, r);
      struct pollfd one = {caps[i].rfd, POLLIN, 0};
      if (poll(&one, 1, 0) <= 0 || !(one.revents & POLLIN)) break;
    }
  }
}
void sh_write(int fd, const char *s, size_t n) {
  while (n) {
    size_t c = n < 2048 ? n : 2048;
    if (write(fd, s, c) < 0) return;
    s += c; n -= c;
    if (ncaps) drain_caps(0);
  }
}
void outs(int fd, const char *s) { sh_write(fd, s, strlen(s)); }
void outf(int fd, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n >= (int)sizeof buf) {
    char *big = malloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    sh_write(fd, big, n);
    free(big);
  } else sh_write(fd, buf, n);
}

static int save_fd(int fd) {
  int s = fcntl(fd, F_DUPFD, 10);
  if (s >= 0) fcntl(s, F_SETFD, FD_CLOEXEC);
  return s;
}

/* ---- redirections ---- */
struct saved { int fd[16], save[16], n; };
static void remember(struct saved *sv, int fd) {
  for (int i = 0; i < sv->n; i++) if (sv->fd[i] == fd) return;
  if (sv->n >= 16) return;
  sv->fd[sv->n] = fd;
  sv->save[sv->n++] = save_fd(fd);
}
static void restore(struct saved *sv) {
  for (int i = sv->n - 1; i >= 0; i--) {
    if (exec_keep) { if (sv->save[i] >= 0) close(sv->save[i]); continue; }
    if (sv->save[i] >= 0) { dup2(sv->save[i], sv->fd[i]); close(sv->save[i]); }
    else close(sv->fd[i]);
  }
  sv->n = 0;
}
/* a subshell without fork: remember cwd, descriptors, positional parameters
 * and variables, and put them all back afterwards */
struct sub { char cwd[512]; int have_cwd, argc; char **argv; struct saved sv; };
static void sub_begin(struct sub *s) {
  s->have_cwd = getcwd(s->cwd, sizeof s->cwd) != NULL;
  memset(&s->sv, 0, sizeof s->sv);
  for (int fd = 0; fd < 10; fd++) remember(&s->sv, fd);
  s->argc = frame->argc;
  s->argv = frame->argv;
  vars_enter();
}
static void sub_end(struct sub *s) {
  vars_leave();
  frame->argc = s->argc;
  frame->argv = s->argv;
  int keep = exec_keep;
  exec_keep = 0;
  restore(&s->sv);
  exec_keep = keep;
  if (s->have_cwd) chdir(s->cwd);
  if (exit_pending) exit_pending = 0;
}
char *capture(const char *text, int *status) {
  int p[2];
  if (ncaps >= 16 || pipe2(p, O_CLOEXEC) < 0) { outs(2, "sh: cannot create pipe\n"); *status = 1; return strdup(""); }
  fflush(stdout);
  /* keep the pipe above fd 9, out of the way of the subshell's fd bookkeeping */
  int r0 = save_fd(p[0]), w0 = save_fd(p[1]);
  close(p[0]); close(p[1]);
  p[0] = r0; p[1] = w0;
  struct sub sb;
  sub_begin(&sb);
  dup2(p[1], 1);
  close(p[1]);
  struct cap *c = &caps[ncaps++];
  memset(c, 0, sizeof *c);
  c->rfd = p[0];
  int lb = brk, lc = cont, lr = retflag;
  brk = cont = retflag = 0;
  *status = run_text(text, NULL);
  if (exit_pending) *status = exit_code;
  brk = lb; cont = lc; retflag = lr;
  sub_end(&sb);
  char tmp[4096];
  long r;
  while ((r = read(p[0], tmp, sizeof tmp)) > 0) cap_append(&caps[ncaps - 1], tmp, r);
  struct cap done = caps[--ncaps];
  close(done.rfd);
  if (!done.buf) return strdup("");
  done.buf[done.len] = 0;
  /* a NUL inside the output would cut it short, as in other shells */
  return done.buf;
}

static int tmpcount;
static char *tmpname(void) {
  char *s = malloc(48);
  snprintf(s, 48, "/tmp/.sh-%d-%d", getpid(), tmpcount++);
  return s;
}
static int heredoc_fd(const char *body) {
  size_t n = strlen(body);
  if (n < 60000) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    write(p[1], body, n);
    close(p[1]);
    return p[0];
  }
  char *name = tmpname();
  int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) { write(fd, body, n); close(fd); fd = open(name, O_RDONLY | O_CLOEXEC); }
  unlink(name);
  free(name);
  return fd;
}
static int all_digits(const char *s) { if (!*s) return 0; for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }

static int apply_redirs(struct redir *r, struct saved *sv) {
  for (; r; r = r->next) {
    if (r->type == R_HEREDOC) {
      char *body = (r->quoted & 1) ? strdup(r->word) : expand_str(r->word, 1);
      int fd = heredoc_fd(body);
      free(body);
      if (fd < 0) { outs(2, "sh: cannot create here-document\n"); return -1; }
      remember(sv, r->fd);
      dup2(fd, r->fd);
      close(fd);
      continue;
    }
    char *w = expand_str(r->word, 0);
    int type = r->type;
    if (expand_error) { free(w); return -1; }
    if (type == R_DUPIN || type == R_DUPOUT) {
      if (!strcmp(w, "-")) { remember(sv, r->fd); close(r->fd); free(w); continue; }
      if (all_digits(w)) {
        int src = atoi(w);
        free(w);
        if (fcntl(src, F_GETFD) < 0) { outf(2, "sh: %d: bad file descriptor\n", src); return -1; }
        if (src == r->fd) continue;
        remember(sv, r->fd);
        dup2(src, r->fd);
        continue;
      }
      if (type == R_DUPIN) { outf(2, "sh: %s: bad file descriptor\n", w); free(w); return -1; }
      type = R_BOTH;
    }
    int flags = type == R_IN ? O_RDONLY : type == R_RDWR ? O_RDWR | O_CREAT :
                (type == R_APPEND || type == R_BOTHAPP) ? O_WRONLY | O_CREAT | O_APPEND : O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(w, flags | O_CLOEXEC, 0644);
    if (fd < 0) { outf(2, "sh: %s: %s\n", w, strerror(errno)); free(w); return -1; }
    free(w);
    if (type == R_BOTH || type == R_BOTHAPP) {
      remember(sv, 1); remember(sv, 2);
      dup2(fd, 1); dup2(fd, 2);
    } else {
      remember(sv, r->fd);
      dup2(fd, r->fd);
    }
    close(fd);
  }
  return 0;
}

/* ---- jobs ---- */
struct job { int id, n; pid_t pids[16]; char done[16]; char text[64]; };
static struct job jobs[32];
static int njobs;
void reap_jobs(int report) {
  for (int j = 0; j < njobs; j++) {
    struct job *jb = &jobs[j];
    int all = 1;
    for (int i = 0; i < jb->n; i++) {
      if (jb->done[i]) continue;
      int st;
      if (waitpid(jb->pids[i], &st, WNOHANG) == jb->pids[i] || errno == ECHILD) jb->done[i] = 1;
      else all = 0;
    }
    if (all) {
      if (report) outf(2, "[%d]+  Done                    %s\n", jb->id, jb->text);
      jobs[j--] = jobs[--njobs];
    }
  }
}
int wait_jobs(void) {
  int st = 0;
  for (int j = 0; j < njobs; j++)
    for (int i = 0; i < jobs[j].n; i++) if (!jobs[j].done[i]) { waitpid(jobs[j].pids[i], &st, 0); jobs[j].done[i] = 1; }
  njobs = 0;
  return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

static void fg_begin(pid_t pg, pid_t *pids, int n) {
  if (!interactive) return;
  for (int i = 0; i < n; i++) if (pids[i] > 0) __syscall2(SYS_setpgid, pids[i], pg);
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &pg);
}
static void fg_end(void) {
  if (!interactive) return;
  pid_t me = getpid();
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
}
static int wait_pid(pid_t pid) {
  int st = 0;
  if (!ncaps) { if (waitpid(pid, &st, 0) < 0) return 127; }
  else for (;;) { /* keep draining $(...) pipes while the child runs */
    long r = waitpid(pid, &st, WNOHANG);
    if (r == pid) break;
    if (r < 0) return 127;
    drain_caps(3);
  }
  if (WIFSIGNALED(st)) {
    int sig = WTERMSIG(st);
    if (sig == SIGSEGV) outs(2, "Segmentation fault\n");
    else if (sig == SIGKILL) outs(2, "Killed\n");
    else if (sig == SIGTERM) outs(2, "Terminated\n");
    else if (sig == SIGINT && interactive) outs(2, "\n");
    return 128 + sig;
  }
  return WEXITSTATUS(st);
}

/* ---- commands ---- */
char *find_in_path(const char *name) {
  struct stat st;
  if (!*name) return NULL;
  if (strchr(name, '/')) return stat(name, &st) == 0 && !S_ISDIR(st.st_mode) ? strdup(name) : NULL;
  const char *path = getvar("PATH");
  if (!path) path = "/bin";
  char buf[512];
  while (1) {
    const char *e = strchr(path, ':');
    size_t n = e ? (size_t)(e - path) : strlen(path);
    if (n + strlen(name) + 2 < sizeof buf) {
      if (n) { memcpy(buf, path, n); buf[n] = '/'; strcpy(buf + n + 1, name); }
      else strcpy(buf, name);
      if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) return strdup(buf);
    }
    if (!e) break;
    path = e + 1;
  }
  return NULL;
}

enum { K_EXT, K_BUILTIN, K_FUNC, K_COMPOUND, K_EMPTY, K_FAIL };
struct prep { node *n; char **argv; int argc, kind; builtin_fn bf; node *fn; };

static void trace(char **argv) {
  if (!opt_x) return;
  outs(2, "+");
  for (char **a = argv; *a; a++) outf(2, " %s", *a);
  outs(2, "\n");
}
static void prepare(node *n, struct prep *pr) {
  memset(pr, 0, sizeof *pr);
  pr->n = n;
  if (n->type != N_CMD) { pr->kind = K_COMPOUND; return; }
  expand_error = 0;
  pr->argc = expand_words(n->words, n->nwords, &pr->argv);
  if (expand_error) { pr->kind = K_FAIL; return; }
  if (!pr->argc) { pr->kind = K_EMPTY; return; }
  trace(pr->argv);
  if ((pr->fn = getfunc(pr->argv[0]))) pr->kind = K_FUNC;
  else if ((pr->bf = find_builtin(pr->argv[0]))) pr->kind = K_BUILTIN;
  else pr->kind = K_EXT;
}

/* NAME=value prefixes. persist: plain assignment; otherwise the old values are
 * returned in *old for restore_assigns. */
static int do_assigns(node *n, int persist, char ***old) {
  if (old) *old = n->nassigns ? calloc(n->nassigns * 2 + 1, sizeof(char *)) : NULL;
  for (int i = 0; i < n->nassigns; i++) {
    char *eq = strchr(n->assigns[i], '=');
    char *name = strndup(n->assigns[i], eq - n->assigns[i]);
    expand_error = 0;
    char *val = expand_str(eq + 1, 0);
    if (expand_error) { free(name); free(val); return -1; }
    if (opt_x) outf(2, "+ %s=%s\n", name, val);
    if (!persist && old) {
      char *o = getvar(name);
      (*old)[2 * i] = strdup(name);
      (*old)[2 * i + 1] = o ? strdup(o) : NULL;
      setvar(name, val, 1);
    } else setvar(name, val, 0);
    free(name); free(val);
  }
  return 0;
}
static void restore_assigns(node *n, char **old) {
  if (!old) return;
  for (int i = 0; i < n->nassigns; i++) {
    if (!old[2 * i]) continue;
    if (old[2 * i + 1]) setvar(old[2 * i], old[2 * i + 1], 0); else unsetvar(old[2 * i]);
    free(old[2 * i]); free(old[2 * i + 1]);
  }
  free(old);
}

struct local { char *name, *val; int had; struct local *next; };
static int call_func(node *fn, int argc, char **argv) {
  struct frame f = {argc - 1, argv + 1, NULL, frame};
  frame = &f;
  in_func++;
  int st = run(fn->a);
  if (retflag) { retflag = 0; st = last_status; }
  in_func--;
  for (struct local *l = f.locals, *nx; l; l = nx) {
    nx = l->next;
    if (l->had) setvar(l->name, l->val, 0); else unsetvar(l->name);
    free(l->name); free(l->val); free(l);
  }
  frame = f.up;
  return st;
}
void local_var(const char *name) {
  if (!frame->up) return;
  for (struct local *l = frame->locals; l; l = l->next) if (!strcmp(l->name, name)) return;
  struct local *l = calloc(1, sizeof *l);
  l->name = strdup(name);
  char *v = getvar(name);
  l->had = v != NULL;
  l->val = v ? strdup(v) : NULL;
  l->next = frame->locals;
  frame->locals = l;
}

static int run_compound(node *n);

static int run_inproc(struct prep *pr) {
  struct saved sv = {0};
  node *n = pr->n;
  int st = 0;
  if (pr->kind == K_FAIL) return 1;
  int subst_before = last_status;
  last_status = 0;
  if (pr->kind == K_EMPTY) {
    if (do_assigns(n, 1, NULL) < 0) return 1;
    st = last_status; /* the status of the last $(...), or 0 */
    if (n->redir) { if (apply_redirs(n->redir, &sv) < 0) st = 1; restore(&sv); }
    return st;
  }
  last_status = subst_before;
  if (apply_redirs(n->redir, &sv) < 0) { restore(&sv); return 1; }
  if (pr->kind == K_COMPOUND) st = run_compound(n);
  else {
    char **old;
    if (do_assigns(n, 0, &old) < 0) { restore(&sv); return 1; }
    exec_keep = 0;
    if (pr->kind == K_BUILTIN) st = pr->bf(pr->argc, pr->argv);
    else st = call_func(pr->fn, pr->argc, pr->argv);
    restore_assigns(n, old);
  }
  restore(&sv);
  exec_keep = 0;
  return st;
}

static char **make_env(node *n) {
  char **base = build_env();
  int nb = 0;
  while (base[nb]) nb++;
  char **env = malloc((nb + n->nassigns + 1) * sizeof *env);
  memcpy(env, base, nb * sizeof *env);
  int ne = nb;
  for (int i = 0; i < n->nassigns; i++) {
    char *eq = strchr(n->assigns[i], '=');
    size_t nl = eq - n->assigns[i];
    char *val = expand_str(eq + 1, 0);
    char *kv = malloc(nl + strlen(val) + 2);
    memcpy(kv, n->assigns[i], nl + 1);
    strcpy(kv + nl + 1, val);
    free(val);
    int j;
    for (j = 0; j < ne; j++) if (!strncmp(env[j], kv, nl + 1)) break;
    env[j] = kv;
    if (j == ne) ne++;
  }
  env[ne] = NULL;
  return env;
}

/* start an external program; in/out (when >= 0) become its stdin/stdout */
static pid_t spawn_ext(struct prep *pr, int in, int out, int *st) {
  struct saved sv = {0};
  pid_t pid = -1;
  if (in >= 0) { remember(&sv, 0); dup2(in, 0); }
  if (out >= 0) { remember(&sv, 1); dup2(out, 1); }
  if (apply_redirs(pr->n->redir, &sv) < 0) { restore(&sv); *st = 1; return -1; }
  char *path = find_in_path(pr->argv[0]);
  if (!path) {
    outf(2, "sh: %s: %s\n", pr->argv[0], strchr(pr->argv[0], '/') ? "No such file or directory" : "not found");
    *st = 127;
  } else {
    char **env = make_env(pr->n);
    fflush(stdout);
    pid = spawn(path, pr->argv, env, NULL);
    if (pid < 0 && errno == ENOEXEC) { /* not a program: run it as a shell script */
      char **av = malloc((pr->argc + 2) * sizeof *av);
      av[0] = "sh"; av[1] = path;
      memcpy(av + 2, pr->argv + 1, pr->argc * sizeof *av);
      pid = spawn("/bin/sh", av, env, NULL);
      free(av);
    }
    if (pid < 0) {
      outf(2, "sh: %s: %s\n", pr->argv[0], errno == 7 ? "argument list too long" : strerror(errno));
      *st = errno == ENOENT ? 127 : 126;
    }
    for (int i = 0; env[i]; i++) {
      int base = 0;
      for (char **b = build_env(); *b; b++) if (*b == env[i]) base = 1;
      if (!base) free(env[i]);
    }
    free(env);
    free(path);
  }
  restore(&sv);
  return pid;
}

static int is_inproc(struct prep *pr) { return pr->kind != K_EXT; }

static int run_pipeline(node **kids, int n, int bang, int bg) {
  struct prep *pr = calloc(n, sizeof *pr);
  int *in = malloc(n * sizeof(int)), *out = malloc(n * sizeof(int)), *st = calloc(n, sizeof(int));
  pid_t *pids = calloc(n, sizeof(pid_t));
  char **tmp = calloc(n, sizeof(char *));
  for (int i = 0; i < n; i++) { prepare(kids[i], &pr[i]); in[i] = out[i] = -1; }
  for (int i = 0; i + 1 < n; i++) {
    if (is_inproc(&pr[i]) && is_inproc(&pr[i + 1])) { /* both run here, one after the other: use a file */
      tmp[i] = tmpname();
      out[i] = open(tmp[i], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      in[i + 1] = -2;
    } else {
      int p[2];
      if (pipe2(p, O_CLOEXEC) < 0) { outs(2, "sh: cannot create pipe\n"); p[0] = p[1] = -1; }
      out[i] = p[1]; in[i + 1] = p[0];
    }
  }
  pid_t pg = 0;
  int npids = 0;
  for (int i = 0; i < n; i++) {
    if (is_inproc(&pr[i])) continue;
    pids[i] = spawn_ext(&pr[i], in[i], out[i], &st[i]);
    if (pids[i] > 0) {
      if (!pg) pg = pids[i];
      if (interactive && !bg) __syscall2(SYS_setpgid, pids[i], pg);
      npids++;
    }
    if (in[i] >= 0) close(in[i]);
    if (out[i] >= 0) close(out[i]);
    in[i] = out[i] = -1;
  }
  if (pg && !bg) fg_begin(pg, pids, n);
  for (int i = 0; i < n; i++) {
    if (!is_inproc(&pr[i])) continue;
    if (in[i] == -2) in[i] = open(tmp[i - 1], O_RDONLY | O_CLOEXEC);
    if (n == 1) { st[i] = run_inproc(&pr[i]); continue; }
    struct saved sv = {0};
    if (in[i] >= 0) { remember(&sv, 0); dup2(in[i], 0); }
    if (out[i] >= 0) { remember(&sv, 1); dup2(out[i], 1); }
    st[i] = run_inproc(&pr[i]);
    restore(&sv);
    if (in[i] >= 0) close(in[i]);
    if (out[i] >= 0) close(out[i]);
    if (i > 0 && tmp[i - 1]) unlink(tmp[i - 1]);
  }
  if (bg && npids) {
    if (njobs < 32) {
      struct job *jb = &jobs[njobs++];
      memset(jb, 0, sizeof *jb);
      static int nextid = 1;
      jb->id = njobs == 1 ? (nextid = 1) : ++nextid;
      for (int i = 0; i < n && jb->n < 16; i++) if (pids[i] > 0) jb->pids[jb->n++] = pids[i];
      snprintf(jb->text, sizeof jb->text, "%s", pr[0].argv && pr[0].argv[0] ? pr[0].argv[0] : "");
      for (int i = 1; i < pr[0].argc && strlen(jb->text) < 50; i++) { strcat(jb->text, " "); strncat(jb->text, pr[0].argv[i], 60 - strlen(jb->text)); }
      if (interactive) outf(2, "[%d] %d\n", jb->id, pids[n - 1] > 0 ? pids[n - 1] : pg);
    }
    for (int i = n - 1; i >= 0; i--) if (pids[i] > 0) { last_bg_pid = pids[i]; break; }
  } else {
    for (int i = 0; i < n; i++) if (pids[i] > 0) st[i] = wait_pid(pids[i]);
    if (pg) fg_end();
  }
  int status = bg ? 0 : st[n - 1];
  for (int i = 0; i < n; i++) { free_argv(pr[i].argv); free(tmp[i]); }
  free(pr); free(in); free(out); free(st); free(pids); free(tmp);
  return bang ? !status : status;
}

static int loop_end(int *stop) {
  /* after a loop body: 1 = leave the loop */
  if (brk) { brk--; *stop = 1; return 1; }
  if (cont) { if (--cont) { *stop = 1; return 1; } return 0; }
  if (retflag || exit_pending) { *stop = 1; return 1; }
  return 0;
}

static int run_compound(node *n) {
  int st = 0, stop = 0;
  switch (n->type) {
  case N_GROUP: return run(n->a);
  case N_SUBSHELL: {
    struct sub sb;
    sub_begin(&sb);
    st = run(n->a);
    if (exit_pending) st = exit_code;
    sub_end(&sb);
    return st;
  }
  case N_IF: {
    int c = run(n->a);
    if (STOP) return c;
    if (!c) return run(n->b);
    return n->c ? run(n->c) : 0;
  }
  case N_WHILE: case N_UNTIL:
    loop_depth++;
    for (;;) {
      int c = run(n->a);
      if (STOP) { loop_end(&stop); break; }
      if ((c == 0) != (n->type == N_WHILE)) break;
      st = run(n->b);
      if (loop_end(&stop)) break;
    }
    loop_depth--;
    return st;
  case N_FOR: {
    char **words;
    int nw;
    if (n->nwords < 0) {
      nw = frame->argc;
      words = calloc(nw + 1, sizeof *words);
      for (int i = 0; i < nw; i++) words[i] = strdup(frame->argv[i]);
    } else {
      expand_error = 0;
      nw = expand_words(n->words, n->nwords, &words);
      if (expand_error) { free_argv(words); return 1; }
    }
    loop_depth++;
    for (int i = 0; i < nw; i++) {
      setvar(n->name, words[i], 0);
      st = run(n->b);
      if (loop_end(&stop)) break;
    }
    loop_depth--;
    free_argv(words);
    return st;
  }
  case N_CASE: {
    char *w = expand_str(n->name, 0);
    for (struct casei *ci = n->cases; ci; ci = ci->next)
      for (int i = 0; i < ci->npats; i++) {
        char *p = expand_pattern(ci->pats[i], NULL);
        int m = pmatch(p, w);
        free(p);
        if (m) { free(w); return run(ci->body); }
      }
    free(w);
    return 0;
  }
  }
  return run(n);
}

int run(node *n) {
  int st = 0;
  if (!n) return last_status = 0;
  switch (n->type) {
  case N_SEQ:
    run(n->a);
    if (STOP) return last_status;
    st = run(n->b);
    break;
  case N_AND: case N_OR:
    st = run(n->a);
    if (!STOP && ((st == 0) == (n->type == N_AND))) st = run(n->b);
    break;
  case N_BG:
    if (n->a->type == N_CMD || n->a->type == N_PIPE) {
      node *a = n->a;
      st = a->type == N_PIPE ? run_pipeline(a->kids, a->nkids, a->bang, 1) : run_pipeline(&n->a, 1, 0, 1);
    } else st = run(n->a); /* no fork: a background list runs in the foreground */
    break;
  case N_FUNC:
    setfunc(n->name, n->a);
    st = 0;
    break;
  case N_PIPE:
    st = run_pipeline(n->kids, n->nkids, n->bang, 0);
    break;
  default:
    st = run_pipeline(&n, 1, 0, 0);
  }
  return last_status = st;
}

int run_text(const char *text, const char *name) {
  struct parser p;
  struct arena ar = {0};
  parser_init(&p, text, &ar);
  for (;;) {
    size_t start = p.s == text ? p.pos : 0;
    node *n = parse_one(&p);
    if (p.status == P_END) break;
    if (p.status != P_OK) {
      int line = 1;
      for (size_t k = 0; k < start && text[k]; k++) line += text[k] == '\n';
      while (text[start] == '\n') start++, line++;
      if (name) outf(2, "sh: %s: line %d: %s\n", name, line, p.status == P_INCOMPLETE ? "syntax error: unexpected end of file" : p.errmsg);
      else outf(2, "sh: %s\n", p.status == P_INCOMPLETE ? "syntax error: unexpected end of file" : p.errmsg);
      last_status = 2;
      break;
    }
    run(n);
    if (STOP) break;
  }
  if (!p.keep) afree(&ar);
  return last_status;
}
