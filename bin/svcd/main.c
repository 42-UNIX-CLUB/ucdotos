// SPDX-License-Identifier: GPL-2.0-or-later
/* svcd (placeholder name): the service manager. It reads systemd unit files (the
 * subset in docs/SERVICES.md), starts and supervises services, keeps their output in a
 * bounded journal, and answers systemctl and journalctl on an AF_UNIX socket.
 *
 * It can run as pid 1 (kernel command line init=/bin/svcd) or under another init.
 * One thread, nothing blocks: every descriptor it watches is non-blocking and the
 * loop sleeps in poll(). Without SIGCHLD (the kernel on main has no signals) child
 * exits are collected with waitpid(WNOHANG) on every turn of the loop, and the loop
 * wakes at least every TICK_MS while a process of a unit is alive.
 *
 *   svcd [--unit=NAME] [--unit-path=DIR] [--guest-root=DIR] [--root=DIR] [--socket=PATH] [--quiet]
 *   (also "systemd.unit=NAME" and "unit=NAME", as kernel command line words) */
#include "core.h"
#include "journal.h"
#include "../../libsvc/os.h"
#include "../../libsvc/proto.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TICK_MS 100
#define IDLE_TICK_MS 1000
#define MAXCLIENTS 12
#define MAXSTREAMS 40
#define NAME "svcd"

static struct core C;
static char control_path[256] = SVC_CONTROL, notify_path[256] = SVC_NOTIFY, prefix[200] = "";
static int devnull = -1, quiet, is_pid1;
static int ctl_fd = -1, notify_fd = -1;

/* ---------- streams: service stdout/stderr into the journal ---------- */
struct stream { int fd, pid, console; char unit[128], ident[32]; char buf[1024]; int len; };
static struct stream streams[MAXSTREAMS];
static int pending_streams[4], npending_streams;

static void stream_line(struct stream *s, const char *l, int n) {
  int prio = 6;
  /* "<N>" prefix, as SyslogLevelPrefix=yes */
  if (n >= 3 && l[0] == '<' && l[1] >= '0' && l[1] <= '7' && l[2] == '>') { prio = l[1] - '0'; l += 3; n -= 3; }
  journal_add(svc_epoch_ms(), prio, s->pid, s->unit, s->ident, l, n);
  if (s->console) { if (write(1, l, n) < 0 || write(1, "\n", 1) < 0) {} }
}
static void stream_read(struct stream *s) {
  for (;;) {
    int room = (int)sizeof s->buf - s->len;
    ssize_t r = read(s->fd, s->buf + s->len, room);
    if (r < 0 && errno == EAGAIN) return;
    if (r <= 0) {
      if (s->len) stream_line(s, s->buf, s->len);
      close(s->fd);
      s->fd = -1;
      return;
    }
    s->len += r;
    int start = 0;
    for (int i = 0; i < s->len; i++)
      if (s->buf[i] == '\n') { stream_line(s, s->buf + start, i - start); start = i + 1; }
    if (start == 0 && s->len == (int)sizeof s->buf) { stream_line(s, s->buf, s->len); s->len = 0; continue; } /* overlong line */
    memmove(s->buf, s->buf + start, s->len - start);
    s->len -= start;
    if (r < room) return;
  }
}

/* ---------- the process layer for the core ---------- */
static int64_t o_now(void) { return svc_now_ms(); }
static int64_t o_epoch(void) { return svc_epoch_ms(); }
static int o_spawn(struct unit *u, const char *path, char *const argv[], char *const envp[], int in, int out, int err, int pgid,
                   const int *extra, int nextra) {
  int fdmap[3] = {in >= 0 ? in : in == -2 ? 0 : devnull, out >= 0 ? out : devnull, err >= 0 ? err : devnull};
  char cwd[256] = "";
  if (u->workdir[0]) {
    const char *w = u->workdir + (u->workdir[0] == '-');
    char h[300];
    if (!strcmp(w, "~")) w = "/home/user";
    svc_host_path(&C.roots, u->root, w, h, sizeof h);
    if (chdir(h) < 0 && u->workdir[0] != '-') return -errno;
    snprintf(cwd, sizeof cwd, "%s", h);
  }
  int pid = svc_spawn(path, argv, envp, fdmap, extra, nextra, pgid);
  if (cwd[0] && chdir("/") < 0) {}
  return pid;
}
static int o_kill(int pid, int sig) { return kill(pid, sig) < 0 ? -errno : 0; }
static struct svc_pinfo ptab[128];
static int nptab;
static int64_t ptab_at = -1;
static void refresh_procs(void) {
  int64_t now = svc_now_ms();
  if (now == ptab_at) return;
  nptab = svc_procs(ptab, 128);
  ptab_at = now;
}
/* A unit's processes: its process group, plus anything descended from a member (a
 * child that called setsid() leaves the group but keeps its parent). */
static int o_members(int pgid, int main_pid, int *pids, int max) {
  ptab_at = -1;
  refresh_procs();
  int n = 0;
  for (int i = 0; i < nptab && n < max; i++)
    if (ptab[i].state != 2 && ((pgid > 0 && ptab[i].pgid == pgid) || (main_pid > 0 && ptab[i].pid == main_pid))) pids[n++] = ptab[i].pid;
  for (int grew = 1; grew && n < max;) {
    grew = 0;
    for (int i = 0; i < nptab && n < max; i++) {
      if (ptab[i].state == 2) continue;
      int in = 0, parent = 0;
      for (int k = 0; k < n; k++) { if (pids[k] == ptab[i].pid) in = 1; if (pids[k] == ptab[i].ppid) parent = 1; }
      if (!in && parent) { pids[n++] = ptab[i].pid; grew = 1; }
    }
  }
  return n;
}
static uint32_t o_pages(int pid) {
  refresh_procs();
  for (int i = 0; i < nptab; i++) if (ptab[i].pid == pid) return ptab[i].pages;
  return 0;
}
static void console_status(const char *unit, int prio, const char *msg) {
  if (quiet) return;
  const char *tag = 0;
  if (prio <= 3) tag = "[FAILED] ";
  else if (!strncmp(msg, "Started ", 8) || !strncmp(msg, "Reached target ", 15) || !strncmp(msg, "Listening on ", 13) ||
           !strncmp(msg, "Finished ", 9) || !strncmp(msg, "Stopped ", 8)) tag = "[  OK  ] ";
  else if (!strncmp(msg, "Startup finished", 16)) tag = "";
  else if (prio <= 4) tag = "[ WARN ] ";
  if (!tag) return;
  char b[600];
  int n = snprintf(b, sizeof b, "%s%s\n", tag, msg);
  if (write(1, b, n > (int)sizeof b ? (int)sizeof b : n) < 0) {}
}
static void o_log(const char *unit, int prio, const char *msg) {
  journal_add(svc_epoch_ms(), prio, getpid(), unit ? unit : "", NAME, msg, (int)strlen(msg));
  console_status(unit, prio, msg);
}
static int o_open_output(struct unit *u, int which, int *keep) {
  int o = which == 2 && u->err >= 0 ? u->err : u->out;
  const char *path = which == 2 && u->err >= 0 ? u->errpath : u->outpath;
  *keep = 0;
  switch (o) {
  case OUT_NULL: return -1;
  case OUT_INHERIT: *keep = 1; return which == 2 ? 2 : 1;
  case OUT_FILE: case OUT_APPEND: {
    char h[300];
    svc_host_path(&C.roots, u->root, path, h, sizeof h);
    int fd = open(h, O_WRONLY | O_CREAT | O_CLOEXEC | (o == OUT_APPEND ? O_APPEND : O_TRUNC), 0644);
    return fd;
  }
  default: {
    int slot = -1;
    for (int i = 0; i < MAXSTREAMS; i++) if (streams[i].fd < 0) { slot = i; break; }
    if (slot < 0) return -1; /* out of streams: /dev/null */
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    svc_set_nonblock(p[0], 1);
    struct stream *s = &streams[slot];
    memset(s, 0, sizeof *s);
    s->fd = p[0];
    s->console = o == OUT_CONSOLE_TOO;
    snprintf(s->unit, sizeof s->unit, "%s", u->name);
    if (npending_streams < 4) pending_streams[npending_streams++] = slot;
    return p[1];
  }
  }
}
static void o_spawned(struct unit *u, int pid, const char *ident) {
  for (int i = 0; i < npending_streams; i++) {
    struct stream *s = &streams[pending_streams[i]];
    s->pid = pid;
    snprintf(s->ident, sizeof s->ident, "%s", ident ? ident : u->name);
  }
  npending_streams = 0;
}
static int o_listen(const char *path) {
  char d[256];
  snprintf(d, sizeof d, "%s", path);
  char *sl = strrchr(d, '/');
  if (sl && sl != d) { *sl = 0; svc_mkdir_p(d); }
  return svc_listen_unix(path, 1);
}
static int o_accept(int lfd) { return svc_accept(lfd, 0); }
static void o_closefd(int fd) { if (fd > 2) close(fd); }
static int o_exists(const char *p, int what) {
  struct stat st;
  if (what == 5) return svc_is_symlink(p) ? 0 : -1;
  if (stat(p, &st) < 0) return -1;
  switch (what) {
  case 1: return S_ISDIR(st.st_mode) ? 0 : -1;
  case 2: return S_ISREG(st.st_mode) ? 0 : -1; /* no permission bits on main: a regular file counts */
  case 3: {
    DIR *d = opendir(p);
    if (!d) return -1;
    struct dirent *de;
    int any = 0;
    while ((de = readdir(d))) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { any = 1; break; }
    closedir(d);
    return any ? 0 : -1;
  }
  case 4: return S_ISREG(st.st_mode) && st.st_size > 0 ? 0 : -1;
  default: return 0;
  }
}
static int64_t o_mtime(const char *p) { struct stat st; return stat(p, &st) < 0 ? -1 : (int64_t)st.st_mtime * 1000 + (int64_t)st.st_size % 1000; }
static void o_mkdirs(const char *p) { svc_mkdir_p(p); }
static void rmtree(const char *p, int depth) {
  if (depth > 8) return;
  DIR *d = opendir(p);
  if (d) {
    struct dirent *de;
    char names[64][256];
    int n = 0;
    while ((de = readdir(d)) && n < 64) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) snprintf(names[n++], 256, "%s", de->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) {
      char c[600];
      snprintf(c, sizeof c, "%s/%s", p, names[i]);
      struct stat st;
      if (!svc_is_symlink(c) && stat(c, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(c, depth + 1);
      else unlink(c);
    }
    rmdir(p);
  }
}
static void o_rmtree(const char *p) { if (!strncmp(p, "/run/", 5) || strstr(p, "/run/")) rmtree(p, 0); }
static int o_read_file(const char *p, char *buf, int len) {
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  int n = 0;
  while (n < len) { ssize_t r = read(fd, buf + n, len - n); if (r <= 0) break; n += r; }
  close(fd);
  return n;
}
static uint32_t o_random(void) {
  static int fd = -2;
  uint32_t v = 0;
  if (fd == -2) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0 || read(fd, &v, 4) != 4) v = (uint32_t)svc_now_ms() * 2654435761u;
  return v;
}
static struct svc_ops ops = {o_now, o_epoch, o_spawn, o_kill, o_members, o_pages, o_log, o_open_output, o_spawned,
                             o_listen, o_accept, o_closefd, o_exists, o_mtime, o_mkdirs, o_rmtree, o_read_file, o_random};

/* ---------- clients ---------- */
struct client {
  int fd, gen, waiting, rc, follow, closing, dumping, skip;
  char in[1024]; int inlen;
  char *out; int outlen, outcap;
  char filter[8][128]; int nfilter; uint32_t seen;
};
static struct client clients[MAXCLIENTS];

static void cl_close(struct client *cl) {
  if (cl->fd >= 0) close(cl->fd);
  cl->fd = -1;
  free(cl->out);
  cl->out = 0; cl->outlen = cl->outcap = 0;
  cl->gen++;
  cl->waiting = 0; cl->follow = 0; cl->closing = 0; cl->inlen = 0; cl->dumping = 0;
}
static void cl_flush(struct client *cl) {
  while (cl->outlen > 0) {
    ssize_t w = write(cl->fd, cl->out, cl->outlen);
    if (w < 0 && errno == EAGAIN) return;
    if (w <= 0) { cl_close(cl); return; }
    memmove(cl->out, cl->out + w, cl->outlen - w);
    cl->outlen -= w;
  }
  if (cl->outcap > 65536) { free(cl->out); cl->out = 0; cl->outcap = 0; } /* give big buffers back */
  if (cl->closing && !cl->outlen) cl_close(cl);
}
static void cl_put(struct client *cl, const char *s, int n) {
  if (cl->fd < 0) return;
  if (cl->outlen + n > 4 << 20) { cl_close(cl); return; } /* a client that does not read */
  if (cl->outlen + n + 1 > cl->outcap) {
    int nc = cl->outcap ? cl->outcap : 4096;
    while (cl->outlen + n + 1 > nc) nc *= 2;
    char *no = realloc(cl->out, nc);
    if (!no) { cl_close(cl); return; }
    cl->out = no; cl->outcap = nc;
  }
  memcpy(cl->out + cl->outlen, s, n);
  cl->outlen += n;
}
static void cl_printf(struct client *cl, const char *fmt, ...) {
  char b[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = sizeof b - 1;
  cl_put(cl, b, n);
}
static void cl_end(struct client *cl, int rc) {
  cl_printf(cl, "=rc %d\n", rc);
  cl->closing = 1;
  cl_flush(cl);
}

static const char *fail_reason(const char *result) {
  if (!strcmp(result, "exit-code")) return "the control process exited with error code";
  if (!strcmp(result, "timeout")) return "a timeout was exceeded";
  if (!strcmp(result, "start-limit-hit")) return "start of the service was attempted too often";
  if (!strcmp(result, "signal")) return "a fatal signal was delivered to the control process";
  if (!strcmp(result, "core-dump")) return "a fatal signal was delivered causing the control process to dump core";
  if (!strcmp(result, "resources")) return "of unavailable resources or another system error";
  if (!strcmp(result, "oom-kill")) return "the control process was killed by the memory limit";
  if (!strcmp(result, "protocol")) return "the service did not take the steps required by its unit configuration";
  return "the control process exited with error code";
}
static void job_done(struct core *c, int client, const char *unit, int type, const char *result) {
  if (client < 0) return;
  int slot = client & 0xff, gen = client >> 8;
  if (slot >= MAXCLIENTS) return;
  struct client *cl = &clients[slot];
  if (cl->fd < 0 || cl->gen != gen || !cl->waiting) return;
  if (!strcmp(result, "failed")) {
    int i = core_find(c, unit);
    const char *res = i >= 0 ? c->u[i]->result : "exit-code";
    if (type == 3) cl_printf(cl, "!Job for %s failed.\n", unit);
    else cl_printf(cl, "!Job for %s failed because %s.\n!See \"systemctl status %s\" and \"journalctl -xeu %s\" for details.\n", unit, fail_reason(res), unit, unit);
    cl->rc = 1;
  } else if (!strcmp(result, "dependency")) { cl_printf(cl, "!A dependency job for %s failed. See 'journalctl -xe' for details.\n", unit); cl->rc = 1; }
  else if (!strcmp(result, "canceled")) { cl_printf(cl, "!Job for %s canceled.\n", unit); cl->rc = 1; }
  else if (!strcmp(result, "timeout")) { cl_printf(cl, "!Job for %s timed out.\n", unit); cl->rc = 1; }
  if (--cl->waiting == 0) cl_end(cl, cl->rc);
}

struct jctx { struct client *cl; int n, skip; };
#define DUMP_CHUNK 16384 /* a journal dump is sent this much at a time, so the manager never holds it all */
static int unit_match(struct client *cl, const struct jrec *r) {
  if (!cl->nfilter) return 1;
  for (int i = 0; i < cl->nfilter; i++)
    if ((int)strlen(cl->filter[i]) == r->ulen && !memcmp(cl->filter[i], r->unit, r->ulen)) return 1;
  return 0;
}
static int j_count(void *ctx, const struct jrec *r) { struct jctx *j = ctx; if (unit_match(j->cl, r)) j->n++; return 0; }
static int j_emit(void *ctx, const struct jrec *r) {
  struct jctx *j = ctx;
  j->cl->seen = r->seq;
  if (!unit_match(j->cl, r)) return 0;
  if (j->cl->skip > 0) { j->cl->skip--; return 0; }
  char head[400];
  int n = snprintf(head, sizeof head, "%u\t%lld\t%d\t%d\t%.*s\t%.*s\t", r->seq, (long long)r->t_ms, r->prio, r->pid, r->ulen, r->unit, r->ilen, r->ident);
  cl_put(j->cl, head, n);
  cl_put(j->cl, r->text, r->tlen);
  cl_put(j->cl, "\n", 1);
  return j->cl->fd < 0 || j->cl->outlen >= DUMP_CHUNK;
}
/* Send the next part of a journal dump; finish it when the end is reached. */
static void dump_more(struct client *cl) {
  while (cl->fd >= 0 && cl->dumping && cl->outlen < DUMP_CHUNK) {
    struct jctx j = {cl, 0, 0};
    uint32_t before = cl->seen;
    journal_walk(cl->seen, j_emit, &j);
    if (cl->seen == before || cl->seen >= journal_last_seq()) {
      cl->dumping = 0;
      if (!cl->follow) { cl_end(cl, 0); return; }
    }
  }
  if (cl->fd >= 0) cl_flush(cl);
}

static void emit_line(void *ctx, const char *line) { struct client *cl = ctx; cl_printf(cl, "%s\n", line); }

static void handle(struct client *cl, int slot, char *line) {
  char *w[64];
  int n = 0;
  for (char *p = strtok(line, " "); p && n < 64; p = strtok(0, " ")) w[n++] = p;
  if (!n) { cl_end(cl, 1); return; }
  const char *v = w[0];
  static const struct { const char *n; int t; } jobs[] = {
    {"start", J_START}, {"stop", J_STOP}, {"restart", J_RESTART}, {"reload", J_RELOAD}, {"try-restart", J_TRYRESTART},
    {"condrestart", J_TRYRESTART}, {"reload-or-restart", -1}, {"try-reload-or-restart", -2}, {"force-reload", -2}, {"isolate", J_START}, {0, 0}};
  for (int k = 0; jobs[k].n; k++) {
    if (strcmp(v, jobs[k].n)) continue;
    int noblock = 0;
    const char *mode = !strcmp(v, "isolate") ? "isolate" : "replace";
    cl->rc = 0;
    cl->waiting = 1; /* held until all requests are queued */
    for (int i = 1; i < n; i++) {
      if (!strcmp(w[i], "--no-block")) { noblock = 1; continue; }
      if (!strncmp(w[i], "--job-mode=", 11)) { mode = w[i] + 11; continue; }
      if (w[i][0] == '-') continue;
      int type = jobs[k].t;
      int ui = core_unit(&C, w[i]);
      struct unit *u = ui >= 0 ? C.u[ui] : 0;
      if (type < 0) { /* reload-or-restart and friends: reload if the unit can, else restart */
        int can = u && u->type == SVC_T_SERVICE && u->exec[X_RELOAD].n;
        if (type == -2 && u && u->active != A_ACTIVE) { continue; }
        type = can && u->active == A_ACTIVE ? J_RELOAD : (type == -2 ? J_TRYRESTART : J_RESTART);
      }
      if (u && type == J_START && u->refuse_manual_start) { cl_printf(cl, "!Operation refused, unit %s may be requested by dependency only (it is configured to refuse manual start/stop).\n", w[i]); cl->rc = 1; continue; }
      char msg[400];
      int r = core_request(&C, w[i], type, mode, noblock ? -1 : ((cl->gen << 8) | slot), msg, sizeof msg);
      if (r < 0) {
        static const char *const verbs[] = {"start", "stop", "restart", "reload", "try-restart", "verify"};
        cl_printf(cl, "!Failed to %s %s: %s\n", !strcmp(v, "isolate") ? "isolate" : verbs[type], w[i], msg);
        cl->rc = r == -ENOENT ? 5 : 1;
        if (r == -ENOENT && !strcmp(v, "stop")) cl->rc = 5;
        continue;
      }
      if (!noblock) cl->waiting++;
    }
    core_run(&C);
    if (--cl->waiting == 0) cl_end(cl, cl->rc);
    return;
  }
  if (!strcmp(v, "show")) {
    for (int i = 1; i < n; i++) {
      if (w[i][0] == '-') continue;
      int ui = core_unit(&C, w[i]);
      if (ui < 0) { cl_printf(cl, "!Invalid unit name \"%s\".\n", w[i]); continue; }
      core_show(&C, C.u[ui], emit_line, cl);
      cl_printf(cl, "\n");
    }
    cl_end(cl, 0);
    return;
  }
  if (!strcmp(v, "list-units") || !strcmp(v, "list")) {
    int all = 0;
    for (int i = 1; i < n; i++) if (!strcmp(w[i], "--all") || !strcmp(w[i], "-a")) all = 1;
    for (int i = 0; i < C.nu; i++) {
      struct unit *u = C.u[i];
      if (!u) continue;
      if (!all && u->active == A_INACTIVE && u->job < 0 && u->load == L_LOADED) continue;
      if (!all && u->load != L_LOADED && u->active == A_INACTIVE) continue;
      cl_printf(cl, "%s\t%s\t%s\t%s\t%s\n", u->name, core_load_str(u->load), core_active_str(u->active), core_sub_str(u), u->desc);
    }
    cl_end(cl, 0);
    return;
  }
  if (!strcmp(v, "list-jobs")) {
    static const char *const jt[] = {"start", "stop", "restart", "reload", "try-restart", "verify-active"};
    for (int j = 0; j < MAXJOBS; j++)
      if (C.jobs[j].used) cl_printf(cl, "%d\t%s\t%s\t%s\n", j, C.u[C.jobs[j].unit]->name, jt[C.jobs[j].type], C.jobs[j].running ? "running" : "waiting");
    cl_end(cl, 0);
    return;
  }
  if (!strcmp(v, "journal")) {
    int lines = -1;
    cl->nfilter = 0;
    int follow = 0;
    for (int i = 1; i < n; i++) {
      if (!strcmp(w[i], "-u") && i + 1 < n && cl->nfilter < 8) { snprintf(cl->filter[cl->nfilter++], 128, "%s", w[++i]); continue; }
      if (!strcmp(w[i], "-n") && i + 1 < n) { lines = atoi(w[++i]); continue; }
      if (!strcmp(w[i], "-f")) follow = 1;
    }
    struct jctx j = {cl, 0, 0};
    cl->skip = 0;
    if (lines >= 0) { journal_walk(0, j_count, &j); cl->skip = j.n > lines ? j.n - lines : 0; }
    cl->seen = 0;
    cl->follow = follow;
    cl->dumping = 1;
    dump_more(cl);
    return;
  }
  if (!strcmp(v, "daemon-reload") || !strcmp(v, "daemon-reexec")) {
    int64_t t = svc_now_ms();
    core_reload(&C);
    core_log(&C, 0, 7, "Reloaded %d units in %lld ms.", C.nu, (long long)(svc_now_ms() - t));
    cl_end(cl, 0);
    return;
  }
  if (!strcmp(v, "load-all")) { /* a measurement hook: load every unit file there is */
    int64_t t = svc_now_ms();
    int before = C.nodes_loaded, files = 0;
    for (int ri = 0; ri < C.roots.n; ri++) {
      char d[300];
      for (int k = 0; svc_unit_dir(&C.roots, ri, k, d, sizeof d); k++) {
        DIR *dd = opendir(d);
        struct dirent *de;
        while (dd && (de = readdir(dd))) {
          if (!svc_name_valid(de->d_name) || svc_name_split(de->d_name, 0, 0, 0, 0) == 2) continue;
          files++;
          core_unit(&C, de->d_name);
        }
        if (dd) closedir(dd);
      }
    }
    int64_t ms = svc_now_ms() - t;
    int units = 0;
    for (int i = 0; i < C.nu; i++) if (C.u[i]) units++;
    cl_printf(cl, "Files=%d\nNewlyLoaded=%d\nUnits=%d\nMs=%lld\n", files, C.nodes_loaded - before, units, (long long)ms);
    t = svc_now_ms();
    core_reload(&C);
    cl_printf(cl, "ReloadMs=%lld\n", (long long)(svc_now_ms() - t));
    cl_end(cl, 0);
    return;
  }
  if (!strcmp(v, "reset-failed")) { core_reset_failed(&C, n > 1 ? w[1] : 0); cl_end(cl, 0); return; }
  if (!strcmp(v, "kill") && n >= 2) {
    int sig = 15;
    const char *who = "all";
    for (int i = 2; i < n; i++) {
      if (!strncmp(w[i], "--signal=", 9)) sig = atoi(w[i] + 9);
      if (!strncmp(w[i], "--kill-whom=", 12) || !strncmp(w[i], "--kill-who=", 11)) who = strchr(w[i], '=') + 1;
    }
    int r = core_kill(&C, w[1], sig, who);
    if (r < 0) cl_printf(cl, "!Failed to kill unit %s: %s\n", w[1], r == -ENOENT ? "Unit not loaded." : "No main process to kill");
    cl_end(cl, r < 0 ? 1 : 0);
    return;
  }
  if (!strcmp(v, "poweroff") || !strcmp(v, "halt") || !strcmp(v, "reboot") || !strcmp(v, "soft-reboot") || !strcmp(v, "exit")) {
    cl_end(cl, 0);
    core_poweroff(&C, !strcmp(v, "reboot") || !strcmp(v, "soft-reboot"));
    return;
  }
  if (!strcmp(v, "status-manager") || !strcmp(v, "ping")) {
    struct svc_pinfo me[128];
    int np = svc_procs(me, 128);
    uint32_t pages = 0;
    for (int i = 0; i < np; i++) if (me[i].pid == getpid()) pages = me[i].pages;
    uint32_t b, used, recs, dropped;
    journal_stats(&b, &used, &recs, &dropped);
    int units = 0, jobsn = 0;
    for (int i = 0; i < C.nu; i++) if (C.u[i]) units++;
    for (int j = 0; j < MAXJOBS; j++) if (C.jobs[j].used) jobsn++;
    cl_printf(cl, "Pid=%d\nMemoryPages=%u\nJournalBytes=%u\nJournalUsed=%u\nJournalRecords=%u\nJournalDropped=%u\nUnits=%d\nJobs=%d\nSignals=%s\nSymlinks=%s\nGuestRoot=%s\nDefaultTarget=%s\nUptimeMs=%lld\n",
              getpid(), pages, b, used, recs, dropped, units, jobsn, C.have_signals ? "yes" : "no", svc_have_symlinks() ? "yes" : "no",
              C.roots.n > 1 ? C.roots.root[1] : "", C.default_target, (long long)(svc_now_ms() - C.boot_ms));
    cl_end(cl, 0);
    return;
  }
  cl_printf(cl, "!Unknown command verb '%s'.\n", v);
  cl_end(cl, 1);
}

static void cl_read(struct client *cl, int slot) {
  ssize_t r = read(cl->fd, cl->in + cl->inlen, sizeof cl->in - 1 - cl->inlen);
  if (r < 0 && errno == EAGAIN) return;
  if (r <= 0) { cl_close(cl); return; }
  cl->inlen += r;
  cl->in[cl->inlen] = 0;
  char *nl = strchr(cl->in, '\n');
  if (!nl) { if (cl->inlen >= (int)sizeof cl->in - 1) cl_close(cl); return; }
  *nl = 0;
  if (cl->waiting || cl->follow || cl->closing || cl->dumping) return; /* one request per connection */
  handle(cl, slot, cl->in);
}

/* Notification connections stay in the poll set until the sender closes them: the
 * message may arrive after the connection. */
struct nconn { int fd, n; char b[1024]; };
static struct nconn nconns[8];
static void notify_read(struct nconn *nc) {
  for (;;) {
    ssize_t r = read(nc->fd, nc->b + nc->n, sizeof nc->b - 1 - nc->n);
    if (r > 0) { nc->n += r; if (nc->n < (int)sizeof nc->b - 1) continue; }
    if (r < 0 && errno == EAGAIN) return;
    break; /* end of message (or full) */
  }
  close(nc->fd);
  nc->fd = -1;
  nc->b[nc->n] = 0;
  /* the sender says who it is (no SO_PEERCRED here): "SENDER_PID=N" */
  int pid = 0, ppid = 0;
  char *p = strstr(nc->b, "SENDER_PID=");
  if (p) pid = atoi(p + 11);
  /* a short-lived sender (systemd-notify) may be gone before this is read: its parent
   * (the service's main process, usually) is given too */
  p = strstr(nc->b, "SENDER_PPID=");
  if (p) ppid = atoi(p + 12);
  if (pid > 0 && core_notify(&C, pid, nc->b) < 0 && ppid > 0) core_notify(&C, ppid, nc->b);
}

/* ---------- configuration ---------- */
static void read_config(uint32_t *jbytes) {
  struct svc_file f = {0};
  char p[300];
  snprintf(p, sizeof p, "%s/etc/systemd/system.conf", prefix);
  if (svc_parse_path(&f, p, 0, 0) >= 0) {
    int64_t ms;
    const char *v;
    if ((v = svc_get(&f, "Manager", "DefaultTimeoutStartSec")) && !svc_timespan(v, &ms, 1000)) C.def_tstart = ms;
    if ((v = svc_get(&f, "Manager", "DefaultTimeoutStopSec")) && !svc_timespan(v, &ms, 1000)) C.def_tstop = ms;
    if ((v = svc_get(&f, "Manager", "DefaultRestartSec")) && !svc_timespan(v, &ms, 1000)) C.def_restart = ms;
    if ((v = svc_get(&f, "Manager", "DefaultStartLimitIntervalSec")) && !svc_timespan(v, &ms, 1000)) C.def_limit_interval = ms;
    if ((v = svc_get(&f, "Manager", "DefaultStartLimitBurst"))) C.def_limit_burst = atoi(v);
    if ((v = svc_get(&f, "Manager", "GuestEngine"))) snprintf(C.engine, sizeof C.engine, "%s", v);
  }
  svc_file_free(&f);
  snprintf(p, sizeof p, "%s/etc/systemd/journald.conf", prefix);
  if (svc_parse_path(&f, p, 0, 0) >= 0) {
    const char *v = svc_get(&f, "Journal", "RuntimeMaxUse");
    if (v) {
      char *e;
      double d = strtod(v, &e);
      if (*e == 'K') d *= 1024; else if (*e == 'M') d *= 1024 * 1024; else if (*e == 'G') d *= 1024.0 * 1024 * 1024;
      if (d >= 4096 && d <= 64.0 * 1024 * 1024) *jbytes = (uint32_t)d;
    }
  }
  svc_file_free(&f);
}
static const char *guest_root_conf(void) {
  static char v[128];
  struct svc_file f = {0};
  char p[300];
  snprintf(p, sizeof p, "%s/etc/systemd/system.conf", prefix);
  const char *r = 0;
  if (svc_parse_path(&f, p, 0, 0) >= 0 && svc_get(&f, "Manager", "GuestRoot")) { snprintf(v, sizeof v, "%s", svc_get(&f, "Manager", "GuestRoot")); r = v; }
  svc_file_free(&f);
  return r;
}

int main(int argc, char **argv) {
  int64_t t_main = svc_now_ms();
  svc_reserve_fds(4); /* LISTEN_FDS land on 3..6 in a child; keep those numbers free (os.c) */
  is_pid1 = getpid() == 1;
  const char *unit = 0, *groot = 0, *unit_path = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strncmp(a, "--unit=", 7)) unit = a + 7;
    else if (!strncmp(a, "systemd.unit=", 13)) unit = a + 13;
    else if (!strncmp(a, "unit=", 5)) unit = a + 5;
    else if (!strncmp(a, "--guest-root=", 13)) groot = a + 13;
    else if (!strncmp(a, "--root=", 7)) snprintf(prefix, sizeof prefix, "%s", a + 7);
    else if (!strncmp(a, "--socket=", 9)) snprintf(control_path, sizeof control_path, "%s", a + 9);
    else if (!strncmp(a, "--unit-path=", 12)) unit_path = a + 12;
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "--system") || !strcmp(a, "--user")) {}
    else { fprintf(stderr, NAME ": unknown option %s\n", a); }
  }
  if (getenv("SVCD_SOCKET")) snprintf(control_path, sizeof control_path, "%s", getenv("SVCD_SOCKET"));
  if (prefix[0]) snprintf(notify_path, sizeof notify_path, "%s%s", prefix, SVC_NOTIFY);
  if (is_pid1 && chdir("/") < 0) {}
  devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
  for (int i = 0; i < MAXSTREAMS; i++) streams[i].fd = -1;
  for (int i = 0; i < MAXCLIENTS; i++) clients[i].fd = -1;
  for (int i = 0; i < 8; i++) nconns[i].fd = -1;
  uint32_t jbytes = 256 * 1024;
  if (!groot) groot = guest_root_conf();
  core_init(&C, &ops, groot ? groot : "/debian");
  if (prefix[0]) snprintf(C.roots.root[0], sizeof C.roots.root[0], "%s", prefix);
  if (unit_path) snprintf(C.roots.extra, sizeof C.roots.extra, "%s", unit_path);
  read_config(&jbytes);
  journal_init(jbytes);
  C.job_done = job_done;
  C.have_signals = svc_have_signals();
  char p[300];
  /* "systemd is running", as Debian's maintainer scripts test it; in the guest root too */
  snprintf(p, sizeof p, "%s%s", prefix, SVC_RUNNING);
  svc_mkdir_p(p);
  if (C.roots.n > 1) { snprintf(p, sizeof p, "%s%s", C.roots.root[1], SVC_RUNNING); svc_mkdir_p(p); }
  snprintf(p, sizeof p, "%s", control_path);
  char *sl = strrchr(p, '/');
  if (sl) { *sl = 0; svc_mkdir_p(p); }
  ctl_fd = svc_listen_unix(control_path, 1);
  if (ctl_fd < 0) fprintf(stderr, NAME ": cannot listen on %s: %s\n", control_path, strerror(-ctl_fd));
  snprintf(p, sizeof p, "%s", notify_path);
  sl = strrchr(p, '/');
  if (sl) { *sl = 0; svc_mkdir_p(p); }
  notify_fd = svc_listen_unix(notify_path, 1);
  if (unit) snprintf(C.default_target, sizeof C.default_target, "%s", unit);
  core_log(&C, 0, 6, NAME " running as pid %d%s; guest root %s; signals %s; symlinks %s.", getpid(), is_pid1 ? " (init)" : "",
           C.roots.n > 1 ? C.roots.root[1] : "none", C.have_signals ? "delivered" : "not delivered (kill ends a process)",
           svc_have_symlinks() ? "yes" : "no (link files)");
  int64_t t0 = svc_now_ms();
  char msg[300];
  int di = core_unit(&C, C.default_target);
  if (di >= 0) snprintf(C.default_target, sizeof C.default_target, "%s", C.u[di]->name);
  core_log(&C, 0, 6, "Loaded %d units in %lld ms.", C.nodes_loaded, (long long)(svc_now_ms() - t0));
  if (core_request(&C, C.default_target, J_START, "replace", -1, msg, sizeof msg) < 0)
    core_log(&C, 0, 3, "Failed to start default target %s: %s", C.default_target, msg);
  int64_t next_measure = svc_now_ms() + 5000;
  int started = 0;
  for (;;) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) core_child_exit(&C, pid, st);
    core_run(&C);
    if (C.quit) break;
    if (!started && di >= 0 && C.u[di] && C.u[di]->active == A_ACTIVE) {
      started = 1;
      core_log(&C, 0, 6, "Startup finished in %lld ms (from the manager's first instruction to %s).", (long long)(svc_now_ms() - t_main), C.default_target);
    }
    int64_t now = svc_now_ms();
    if (now >= next_measure) { core_measure(&C); next_measure = now + 5000; }
    /* new journal records for followers */
    for (int i = 0; i < MAXCLIENTS; i++) {
      struct client *cl = &clients[i];
      if (cl->fd < 0 || cl->outlen >= DUMP_CHUNK) continue;
      if (cl->dumping) { dump_more(cl); continue; }
      if (!cl->follow || cl->seen == journal_last_seq()) continue;
      cl->dumping = 1; /* new records for a follower: sent the same way */
      dump_more(cl);
    }
    struct pollfd pf[64];
    int what[64], idx[64], np = 0;
#define ADD(fd_, ev_, w_, i_) do { if (np < 64) { pf[np].fd = (fd_); pf[np].events = (ev_); pf[np].revents = 0; what[np] = (w_); idx[np] = (i_); np++; } } while (0)
    if (ctl_fd >= 0) ADD(ctl_fd, POLLIN, 0, 0);
    if (notify_fd >= 0) ADD(notify_fd, POLLIN, 1, 0);
    for (int i = 0; i < MAXCLIENTS; i++) if (clients[i].fd >= 0) ADD(clients[i].fd, POLLIN | (clients[i].outlen ? POLLOUT : 0), 2, i);
    for (int i = 0; i < MAXSTREAMS; i++) if (streams[i].fd >= 0) ADD(streams[i].fd, POLLIN, 3, i);
    for (int i = 0; i < 8; i++) if (nconns[i].fd >= 0) ADD(nconns[i].fd, POLLIN, 5, i);
    for (int i = 0; i < C.nu; i++) {
      struct unit *u = C.u[i];
      if (!u || u->type != SVC_T_SOCKET || !u->listening) continue;
      for (int k = 0; k < u->nlisten; k++) if (u->lfd[k] >= 0) ADD(u->lfd[k], POLLIN, 4, u->lfd[k]);
    }
    int64_t wake = core_next_wakeup(&C);
    /* Without SIGCHLD, exits are found by polling: every TICK_MS while something is
     * changing (jobs, units starting or stopping), every IDLE_TICK_MS while processes
     * merely run (or, as pid 1, orphans may need reaping). */
    int busy = 0, running = 0;
    for (int i = 0; i < C.nu; i++) {
      struct unit *u = C.u[i];
      if (!u) continue;
      if (u->main_pid > 0 || u->ctl_pid > 0) running = 1;
      if (u->active == A_ACTIVATING || u->active == A_DEACTIVATING || u->active == A_RELOADING) busy = 1;
    }
    for (int j = 0; j < MAXJOBS; j++) if (C.jobs[j].used) busy = 1;
    int timeout = wake < 0 ? -1 : wake > 3600000 ? 3600000 : (int)wake;
    int tick = busy ? TICK_MS : (running || is_pid1) ? IDLE_TICK_MS : -1;
    if (!C.have_signals && tick > 0 && (timeout < 0 || timeout > tick)) timeout = tick;
    if (timeout < 0 || timeout > 5000) timeout = 5000; /* memory measurement */
    int r = poll(pf, np, timeout);
    if (r <= 0) continue;
    for (int i = 0; i < np; i++) {
      if (!pf[i].revents) continue;
      switch (what[i]) {
      case 0: {
        int fd = svc_accept(ctl_fd, 1);
        if (fd < 0) break;
        int slot = -1;
        for (int k = 0; k < MAXCLIENTS; k++) if (clients[k].fd < 0) { slot = k; break; }
        if (slot < 0) { close(fd); break; }
        clients[slot].fd = fd;
        clients[slot].inlen = 0; clients[slot].waiting = 0; clients[slot].follow = 0; clients[slot].closing = 0;
        break;
      }
      case 1: {
        int fd = svc_accept(notify_fd, 1);
        if (fd < 0) break;
        int k = 0;
        while (k < 8 && nconns[k].fd >= 0) k++;
        if (k == 8) { close(fd); break; }
        nconns[k].fd = fd; nconns[k].n = 0;
        notify_read(&nconns[k]);
        break;
      }
      case 5: if (nconns[idx[i]].fd >= 0) notify_read(&nconns[idx[i]]); break;
      case 2: {
        struct client *cl = &clients[idx[i]];
        if (cl->fd < 0) break;
        if (pf[i].revents & POLLOUT) { cl_flush(cl); if (cl->fd >= 0 && cl->dumping) dump_more(cl); }
        if (cl->fd >= 0 && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) cl_read(cl, idx[i]);
        break;
      }
      case 3: if (streams[idx[i]].fd >= 0) stream_read(&streams[idx[i]]); break;
      case 4: core_socket_ready(&C, idx[i]); break;
      }
    }
  }
  /* poweroff: everything is stopped */
  for (int i = 0; i < MAXSTREAMS; i++) if (streams[i].fd >= 0) stream_read(&streams[i]);
  for (int i = 0; i < MAXCLIENTS; i++) if (clients[i].fd >= 0) { cl_flush(&clients[i]); close(clients[i].fd); }
  if (ctl_fd >= 0) { close(ctl_fd); unlink(control_path); }
  if (notify_fd >= 0) { close(notify_fd); unlink(notify_path); }
  snprintf(p, sizeof p, "%s%s", prefix, SVC_RUNNING);
  rmdir(p);
  if (!quiet) { const char *m = is_pid1 ? "System halted. Reload the page to start again.\n" : NAME ": all units stopped.\n"; if (write(1, m, strlen(m)) < 0) {} }
  return 0;
}
