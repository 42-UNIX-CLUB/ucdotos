// SPDX-License-Identifier: GPL-2.0-or-later
/* Execution without fork: external programs are started with spawn(); builtins,
 * functions and compound commands run inside the shell, with redirections
 * applied to the shell's own descriptors and undone afterwards. */
#include "sh.h"
#include <poll.h>
#include <signal.h>

int last_status, last_bg_pid, interactive, opt_x, brk, cont, retflag, in_func, loop_depth;
int exit_pending, exit_code, exec_keep;
int lineno, cmd_nest; /* cmd_nest: inside "command NAME": special builtins lose their special rules */
extern int expand_error, subst_ran;
#define STOP (brk || cont || retflag || exit_pending || intr_pending)
/* LASTPIPE_IN_SHELL: run the last stage of a multi-stage pipeline in this
 * shell (as ksh and zsh do: `echo x | read v` sets v) instead of a subshell
 * (as dash and bash do). Off by the owners' choice (POSIX allows both); kept
 * for a possible zsh-like mode later. Not a user option. */
#define LASTPIPE_IN_SHELL 0
int intr_pending;
/* Ctrl-C while the shell itself is busy (a loop of builtins, say): the kernel
 * only sets a flag for us. Checking it is also a preemption point. */
int check_intr(void) {
  if (!intr_pending && __syscall0(SYS_intr) == 1) intr_pending = 1;
  if (traps_waiting) run_pending_traps();
  return intr_pending;
}
/* an expansion error: a non-interactive shell exits (POSIX 2.8.1) */
static int exp_failed(void) {
  if (!interactive) { exit_pending = 1; exit_code = 2; }
  return 2;
}

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
struct sub;
static struct sub *cur_sub;
void sh_write(int fd, const char *s, size_t n) {
  while (n) {
    size_t c = n < 2048 ? n : 2048;
    if (write(fd, s, c) < 0) {
      /* nobody reads the pipe: a forked subshell would die of SIGPIPE; ours ends */
      if (errno == EPIPE && cur_sub && !exit_pending) { exit_pending = 1; exit_code = 128 + SIGPIPE; }
      return;
    }
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
static int remembered(struct saved *sv, int fd) {
  for (int i = 0; i < sv->n; i++) if (sv->fd[i] == fd) return 1;
  return 0;
}
static void remember(struct saved *sv, int fd) {
  if (remembered(sv, fd) || sv->n >= 16) return;
  sv->fd[sv->n] = fd;
  sv->save[sv->n++] = save_fd(fd);
}

/* A subshell without fork: what it changes is put back when it ends.
 * Variables, functions and aliases are journaled (main.c); descriptors, the
 * directory and the umask are saved only when something in the subshell is
 * about to change them for good (exec, cd, umask), so a $(...) that does none
 * of that costs no system calls for it. */
struct sub {
  char cwd[512];
  int have_cwd, argc, umask, bg;
  char **argv;
  struct saved sv;
  struct trapsave traps;
  struct optsave opts;
  struct sub *up;
};
static void restore(struct saved *sv) {
  for (int i = sv->n - 1; i >= 0; i--) {
    if (exec_keep) { /* exec: the change stays; an enclosing subshell undoes it at its end */
      if (cur_sub && sv != &cur_sub->sv && !remembered(&cur_sub->sv, sv->fd[i]) && cur_sub->sv.n < 16) {
        cur_sub->sv.fd[cur_sub->sv.n] = sv->fd[i];
        cur_sub->sv.save[cur_sub->sv.n++] = sv->save[i];
      } else if (sv->save[i] >= 0) close(sv->save[i]);
      continue;
    }
    if (sv->save[i] >= 0) { dup2(sv->save[i], sv->fd[i]); close(sv->save[i]); }
    else close(sv->fd[i]);
  }
  sv->n = 0;
}
void sub_saw_cd(void) {
  if (cur_sub && !cur_sub->have_cwd) cur_sub->have_cwd = getcwd(cur_sub->cwd, sizeof cur_sub->cwd) != NULL;
}
void sub_saw_umask(int old) { if (cur_sub && cur_sub->umask < 0) cur_sub->umask = old; }
static void sub_begin(struct sub *s) {
  s->have_cwd = 0;
  s->umask = -1;
  s->sv.n = 0;
  s->argc = frame->argc;
  s->argv = frame->argv;
  s->bg = last_bg_pid;
  traps_enter(&s->traps);
  opts_save(&s->opts);
  s->up = cur_sub;
  cur_sub = s;
  vars_enter();
}
/* returns the subshell's status: st, or the code it exited with */
static int sub_end(struct sub *s, int st) {
  if (exit_pending) st = exit_code;
  last_status = st;
  traps_leave(&s->traps); /* runs the subshell's own EXIT trap, inside it */
  if (exit_pending) st = exit_code;
  vars_leave();
  frame->argc = s->argc;
  frame->argv = s->argv;
  last_bg_pid = s->bg;
  opts_restore(&s->opts);
  cur_sub = s->up;
  int keep = exec_keep;
  exec_keep = 0;
  restore(&s->sv);
  exec_keep = keep;
  if (s->have_cwd) chdir(s->cwd);
  if (s->umask >= 0) set_mask(s->umask);
  exit_pending = brk = cont = retflag = 0;
  return st;
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
  remember(&sb.sv, 1);
  dup2(p[1], 1);
  close(p[1]);
  struct cap *c = &caps[ncaps++];
  memset(c, 0, sizeof *c);
  c->rfd = p[0];
  int lb = brk, lc = cont, lr = retflag;
  brk = cont = retflag = 0;
  *status = sub_end(&sb, run_text(text, NULL));
  brk = lb; cont = lc; retflag = lr;
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

/* make fd a copy of src (or close it when src < 0), remembering the old one first */
static void move_fd(struct saved *sv, int src, int fd) {
  if (src == fd) { fcntl(fd, F_SETFD, 0); return; }
  dup2(src, fd);
  close(src);
}
static int apply_redirs(struct redir *r, struct saved *sv) {
  for (; r; r = r->next) {
    if (r->type == R_HEREDOC) {
      char *body = (r->quoted & 1) ? strdup(r->word) : expand_str(r->word, 1);
      if (expand_error) { free(body); return -1; }
      remember(sv, r->fd); /* before the pipe exists: it may get this very number */
      int fd = heredoc_fd(body);
      free(body);
      if (fd < 0) { outs(2, "sh: cannot create here-document\n"); return -1; }
      move_fd(sv, fd, r->fd);
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
        if (src == r->fd) { if (fcntl(src, F_GETFD) < 0) { outf(2, "sh: %d: bad file descriptor\n", src); return -1; } continue; }
        if (fcntl(src, F_GETFD) < 0) { outf(2, "sh: %d: bad file descriptor\n", src); return -1; }
        remember(sv, r->fd);
        dup2(src, r->fd);
        continue;
      }
      if (type == R_DUPIN) { outf(2, "sh: %s: bad file descriptor\n", w); free(w); return -1; }
      type = R_BOTH;
    }
    int flags = type == R_IN ? O_RDONLY : type == R_RDWR ? O_RDWR | O_CREAT :
                (type == R_APPEND || type == R_BOTHAPP) ? O_WRONLY | O_CREAT | O_APPEND : O_WRONLY | O_CREAT | O_TRUNC;
    if (opt_C && (type == R_OUT || type == R_BOTH)) { /* noclobber: > must not replace an existing regular file */
      struct stat st;
      if (stat(w, &st) == 0 && S_ISREG(st.st_mode)) { outf(2, "sh: %s: cannot overwrite existing file\n", w); free(w); return -1; }
      if (stat(w, &st) != 0) flags |= O_EXCL;
    }
    if (type == R_BOTH || type == R_BOTHAPP) { remember(sv, 1); remember(sv, 2); }
    else remember(sv, r->fd); /* before open: open may return this very number */
    int fd = open(w, flags | O_CLOEXEC, 0644);
    if (fd < 0) { outf(2, "sh: %s: %s\n", w, strerror(errno)); free(w); return -1; }
    free(w);
    if (type == R_BOTH || type == R_BOTHAPP) {
      if (fd != 1) dup2(fd, 1);
      if (fd != 2) dup2(fd, 2);
      if (fd != 1 && fd != 2) close(fd);
    } else if (fd == r->fd) fcntl(fd, F_SETFD, 0); /* it is the target: drop close-on-exec */
    else move_fd(sv, fd, r->fd);
  }
  return 0;
}

/* ---- jobs ----
 * Job control (set -m) rests on kernel support that main does not have yet:
 * stop and continue signals, waitpid's WUNTRACED and WCONTINUED, and Ctrl-Z
 * (the tty's VSUSP character). jobs_init() asks the kernel once; without that
 * support set -m is refused and the shell works as before: pipelines of an
 * interactive shell still get their own process group, so Ctrl-C works. */
#ifndef WUNTRACED
#define WUNTRACED 2
#endif
#ifndef WCONTINUED
#define WCONTINUED 8
#endif
#define ST_STOPPED(s) (((s) & 0xff) == 0x7f)
#define ST_CONTINUED(s) ((s) == 0xffff)
int jc_ok; /* the kernel can stop and continue processes */
static int jc(void) { return opt_m && jc_ok; }
int job_control_active(void) { return jc(); }
void jobs_init(int tty) {
  static int probed;
  if (!probed) { /* main's kernel has no signal masks: ENOSYS there */
    unsigned long long old = 0;
    jc_ok = __syscall4(135 /* rt_sigprocmask */, 0, 0, &old, 8) == 0;
    probed = 1;
  }
  if (jc_ok && tty) opt_m = 1; /* on by default for an interactive shell */
}

struct job { int id, n, notified; pid_t pgid, pids[16]; char done[16], stopped[16]; int st[16]; char text[64]; };
static struct job jobs[32];
static int njobs, nextid;
static int decode(int st) { return WIFSIGNALED(st) && !ST_STOPPED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st); }
static int job_state(struct job *jb) { /* 0 running, 1 stopped, 2 done */
  int running = 0, stopped = 0;
  for (int i = 0; i < jb->n; i++) { if (jb->done[i]) continue; if (jb->stopped[i]) stopped = 1; else running = 1; }
  return running ? 0 : stopped ? 1 : 2;
}
static struct job *job_current(int which) { /* which 0: %+ (newest), 1: %- */
  struct job *a = NULL, *b = NULL;
  for (int j = 0; j < njobs; j++) {
    if (!a || jobs[j].id > a->id) { b = a; a = &jobs[j]; }
    else if (!b || jobs[j].id > b->id) b = &jobs[j];
  }
  return which ? b : a;
}
static void job_line(struct job *jb, const char *state, int fd) {
  outf(fd, "[%d]%c  %-24s%s\n", jb->id, jb == job_current(0) ? '+' : jb == job_current(1) ? '-' : ' ', state, jb->text);
}
static struct job *job_add(pid_t pg, pid_t *pids, int n, const char *text) {
  if (njobs == 32) reap_jobs(0); /* make room: finished jobs are forgotten */
  if (njobs == 32) return NULL;
  struct job *jb = &jobs[njobs++];
  memset(jb, 0, sizeof *jb);
  jb->id = njobs == 1 ? (nextid = 1) : ++nextid;
  jb->pgid = pg;
  for (int i = 0; i < n && jb->n < 16; i++) if (pids[i] > 0) jb->pids[jb->n++] = pids[i];
  snprintf(jb->text, sizeof jb->text, "%s", text);
  return jb;
}
static void job_remove(struct job *jb) { *jb = jobs[--njobs]; }
/* one waitpid result for a pid of a job */
static void job_note(struct job *jb, int i, int st) {
  if (ST_STOPPED(st)) jb->stopped[i] = 1;
  else if (ST_CONTINUED(st)) jb->stopped[i] = 0;
  else { jb->done[i] = 1; jb->st[i] = decode(st); }
}
void reap_jobs(int report) {
  int flags = WNOHANG | (jc_ok ? WUNTRACED | WCONTINUED : 0);
  for (int j = 0; j < njobs; j++) {
    struct job *jb = &jobs[j];
    for (int i = 0; i < jb->n; i++) {
      if (jb->done[i]) continue;
      int st = 0;
      long r = waitpid(jb->pids[i], &st, flags);
      if (r == jb->pids[i]) job_note(jb, i, st);
      else if (r < 0 && errno == ECHILD) { jb->done[i] = 1; jb->st[i] = 127; }
    }
    int state = job_state(jb);
    if (state == 2) {
      if (report) job_line(jb, "Done", 2);
      job_remove(jb);
      j--;
    } else if (state == 1 && !jb->notified) {
      if (report) job_line(jb, "Stopped", 2);
      jb->notified = 1;
    } else if (state == 0) jb->notified = 0;
  }
}
int wait_jobs(void) {
  for (int j = 0; j < njobs; j++)
    for (int i = 0; i < jobs[j].n; i++) if (!jobs[j].done[i] && !jobs[j].stopped[i]) { int st; waitpid(jobs[j].pids[i], &st, 0); jobs[j].done[i] = 1; }
  for (int j = 0; j < njobs; j++) if (job_state(&jobs[j]) == 2) job_remove(&jobs[j--]);
  return 0;
}
/* the wait builtin for one pid: its status, also when it was reaped before */
int wait_for(pid_t pid, int *found) {
  int st = 0;
  *found = 1;
  for (int j = 0; j < njobs; j++)
    for (int i = 0; i < jobs[j].n; i++) {
      if (jobs[j].pids[i] != pid) continue;
      if (!jobs[j].done[i]) {
        if (waitpid(pid, &st, 0) == pid) jobs[j].st[i] = decode(st); else jobs[j].st[i] = 127;
        jobs[j].done[i] = 1;
      }
      return jobs[j].st[i];
    }
  if (waitpid(pid, &st, 0) == pid) return decode(st);
  *found = 0;
  return 127;
}
/* %N, %%, %+, %-, %string, %?string */
static struct job *job_find(const char *spec) {
  const char *s = spec[0] == '%' ? spec + 1 : spec;
  if (!*s || !strcmp(s, "%") || !strcmp(s, "+")) return job_current(0);
  if (!strcmp(s, "-")) return job_current(1);
  if (isdigit((unsigned char)*s)) { for (int j = 0; j < njobs; j++) if (jobs[j].id == atoi(s)) return &jobs[j]; return NULL; }
  for (int j = 0; j < njobs; j++)
    if (*s == '?' ? strstr(jobs[j].text, s + 1) != NULL : !strncmp(jobs[j].text, s, strlen(s))) return &jobs[j];
  return NULL;
}
int job_pids(const char *spec, pid_t *out, int max) {
  struct job *jb = job_find(spec);
  if (!jb) return -1;
  int n = 0;
  for (int i = 0; i < jb->n && n < max; i++) out[n++] = jb->pids[i];
  return n;
}
/* kill %job: the whole process group when job control has made one */
int job_kill(const char *spec, int sig) {
  struct job *jb = job_find(spec);
  if (!jb) return -2;
  int stopped = job_state(jb) == 1, rc = 0;
  if (jc_ok && jb->pgid > 0) rc = kill(-jb->pgid, sig);
  else for (int i = 0; i < jb->n; i++) if (!jb->done[i] && kill(jb->pids[i], sig) < 0) rc = -1;
  /* a stopped job only sees a terminating signal once it runs again */
  if (rc == 0 && stopped && jc_ok && jb->pgid > 0 && sig != SIGCONT && sig != SIGSTOP && sig != SIGTSTP && sig) kill(-jb->pgid, SIGCONT);
  return rc;
}

static void fg_begin(pid_t pg, pid_t *pids, int n) {
  if (!interactive && !jc()) return;
  for (int i = 0; i < n; i++) if (pids[i] > 0) __syscall2(SYS_setpgid, pids[i], pg);
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &pg);
}
static void fg_end(void) {
  if (!interactive && !jc()) return;
  pid_t me = getpgrp();
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &me);
}
/* how a child ended, as a status; prints what other shells print for signals */
static int report(int st) {
  if (WIFSIGNALED(st) && !ST_STOPPED(st)) {
    int sig = WTERMSIG(st);
    if (sig == SIGSEGV) outs(2, "Segmentation fault\n");
    else if (sig == SIGKILL) outs(2, "Killed\n");
    else if (sig == SIGTERM) outs(2, "Terminated\n");
    else if (sig == SIGINT) intr_pending = 1; /* stop the loop or list it ran in, like other shells */
    return 128 + sig;
  }
  return WEXITSTATUS(st);
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
  return report(st);
}
/* wait for a job in the foreground until it ends or stops (job control);
 * *stopped tells which; the status is the last process's, or 128+signal */
static int job_wait_fg(struct job *jb, int *stopped, int *st_out) {
  int last = 0, stopsig = 0;
  for (int i = 0; i < jb->n; i++) {
    if (jb->done[i]) continue;
    int st = 0;
    long r;
    while ((r = waitpid(jb->pids[i], &st, WUNTRACED)) == jb->pids[i] && ST_CONTINUED(st)) {}
    if (r != jb->pids[i]) { jb->done[i] = 1; jb->st[i] = 127; continue; }
    if (ST_STOPPED(st)) { jb->stopped[i] = 1; stopsig = WEXITSTATUS(st); continue; }
    jb->done[i] = 1;
    jb->st[i] = report(st);
    if (st_out) st_out[i] = jb->st[i];
  }
  last = jb->st[jb->n - 1];
  *stopped = job_state(jb) == 1;
  if (*stopped) {
    outs(2, "\n");
    job_line(jb, "Stopped", 2);
    jb->notified = 1;
    return 128 + (stopsig ? stopsig : SIGTSTP);
  }
  return last;
}
static int need_jc(const char *who) {
  if (jc()) return 1;
  outf(2, "%s: %s\n", who, jc_ok ? "job control is off (set -m)" : "no job control on this kernel");
  return 0;
}
int job_fg(const char *spec) {
  if (!need_jc("fg")) return 1;
  struct job *jb = spec ? job_find(spec) : job_current(0);
  if (!jb) { outf(2, "fg: %s: no such job\n", spec ? spec : "current"); return 1; }
  outf(2, "%s\n", jb->text);
  for (int i = 0; i < jb->n; i++) jb->stopped[i] = 0;
  jb->notified = 0;
  __syscall3(SYS_ioctl, 0, TIOCSPGRP, &jb->pgid);
  kill(-jb->pgid, SIGCONT);
  int stopped;
  int st = job_wait_fg(jb, &stopped, NULL);
  fg_end();
  if (!stopped) job_remove(jb);
  return st;
}
int job_bg(const char *spec) {
  if (!need_jc("bg")) return 1;
  struct job *jb = spec ? job_find(spec) : job_current(0);
  if (!jb) { outf(2, "bg: %s: no such job\n", spec ? spec : "current"); return 1; }
  for (int i = 0; i < jb->n; i++) jb->stopped[i] = 0;
  jb->notified = 0;
  kill(-jb->pgid, SIGCONT);
  outf(2, "[%d] %s &\n", jb->id, jb->text);
  return 0;
}
/* jobs [-l | -p] [job...] */
int job_list(int argc, char **argv) {
  int i = 1, mode = 0, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "-l")) mode = 1; else if (!strcmp(argv[i], "-p")) mode = 2;
    else { outf(2, "jobs: %s: invalid option\n", argv[i]); return 2; }
  }
  reap_jobs(0);
  for (int j = 0; j < njobs; j++) {
    struct job *jb = &jobs[j];
    int shown = i >= argc;
    for (int k = i; k < argc; k++) if (job_find(argv[k]) == jb) shown = 1;
    if (!shown) continue;
    int state = job_state(jb);
    const char *st = state == 0 ? "Running" : state == 1 ? "Stopped" : "Done";
    if (mode == 2) { outf(1, "%d\n", (int)(jb->pgid > 0 ? jb->pgid : jb->pids[0])); continue; }
    if (mode == 1) outf(1, "[%d]%c %5d %-24s%s\n", jb->id, jb == job_current(0) ? '+' : jb == job_current(1) ? '-' : ' ', (int)jb->pids[0], st, jb->text);
    else job_line(jb, st, 1);
    if (state == 1) jb->notified = 1;
  }
  for (int k = i; k < argc; k++) if (!job_find(argv[k])) { outf(2, "jobs: %s: no such job\n", argv[k]); rc = 1; }
  for (int j = 0; j < njobs; j++) if (job_state(&jobs[j]) == 2) job_remove(&jobs[j--]);
  return rc;
}
int job_disown(int argc, char **argv) {
  if (argc < 2) { struct job *jb = job_current(0); if (jb) job_remove(jb); return 0; }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    struct job *jb = job_find(argv[i]);
    if (!jb) { outf(2, "disown: %s: no such job\n", argv[i]); rc = 1; continue; }
    job_remove(jb);
  }
  return rc;
}

/* ---- commands ---- */
char *find_in_path(const char *name) { return find_in_path2(name, NULL); }
char *find_in_path2(const char *name, const char *path) {
  struct stat st;
  if (!*name) return NULL;
  if (strchr(name, '/')) return stat(name, &st) == 0 && !S_ISDIR(st.st_mode) ? strdup(name) : NULL;
  if (!path) path = getvar("PATH");
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
/* argv0: where the command name starts in argv (after "command [-p]");
 * viacmd: run through "command"; special: a POSIX special builtin;
 * subst_st: status of the last $(...) when the command expanded to nothing */
struct prep { node *n; char **argv; int argc, kind, argv0, viacmd, special, subst_st; const char *path; builtin_fn bf; node *fn; };

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
  lineno = n->lineno;
  expand_error = 0;
  subst_ran = 0;
  pr->argc = expand_words(n->words, n->nwords, &pr->argv);
  pr->subst_st = subst_ran ? last_status : -1;
  if (expand_error) { pr->kind = K_FAIL; return; }
  if (!pr->argc) { pr->kind = K_EMPTY; return; }
  trace(pr->argv);
  /* command [-p] NAME ...: skip functions; special builtins are not special */
  char **av = pr->argv;
  while (pr->argc - pr->argv0 > 1 && !strcmp(av[pr->argv0], "command")) {
    int k = pr->argv0 + 1, p = 0, v = 0;
    for (; k < pr->argc && av[k][0] == '-' && av[k][1]; k++) {
      if (!strcmp(av[k], "--")) { k++; break; }
      if (strspn(av[k] + 1, "pvV") != strlen(av[k] + 1)) break;
      if (strpbrk(av[k] + 1, "vV")) v = 1;
      if (strchr(av[k] + 1, 'p')) p = 1;
    }
    if (v || k >= pr->argc) break; /* command -v/-V, or nothing to run: the builtin does it */
    pr->argv0 = k;
    pr->viacmd = 1;
    if (p) pr->path = "/bin";
  }
  const char *name = av[pr->argv0];
  if ((pr->bf = find_builtin(name)) && is_special_builtin(name)) { pr->kind = K_BUILTIN; pr->special = !pr->viacmd; }
  else if (!pr->viacmd && (pr->fn = getfunc(name))) pr->kind = K_FUNC;
  else if (pr->bf) pr->kind = K_BUILTIN;
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
    if (expand_error) { free(name); free(val); exp_failed(); return -1; }
    if (opt_x) outf(2, "+ %s=%s\n", name, val);
    if (!persist && old) {
      char *o = getvar(name);
      (*old)[2 * i] = strdup(name);
      (*old)[2 * i + 1] = o ? strdup(o) : NULL;
      setvar(name, val, 1);
    } else setvar(name, val, 0);
    free(name); free(val);
    if (assign_error) return -1;
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

/* a failed assignment to a read-only variable: an error that ends a
 * non-interactive shell */
static int assign_failed(void) {
  assign_error = 0;
  if (!interactive) { exit_pending = 1; exit_code = 2; }
  return 2;
}
static int run_inproc(struct prep *pr) {
  struct saved sv = {0};
  node *n = pr->n;
  int st = 0;
  if (pr->kind == K_FAIL) return exp_failed();
  int subst_before = last_status;
  if (pr->kind == K_EMPTY) {
    /* $? in the assignments is still the previous command's status; the
     * status of this command is that of its last $(...), or 0 */
    subst_ran = 0;
    if (do_assigns(n, 1, NULL) < 0) return assign_error ? assign_failed() : 2;
    st = subst_ran ? last_status : pr->subst_st >= 0 ? pr->subst_st : 0;
    if (n->redir) { if (apply_redirs(n->redir, &sv) < 0) st = expand_error ? exp_failed() : 1; restore(&sv); }
    return st;
  }
  last_status = subst_before;
  if (apply_redirs(n->redir, &sv) < 0) { restore(&sv); return expand_error ? exp_failed() : 1; }
  if (pr->kind == K_COMPOUND) st = run_compound(n);
  else {
    char **old;
    /* assignments in front of a special builtin stay; others last for the command */
    if (do_assigns(n, pr->special, &old) < 0) { restore_assigns(n, old); restore(&sv); return assign_error ? assign_failed() : 2; }
    exec_keep = 0;
    char **av = pr->argv + pr->argv0;
    int ac = pr->argc - pr->argv0;
    if (pr->kind == K_BUILTIN) {
      special_error = 0;
      cmd_nest += pr->viacmd;
      st = pr->bf(ac, av);
      cmd_nest -= pr->viacmd;
      if (special_error && pr->special && !interactive) { exit_pending = 1; exit_code = 2; }
      special_error = 0;
      if (assign_error) st = assign_failed();
    } else st = call_func(pr->fn, ac, av);
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
  if (apply_redirs(pr->n->redir, &sv) < 0) { restore(&sv); *st = expand_error ? exp_failed() : 1; return -1; }
  char **argv = pr->argv + pr->argv0;
  int argc = pr->argc - pr->argv0;
  char *path = find_in_path2(argv[0], pr->path);
  if (!path) {
    outf(2, "sh: %s: %s\n", argv[0], strchr(argv[0], '/') ? "No such file or directory" : "not found");
    *st = 127;
  } else {
    char **env = make_env(pr->n);
    fflush(stdout);
    child_sigs(1);
    pid = spawn(path, argv, env, NULL);
    if (pid < 0 && errno == ENOEXEC) { /* not a program: run it as a shell script */
      char **av = malloc((argc + 2) * sizeof *av);
      av[0] = "sh"; av[1] = path;
      memcpy(av + 2, argv + 1, argc * sizeof *av);
      pid = spawn("/bin/sh", av, env, NULL);
      free(av);
    }
    { int e = errno; child_sigs(0); errno = e; }
    if (pid < 0) {
      outf(2, "sh: %s: %s\n", argv[0], errno == 7 ? "argument list too long" : strerror(errno));
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

/* exec CMD: replace the shell with execve (221, Linux's number). A kernel
 * without it answers ENOSYS: then CMD runs as a child and the shell exits with
 * its status, which a script cannot tell apart except by the pid. The same is
 * done inside a subshell, because subshells run in this process. */
#ifndef SYS_execve
#define SYS_execve 221
#endif
int exec_cmd(int argc, char **argv) {
  int st;
  char *path = find_in_path(argv[0]);
  if (!path) { outf(2, "sh: exec: %s: not found\n", argv[0]); st = 127; goto fail; }
  char **env = build_env();
  fflush(stdout);
  /* inside a subshell that runs in this process, replacing the process would
   * end the parent shell too: run the command and leave the subshell instead */
  long r = -ENOSYS;
  child_sigs(1); /* the new program starts with SIGPIPE at its default */
  if (!cur_sub) {
    if (interactive) tty_restore();
    r = __syscall3(SYS_execve, path, argv, env);
  }
  if (r == -ENOEXEC) { /* not a program: a shell script */
    char **av = malloc((argc + 2) * sizeof *av);
    av[0] = "sh"; av[1] = path;
    memcpy(av + 2, argv + 1, argc * sizeof *av);
    r = __syscall3(SYS_execve, "/bin/sh", av, env);
    free(av);
  }
  if (r == -ENOSYS) {
    pid_t pid = spawn(path, argv, env, NULL);
    if (pid < 0 && errno == ENOEXEC) {
      char **av = malloc((argc + 2) * sizeof *av);
      av[0] = "sh"; av[1] = path;
      memcpy(av + 2, argv + 1, argc * sizeof *av);
      pid = spawn("/bin/sh", av, env, NULL);
      free(av);
    }
    child_sigs(0);
    if (pid > 0) {
      fg_begin(pid, &pid, 1);
      st = wait_pid(pid);
      fg_end();
      free(path);
      trap_forget_exit(); /* an exec'ed program runs no EXIT trap of ours */
      exit_pending = 1;
      exit_code = st;
      return st;
    }
    r = -errno;
  }
  child_sigs(0); /* exec failed: the shell goes on, with SIGPIPE ignored again */
  outf(2, "sh: exec: %s: %s\n", argv[0], strerror(-r));
  st = r == -ENOENT ? 127 : 126;
  free(path);
fail:
  if (!interactive) { exit_pending = 1; exit_code = st; }
  return st;
}

static int is_inproc(struct prep *pr) { return pr->kind != K_EXT; }

/* the text a job is listed with: its commands joined by | */
static void job_text(struct prep *pr, int n, char *buf, size_t len) {
  size_t o = 0;
  buf[0] = 0;
  for (int i = 0; i < n && o + 4 < len; i++) {
    if (i) o += snprintf(buf + o, len - o, " | ");
    if (!pr[i].argv) { o += snprintf(buf + o, len - o, "(...)"); continue; }
    for (int k = pr[i].argv0; k < pr[i].argc && o + 2 < len; k++) o += snprintf(buf + o, len - o, k > pr[i].argv0 ? " %s" : "%s", pr[i].argv[k]);
  }
  if (o >= len) buf[len - 1] = 0;
}
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
      if ((interactive && !bg) || jc()) __syscall2(SYS_setpgid, pids[i], pg);
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
    if (i < n - 1 || !LASTPIPE_IN_SHELL) { /* each stage of a pipeline is a subshell */
      struct sub sb;
      sub_begin(&sb);
      st[i] = sub_end(&sb, run_inproc(&pr[i]));
    } else st[i] = run_inproc(&pr[i]);
    restore(&sv);
    if (in[i] >= 0) close(in[i]);
    if (out[i] >= 0) close(out[i]);
    if (i > 0 && tmp[i - 1]) unlink(tmp[i - 1]);
  }
  char text[64];
  if (npids) job_text(pr, n, text, sizeof text);
  struct job *fgjob = !bg && npids && jc() && !ncaps ? job_add(pg, pids, n, text) : NULL;
  if (bg && npids) {
    struct job *jb = job_add(jc() ? pg : 0, pids, n, text);
    if (jb && interactive) outf(2, "[%d] %d\n", jb->id, pids[n - 1] > 0 ? pids[n - 1] : pg);
    for (int i = n - 1; i >= 0; i--) if (pids[i] > 0) { last_bg_pid = pids[i]; break; }
  } else if (fgjob) { /* job control: wait until it ends or stops */
    int stopped, sts[16] = {0};
    int last = job_wait_fg(fgjob, &stopped, sts);
    for (int i = 0, k = 0; i < n; i++) if (pids[i] > 0 && k < 16) st[i] = sts[k++];
    if (stopped) st[n - 1] = last; /* the job stays in the table; the list goes on, as in other shells */
    else job_remove(fgjob);
    fg_end();
  } else {
    for (int i = 0; i < n; i++) if (pids[i] > 0) st[i] = wait_pid(pids[i]);
    if (pg) fg_end();
  }
  int status = bg ? 0 : st[n - 1];
  if (opt_pipefail && !bg) for (int i = 0; i < n; i++) if (st[i]) status = st[i];
  for (int i = 0; i < n; i++) { free_argv(pr[i].argv); free(tmp[i]); }
  free(pr); free(in); free(out); free(st); free(pids); free(tmp);
  return bang ? !status : status;
}

static int loop_end(int *stop) {
  /* after a loop body: 1 = leave the loop */
  if (brk) { brk--; *stop = 1; return 1; }
  if (cont) { if (--cont) { *stop = 1; return 1; } return 0; }
  if (retflag || exit_pending || check_intr()) { *stop = 1; return 1; }
  return 0;
}

static int run_compound(node *n) {
  int st = 0, stop = 0;
  switch (n->type) {
  case N_GROUP: return run(n->a);
  case N_SUBSHELL: {
    struct sub sb;
    sub_begin(&sb);
    return sub_end(&sb, run(n->a));
  }
  case N_IF: {
    noerrexit++;
    int c = run(n->a);
    noerrexit--;
    if (STOP) return c;
    if (!c) return run(n->b);
    return n->c ? run(n->c) : 0;
  }
  case N_WHILE: case N_UNTIL:
    loop_depth++;
    for (;;) {
      noerrexit++;
      int c = run(n->a);
      noerrexit--;
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
      if (expand_error) { free_argv(words); return exp_failed(); }
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
    expand_error = 0;
    char *w = expand_str(n->name, 0);
    if (expand_error) { free(w); return exp_failed(); }
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
  if (check_intr()) return last_status = 130;
  if (!n) return last_status = 0;
  if (opt_n && !interactive) return 0; /* set -n: read commands, run none */
  switch (n->type) {
  case N_SEQ:
    run(n->a);
    if (STOP) return last_status;
    st = run(n->b);
    break;
  case N_AND: case N_OR:
    noerrexit++; /* set -e does not look at the left side of && and || */
    st = run(n->a);
    noerrexit--;
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
    noerrexit += n->bang; /* nor at a pipeline after ! */
    st = run_pipeline(n->kids, n->nkids, n->bang, 0);
    noerrexit -= n->bang;
    break;
  default:
    st = run_pipeline(&n, 1, 0, 0);
  }
  /* set -e: a failed simple command, pipeline or subshell ends the shell, unless
   * it is a condition, the left side of && or ||, or after ! (then noerrexit > 0).
   * A compound command's status is not looked at: what failed inside it was. */
  if (st && opt_e && !noerrexit && !(brk || cont || retflag || exit_pending) &&
      (n->type == N_CMD || (n->type == N_PIPE && !n->bang) || n->type == N_SUBSHELL)) {
    exit_pending = 1;
    exit_code = st;
  }
  return last_status = st;
}

static int run_text1(const char *text, const char *name);
/* A complete text (-c, a script, eval, $(...)) that ends in a backslash: a lone
 * one is taken literally, a backslash-newline joins the line to the end. */
int run_text(const char *text, const char *name) {
  size_t n = strlen(text), k = 0;
  while (k < n && text[n - 1 - k] == '\\') k++;
  int lone = k % 2 == 1;
  if (!lone && n >= 2 && text[n - 1] == '\n') {
    k = 0;
    while (k + 1 < n && text[n - 2 - k] == '\\') k++;
    if (k % 2 == 1) { /* ends in backslash-newline */
      char *t = strndup(text, n - 2);
      int st = run_text1(t, name);
      free(t); /* the parse tree keeps copies of its words, not pointers into the text */
      return st;
    }
  }
  if (!lone) return run_text1(text, name);
  char *t = malloc(n + 2);
  memcpy(t, text, n);
  t[n] = '\\';
  t[n + 1] = 0;
  int st = run_text1(t, name);
  free(t);
  return st;
}
static int run_text1(const char *text, const char *name) {
  struct parser p;
  struct arena ar = {0};
  parser_init(&p, text, &ar);
  for (;;) {
    size_t start = p.s == text ? p.pos : 0;
    node *n = parse_one(&p);
    if (opt_v && p.s == text && p.pos > start) { /* set -v: show the input as it is read */
      sh_write(2, text + start, p.pos - start);
      if (text[p.pos - 1] != '\n') outs(2, "\n");
    }
    if (p.status == P_END) break;
    if (p.status != P_OK) {
      int line = 1;
      for (size_t k = 0; k < start && text[k]; k++) line += text[k] == '\n';
      while (text[start] == '\n') start++, line++;
      if (name) outf(2, "sh: %s: line %d: %s\n", name, line, p.status == P_INCOMPLETE ? "syntax error: unexpected end of file" : p.errmsg);
      else outf(2, "sh: %s\n", p.status == P_INCOMPLETE ? "syntax error: unexpected end of file" : p.errmsg);
      last_status = 2;
      if (!interactive && !cmd_nest) { exit_pending = 1; exit_code = 2; } /* a syntax error ends a script */
      break;
    }
    run(n);
    if (STOP) break;
  }
  if (!p.keep) afree(&ar);
  return last_status;
}
