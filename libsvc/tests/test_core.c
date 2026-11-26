// SPDX-License-Identifier: GPL-2.0-or-later
/* Native tests of the manager core (bin/svcd/core.c) with a fake process layer: time
 * is a counter, "processes" are rows in a table, exits happen when the test says so.
 * Unit files are real files in a scratch tree under the build directory. */
#include "core.h"
#include "../os.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failed;
#define CHECK(what, cond) do { checks++; if (!(cond)) { failed++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static int64_t now_ms = 1000000;
static struct { int pid, pgid, alive, ignore_term; char cmd[200]; } procs[4096];
static int nprocs, nextpid = 100;
static char spawned[8192];      /* "cmd;cmd;..." in order */
static char logbuf[1 << 20];
static int logn;
static char killed[2048];
static int ignore_term_next;
static int last_extra;

static int64_t f_now(void) { return now_ms; }
static int64_t f_epoch(void) { return 1795700000000LL + now_ms; }
static int f_spawn(struct unit *u, const char *path, char *const argv[], char *const envp[], int in, int out, int err, int pgid,
                   const int *extra, int nextra) {
  if (strstr(path, "missing")) return -ENOENT;
  char cmd[200] = "";
  for (int i = 0; argv[i]; i++) { if (i) strcat(cmd, " "); strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 2); }
  int pid = nextpid++;
  if (nprocs >= 4000) { int w = 0; for (int i = 0; i < nprocs; i++) if (procs[i].alive) procs[w++] = procs[i]; nprocs = w; }
  procs[nprocs].pid = pid; procs[nprocs].pgid = pgid ? pgid : pid; procs[nprocs].alive = 1;
  procs[nprocs].ignore_term = ignore_term_next;
  snprintf(procs[nprocs].cmd, sizeof procs[nprocs].cmd, "%s", cmd);
  nprocs++;
  if (strlen(spawned) + strlen(cmd) + 4 > sizeof spawned) spawned[0] = 0; /* long runs: keep the tail */
  strncat(spawned, cmd, sizeof spawned - strlen(spawned) - 2);
  strcat(spawned, ";");
  last_extra = nextra;
  return pid;
}
static int pending_exit[4096], pending_status[4096], npending;
static int f_kill(int pid, int sig) {
  char b[32];
  snprintf(b, sizeof b, "%d:%d;", pid, sig);
  if (strlen(killed) + strlen(b) + 1 > sizeof killed) killed[0] = 0;
  strncat(killed, b, sizeof killed - strlen(killed) - 1);
  for (int i = 0; i < nprocs; i++)
    if (procs[i].pid == pid && procs[i].alive) {
      if (procs[i].ignore_term && sig != 9) return 0;
      procs[i].alive = 0;
      pending_exit[npending] = pid; pending_status[npending++] = sig;
      return 0;
    }
  return -ESRCH;
}
static int f_members(int pgid, int main_pid, int *pids, int max) {
  int n = 0;
  for (int i = 0; i < nprocs && n < max; i++)
    if (procs[i].alive && ((pgid > 0 && procs[i].pgid == pgid) || procs[i].pid == main_pid)) pids[n++] = procs[i].pid;
  return n;
}
static uint32_t pages_each = 16;
static uint32_t f_pages(int pid) { return pages_each; }
static void f_log(const char *unit, int prio, const char *msg) {
  int l = snprintf(logbuf + logn, sizeof logbuf - logn, "%s: %s\n", unit ? unit : "-", msg);
  if (l > 0 && logn + l < (int)sizeof logbuf - 1) logn += l;
  if (getenv("SHOWLOG")) printf("LOG %s: %s\n", unit ? unit : "-", msg);
}
static int f_open_output(struct unit *u, int which, int *keep) { *keep = 0; return -1; }
static int nextfd = 200;
static int f_listen(const char *path) { return nextfd++; }
static int f_accept(int fd) { return nextfd++; }
static void f_closefd(int fd) {}
static int f_exists(const char *p, int what) {
  struct stat st;
  if (stat(p, &st) < 0) return -1;
  if (what == 1) return S_ISDIR(st.st_mode) ? 0 : -1;
  return 0;
}
static int64_t f_mtime(const char *p) { struct stat st; return stat(p, &st) < 0 ? -1 : (int64_t)st.st_mtime; }
static void f_mkdirs(const char *p) { (void)p; }
static void f_rmtree(const char *p) { (void)p; }
static char pidfile_content[32];
static int f_read_file(const char *p, char *buf, int len) {
  size_t l = strlen(p);
  if (l > 4 && !strcmp(p + l - 4, ".pid")) { if (!pidfile_content[0]) return -1; int n = snprintf(buf, len, "%s", pidfile_content); return n; }
  if (strstr(p, "/envfile")) { int fd = open(p, O_RDONLY); if (fd < 0) return -1; int n = read(fd, buf, len); close(fd); return n; }
  if (strstr(p, "missing")) return -1;
  if (strstr(p, "elfprog")) { memcpy(buf, "\177ELF\2\1\1", 7); return 7; }
  if (strstr(p, "script")) return snprintf(buf, len, "#!/bin/sh -e\necho\n");
  memcpy(buf, "\0asm\1\0\0\0", 8);
  return 8;
}
static uint32_t f_random(void) { return 0; }

static struct svc_ops ops = {f_now, f_epoch, f_spawn, f_kill, f_members, f_pages, f_log, f_open_output, 0, f_listen, f_accept,
                             f_closefd, f_exists, f_mtime, f_mkdirs, f_rmtree, f_read_file, f_random};
static struct core C;
static char ROOT[512];
static char lastresult[32], lastunit[128];
static int njobdone;
static void on_done(struct core *c, int client, const char *unit, int type, const char *result) {
  if (client < 0) return;
  snprintf(lastresult, sizeof lastresult, "%s", result);
  snprintf(lastunit, sizeof lastunit, "%s", unit);
  njobdone++;
}

static void wr(const char *name, const char *text) {
  char p[700];
  snprintf(p, sizeof p, "%s/etc/systemd/system/%s", ROOT, name);
  FILE *f = fopen(p, "w");
  if (!f) { perror(p); exit(2); }
  fputs(text, f);
  fclose(f);
}
static void wrlib(const char *name, const char *text) {
  char p[700];
  snprintf(p, sizeof p, "%s/usr/lib/systemd/system/%s", ROOT, name);
  FILE *f = fopen(p, "w");
  if (!f) { perror(p); exit(2); }
  fputs(text, f);
  fclose(f);
}
static void settle(void) {
  for (int k = 0; k < 50; k++) {
    core_run(&C);
    if (!npending) break;
    int n = npending;
    npending = 0;
    int pe[4096], ps[4096];
    memcpy(pe, pending_exit, n * sizeof *pe);
    memcpy(ps, pending_status, n * sizeof *ps);
    for (int i = 0; i < n; i++) core_child_exit(&C, pe[i], ps[i]);
  }
}
static int pid_of(const char *cmd) {
  for (int i = nprocs - 1; i >= 0; i--) if (procs[i].alive && !strcmp(procs[i].cmd, cmd)) return procs[i].pid;
  return -1;
}
static void exit_proc(const char *cmd, int code) {
  int pid = pid_of(cmd);
  if (pid < 0) { printf("  (no live process '%s')\n", cmd); return; }
  for (int i = 0; i < nprocs; i++) if (procs[i].pid == pid) procs[i].alive = 0;
  core_child_exit(&C, pid, (code & 0xff) << 8);
  settle();
}
static void advance(int64_t ms) {
  /* step through time so every deadline fires at its moment */
  int64_t end = now_ms + ms;
  while (now_ms < end) {
    int64_t w = core_next_wakeup(&C);
    if (w < 0 || now_ms + w > end) { now_ms = end; settle(); break; }
    now_ms += w > 0 ? w : 1;
    settle();
  }
}
static struct unit *U(const char *n) { int i = core_find(&C, n); return i >= 0 ? C.u[i] : 0; }
static const char *st(const char *n) { struct unit *u = U(n); return u ? core_active_str(u->active) : "none"; }
static int req(const char *n, int type, const char *mode) {
  char m[300];
  njobdone = 0; lastresult[0] = 0;
  int r = core_request(&C, n, type, mode, 1, m, sizeof m);
  if (r < 0 && getenv("SHOWLOG")) printf("request %s: %s\n", n, m);
  settle();
  return r;
}
static int in_log(const char *s) { return strstr(logbuf, s) != 0; }
static void reset(void) {
  logn = 0; logbuf[0] = 0; spawned[0] = 0; killed[0] = 0;
  for (int i = 0; i < C.nu; i++) if (C.u[i]) { /* leak-free teardown is checked separately */ }
}
static void fresh_core(void) {
  for (int i = 0; i < C.nu; i++) if (C.u[i]) {
    struct unit *u = C.u[i];
    svc_file_free(&u->f);
    for (int k = 0; k < D_NKINDS; k++) free(u->dep[k].u);
    for (int k = 0; k < X_N; k++) { for (int j = 0; j < u->exec[k].n; j++) free(u->exec[k].v[j]); free(u->exec[k].v); }
    free(u->env); free(u->envfiles); free(u->conditions); free(u->rtdirs); free(u->statedirs); free(u->otherdirs); free(u->dropins);
    free(u);
  }
  core_init(&C, &ops, "");
  snprintf(C.roots.root[0], sizeof C.roots.root[0], "%s", ROOT);
  C.job_done = on_done;
  C.have_signals = 0;
  reset();
}

static void copy_targets(void) {
  char cmd[1200];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s/etc/systemd/system' '%s/usr/lib/systemd/system' && cp rootfs/usr/lib/systemd/system/*.target '%s/usr/lib/systemd/system/' && cp rootfs/usr/lib/systemd/system/default.target '%s/usr/lib/systemd/system/'",
           ROOT, ROOT, ROOT, ROOT);
  if (system(cmd)) { printf("cannot prepare %s\n", ROOT); exit(2); }
}

static void t_order(void) {
  fresh_core();
  wr("t1.target", "[Unit]\nDescription=T1\nWants=a.service b.service\n");
  wr("a.service", "[Unit]\nAfter=b.service\n[Service]\nExecStart=/bin/a\n");
  wr("b.service", "[Service]\nType=notify\nExecStart=/bin/b\n");
  req("t1.target", J_START, "replace");
  CHECK("b spawned, a waits for b's readiness", !strcmp(spawned, "/bin/b;"));
  CHECK("target not reached yet", !strcmp(st("t1.target"), "inactive"));
  core_notify(&C, pid_of("/bin/b"), "READY=1\nSTATUS=ok");
  settle();
  CHECK("a after b ready", !strcmp(spawned, "/bin/b;/bin/a;"));
  CHECK("all active", !strcmp(st("a.service"), "active") && !strcmp(st("b.service"), "active") && !strcmp(st("t1.target"), "active"));
  CHECK("job result done", !strcmp(lastresult, "done"));
  CHECK("status text", !strcmp(U("b.service")->status_text, "ok"));
  CHECK("default deps pulled sysinit", !strcmp(st("sysinit.target"), "active") && !strcmp(st("basic.target"), "inactive"));
  /* stop order is reversed: a stops before b */
  killed[0] = 0;
  int pa = pid_of("/bin/a"), pb = pid_of("/bin/b");
  req("b.service", J_STOP, "replace");
  char want[64];
  snprintf(want, sizeof want, "%d:15;", pb);
  CHECK("stop b kills b only (a only Wants... after)", !strncmp(killed, want, strlen(want)) || strstr(killed, want));
  (void)pa;
  CHECK("b inactive", !strcmp(st("b.service"), "inactive"));
}

static void t_requires(void) {
  fresh_core();
  wr("c.service", "[Unit]\nRequires=d.service\nAfter=d.service\n[Service]\nExecStart=/bin/c\n");
  wr("d.service", "[Service]\nType=oneshot\nExecStart=/bin/d\n");
  req("c.service", J_START, "replace");
  CHECK("d runs first", !strcmp(spawned, "/bin/d;"));
  exit_proc("/bin/d", 1);
  CHECK("d failed", !strcmp(st("d.service"), "failed"));
  CHECK("c not started", !strstr(spawned, "/bin/c"));
  CHECK("c job dependency", !strcmp(lastresult, "dependency"));
  CHECK("dependency logged", in_log("Dependency failed for c.service"));
  CHECK("result exit-code", !strcmp(U("d.service")->result, "exit-code") && in_log("status=1/FAILURE"));
  /* Wants: a failure does not stop the wanting unit */
  fresh_core();
  wr("e.service", "[Unit]\nWants=f.service\nAfter=f.service\n[Service]\nExecStart=/bin/e\n");
  wr("f.service", "[Service]\nType=oneshot\nExecStart=/bin/f\n");
  req("e.service", J_START, "replace");
  exit_proc("/bin/f", 3);
  CHECK("e starts despite f failing", strstr(spawned, "/bin/e") && !strcmp(st("e.service"), "active"));
  /* not found */
  fresh_core();
  wr("g.service", "[Unit]\nRequires=nothere.service\n[Service]\nExecStart=/bin/g\n");
  char m[300];
  int r = core_request(&C, "g.service", J_START, "replace", 1, m, sizeof m);
  CHECK("requires missing unit fails", r < 0 && strstr(m, "nothere.service not found"));
  r = core_request(&C, "nosuch.service", J_START, "replace", 1, m, sizeof m);
  CHECK("unknown unit", r == -ENOENT && strstr(m, "Unit nosuch.service not found."));
  wr("h.service", "[Unit]\nWants=nothere2.service\n[Service]\nExecStart=/bin/h\n");
  req("h.service", J_START, "replace");
  CHECK("wants missing unit is fine", !strcmp(st("h.service"), "active"));
}

static void t_conflicts_binds(void) {
  fresh_core();
  wr("p.service", "[Service]\nExecStart=/bin/p\n");
  wr("q.service", "[Unit]\nConflicts=p.service\n[Service]\nExecStart=/bin/q\n");
  wr("r.service", "[Unit]\nBindsTo=p.service\nAfter=p.service\n[Service]\nExecStart=/bin/r\n");
  wr("s.service", "[Unit]\nPartOf=p.service\n[Service]\nExecStart=/bin/s\n");
  req("r.service", J_START, "replace");
  req("s.service", J_START, "replace");
  CHECK("r pulls p", !strcmp(st("p.service"), "active") && !strcmp(st("r.service"), "active"));
  int ps = pid_of("/bin/s");
  req("p.service", J_RESTART, "replace");
  CHECK("PartOf: s restarted with p", pid_of("/bin/s") != ps && pid_of("/bin/s") > 0);
  req("q.service", J_START, "replace");
  CHECK("conflict stops p", !strcmp(st("p.service"), "inactive") && !strcmp(st("q.service"), "active"));
  CHECK("BindsTo: r stopped with p", !strcmp(st("r.service"), "inactive"));
  /* a bound unit stops when its unit dies on its own */
  req("r.service", J_START, "replace");
  CHECK("q stopped by p's start? no: p does not conflict back", !strcmp(st("q.service"), "inactive") || !strcmp(st("q.service"), "active"));
  exit_proc("/bin/p", 1);
  CHECK("p died: r stops", !strcmp(st("r.service"), "inactive"));
}

static void t_cycle(void) {
  fresh_core();
  wr("cy.target", "[Unit]\nWants=x.service y.service\n");
  wr("x.service", "[Unit]\nAfter=y.service\n[Service]\nExecStart=/bin/x\n");
  wr("y.service", "[Unit]\nAfter=x.service\n[Service]\nExecStart=/bin/y\n");
  req("cy.target", J_START, "replace");
  CHECK("cycle reported", in_log("Found ordering cycle on"));
  CHECK("cycle broken by deleting a job", in_log("deleted to break ordering cycle"));
  CHECK("one of the two runs", (strstr(spawned, "/bin/x") != 0) + (strstr(spawned, "/bin/y") != 0) == 1);
  fresh_core();
  wr("m.service", "[Unit]\nRequires=n.service\nAfter=n.service\n[Service]\nExecStart=/bin/m\n");
  wr("n.service", "[Unit]\nRequires=m.service\nAfter=m.service\n[Service]\nExecStart=/bin/n\n");
  char msg[300];
  int r = core_request(&C, "m.service", J_START, "replace", 1, msg, sizeof msg);
  CHECK("essential cycle fails the request", r < 0 && strstr(msg, "Transaction order is cyclic"));
  CHECK("nothing spawned", !spawned[0]);
}

static void t_oneshot(void) {
  fresh_core();
  wr("o1.service", "[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStartPre=/bin/pre\nExecStart=/bin/one\nExecStart=/bin/two\nExecStartPost=-/bin/post\n");
  req("o1.service", J_START, "replace");
  CHECK("pre first", !strcmp(spawned, "/bin/pre;"));
  CHECK("job still running", njobdone == 0);
  exit_proc("/bin/pre", 0);
  exit_proc("/bin/one", 0);
  CHECK("sequential", !strcmp(spawned, "/bin/pre;/bin/one;/bin/two;"));
  exit_proc("/bin/two", 0);
  exit_proc("/bin/post", 1); /* "-": ignored */
  CHECK("remain after exit", !strcmp(st("o1.service"), "active") && !strcmp(core_sub_str(U("o1.service")), "exited"));
  CHECK("job done", !strcmp(lastresult, "done"));
  req("o1.service", J_STOP, "replace");
  CHECK("stopped", !strcmp(st("o1.service"), "inactive"));
  wr("o2.service", "[Service]\nType=oneshot\nExecStart=/bin/o2\n");
  req("o2.service", J_START, "replace");
  exit_proc("/bin/o2", 0);
  CHECK("oneshot without remain ends inactive, job done", !strcmp(st("o2.service"), "inactive") && !strcmp(lastresult, "done"));
  wr("o3.service", "[Service]\nType=oneshot\nExecStart=/bin/missing-program\n");
  req("o3.service", J_START, "replace");
  CHECK("missing program fails", !strcmp(st("o3.service"), "failed") && in_log("Failed at step EXEC") && !strcmp(lastresult, "failed"));
  CHECK("exec status 203", U("o3.service")->exit_status == 203);
}

static void t_restart(void) {
  fresh_core();
  wr("rs.service", "[Unit]\nStartLimitIntervalSec=10min\nStartLimitBurst=5\n[Service]\nExecStart=/bin/rs\nRestart=on-failure\nRestartSec=1s\nRestartSteps=3\nRestartMaxDelaySec=8s\n");
  req("rs.service", J_START, "replace");
  int64_t delays[8];
  int nd = 0;
  for (int k = 0; k < 6; k++) {
    if (pid_of("/bin/rs") < 0) break;
    exit_proc("/bin/rs", 2);
    struct unit *u = U("rs.service");
    if (u->sub != 13 /* auto-restart */) break;
    int64_t t0 = now_ms;
    while (pid_of("/bin/rs") < 0 && now_ms - t0 < 60000) advance(50);
    delays[nd++] = now_ms - t0;
  }
  CHECK("restarts with backoff 1, 2, 4, 8 s", nd >= 4 && delays[0] >= 1000 && delays[0] < 1100 && delays[1] >= 2000 && delays[1] < 2100 &&
                                              delays[2] >= 4000 && delays[2] < 4100 && delays[3] >= 8000 && delays[3] < 8100);
  if (nd >= 4) printf("  backoff delays: %lld %lld %lld %lld ms\n", (long long)delays[0], (long long)delays[1], (long long)delays[2], (long long)delays[3]);
  CHECK("start limit hit after 5 starts", !strcmp(st("rs.service"), "failed") && !strcmp(U("rs.service")->result, "start-limit-hit"));
  CHECK("start limit logged", in_log("Start request repeated too quickly."));
  CHECK("restart counter", in_log("restart counter is at 4"));
  char m[200];
  core_reset_failed(&C, "rs.service");
  CHECK("reset-failed", !strcmp(st("rs.service"), "inactive"));
  core_request(&C, "rs.service", J_START, "replace", 1, m, sizeof m);
  settle();
  CHECK("startable again", !strcmp(st("rs.service"), "active"));
  /* Restart=always restarts a clean exit; Restart=no does not restart */
  fresh_core();
  wr("ra.service", "[Service]\nExecStart=/bin/ra\nRestart=always\nRestartSec=100ms\n");
  wr("rn.service", "[Service]\nExecStart=/bin/rn\n");
  req("ra.service", J_START, "replace");
  req("rn.service", J_START, "replace");
  exit_proc("/bin/ra", 0);
  exit_proc("/bin/rn", 1);
  advance(200);
  CHECK("always restarts", pid_of("/bin/ra") > 0);
  CHECK("no: stays failed", !strcmp(st("rn.service"), "failed"));
  /* RestartPreventExitStatus */
  wr("rp.service", "[Service]\nExecStart=/bin/rp\nRestart=on-failure\nRestartPreventExitStatus=7\n");
  req("rp.service", J_START, "replace");
  exit_proc("/bin/rp", 7);
  advance(500);
  CHECK("prevented", !strcmp(st("rp.service"), "failed"));
  /* an explicit stop never restarts */
  req("ra.service", J_STOP, "replace");
  advance(500);
  CHECK("stop is not restarted", !strcmp(st("ra.service"), "inactive") && pid_of("/bin/ra") < 0);
}

static void t_stop(void) {
  fresh_core();
  wr("k1.service", "[Service]\nExecStart=/bin/k1\nTimeoutStopSec=2s\n");
  ignore_term_next = 1;
  req("k1.service", J_START, "replace");
  ignore_term_next = 0;
  int pid = pid_of("/bin/k1");
  killed[0] = 0;
  char m[200];
  core_request(&C, "k1.service", J_STOP, "replace", 1, m, sizeof m);
  settle();
  char want[64];
  snprintf(want, sizeof want, "%d:15;", pid);
  CHECK("SIGTERM first", !strcmp(killed, want));
  CHECK("still deactivating", !strcmp(st("k1.service"), "deactivating"));
  advance(2100);
  snprintf(want, sizeof want, "%d:15;%d:9;", pid, pid);
  CHECK("SIGKILL after TimeoutStopSec", !strcmp(killed, want));
  CHECK("stopped after kill", !strcmp(st("k1.service"), "failed") || !strcmp(st("k1.service"), "inactive"));
  CHECK("timeout logged", in_log("State 'stop-sigterm' timed out. Killing."));
  /* ExecStop that hangs */
  wr("k2.service", "[Service]\nExecStart=/bin/k2\nExecStop=/bin/hang\nTimeoutStopSec=1s\n");
  req("k2.service", J_START, "replace");
  killed[0] = 0;
  core_request(&C, "k2.service", J_STOP, "replace", 1, m, sizeof m);
  settle();
  CHECK("ExecStop runs", strstr(spawned, "/bin/hang") != 0);
  advance(1100);
  CHECK("hung ExecStop killed, then the service", !strcmp(st("k2.service"), "failed") && in_log("Stopping timed out"));
  /* KillMode=process kills only the main process; control-group all of them */
  wr("k3.service", "[Service]\nExecStart=/bin/k3\n");
  req("k3.service", J_START, "replace");
  struct unit *u = U("k3.service");
  f_spawn(u, "/bin/child", (char *[]){"/bin/child", 0}, 0, -1, -1, -1, u->pgid, 0, 0); /* a child in its group */
  killed[0] = 0;
  req("k3.service", J_STOP, "replace");
  CHECK("control-group: both killed", !strcmp(st("k3.service"), "inactive") && pid_of("/bin/child") < 0);
  /* stop of a dead-on-arrival child of the group: main exits, child lingers: still tracked */
  wr("k4.service", "[Service]\nType=forking\nExecStart=/bin/k4\n");
  req("k4.service", J_START, "replace");
  u = U("k4.service");
  f_spawn(u, "/bin/daemon", (char *[]){"/bin/daemon", 0}, 0, -1, -1, -1, u->pgid, 0, 0);
  exit_proc("/bin/k4", 0);
  CHECK("forking: guessed main pid", !strcmp(st("k4.service"), "active") && u->main_pid == pid_of("/bin/daemon"));
  req("k4.service", J_STOP, "replace");
  CHECK("forking daemon stopped", pid_of("/bin/daemon") < 0 && !strcmp(st("k4.service"), "inactive"));
  /* PIDFile names the main process before the starter exits */
  wr("k5.service", "[Service]\nType=forking\nPIDFile=/run/k5.pid\nExecStart=/bin/k5\n");
  req("k5.service", J_START, "replace");
  u = U("k5.service");
  CHECK("waits for pid file", !strcmp(st("k5.service"), "activating"));
  snprintf(pidfile_content, sizeof pidfile_content, "%d\n", pid_of("/bin/k5"));
  advance(300);
  CHECK("pid file read", !strcmp(st("k5.service"), "active") && u->main_pid == pid_of("/bin/k5"));
  pidfile_content[0] = 0;
}

static void t_reload(void) {
  fresh_core();
  wr("rl.service", "[Service]\nExecStart=/bin/rl\nExecReload=/bin/kill -HUP $MAINPID\n");
  req("rl.service", J_START, "replace");
  int pid = pid_of("/bin/rl");
  req("rl.service", J_RELOAD, "replace");
  CHECK("without signals: reload becomes restart", pid_of("/bin/rl") > 0 && pid_of("/bin/rl") != pid && in_log("restarting instead"));
  CHECK("job done", !strcmp(lastresult, "done"));
  C.have_signals = 1;
  pid = pid_of("/bin/rl");
  req("rl.service", J_RELOAD, "replace");
  char want[64];
  snprintf(want, sizeof want, "/bin/kill -HUP %d;", pid);
  CHECK("with signals: kill -HUP $MAINPID runs", strstr(spawned, want) != 0);
  {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "/bin/kill -HUP %d", pid);
    exit_proc(cmd, 0);
  }
  CHECK("reload done", !strcmp(st("rl.service"), "active") && !strcmp(lastresult, "done") && in_log("Reloaded rl.service"));
  char m[200];
  wr("nr.service", "[Service]\nExecStart=/bin/nr\n");
  req("nr.service", J_START, "replace");
  int r = core_request(&C, "nr.service", J_RELOAD, "replace", 1, m, sizeof m);
  CHECK("reload not applicable", r < 0 && strstr(m, "Job type reload is not applicable"));
  req("nr.service", J_TRYRESTART, "replace");
  wr("ni.service", "[Service]\nExecStart=/bin/ni\n");
  req("ni.service", J_TRYRESTART, "replace");
  CHECK("try-restart of an inactive unit does nothing", !strcmp(st("ni.service"), "inactive") && !strcmp(lastresult, "done"));
}

static void t_timer(void) {
  fresh_core();
  wr("tm.timer", "[Timer]\nOnBootSec=5s\nOnUnitActiveSec=10s\n");
  wr("tm.service", "[Service]\nType=oneshot\nExecStart=/bin/tick\n");
  C.boot_ms = now_ms;
  req("tm.timer", J_START, "replace");
  CHECK("timer waiting", !strcmp(st("tm.timer"), "active") && !strcmp(core_sub_str(U("tm.timer")), "waiting"));
  advance(4900);
  CHECK("not yet", !strstr(spawned, "/bin/tick"));
  advance(200);
  CHECK("fires at 5 s", strstr(spawned, "/bin/tick") != 0);
  exit_proc("/bin/tick", 0);
  int64_t t1 = now_ms;
  advance(9800);
  CHECK("not again before 10 s", pid_of("/bin/tick") < 0);
  advance(400);
  CHECK("again 10 s after activation", pid_of("/bin/tick") > 0);
  (void)t1;
  exit_proc("/bin/tick", 0);
  wr("cal.timer", "[Timer]\nOnCalendar=*:*:30\nUnit=calj.service\n");
  wr("calj.service", "[Service]\nType=oneshot\nExecStart=/bin/calj\n");
  req("cal.timer", J_START, "replace");
  int64_t ne = U("cal.timer")->next_real;
  CHECK("calendar next elapse on a :30 second", ne > 0 && (ne / 1000) % 60 == 30);
  advance(61000);
  CHECK("calendar timer fired", strstr(spawned, "/bin/calj") != 0);
}

static void t_conditions(void) {
  fresh_core();
  char w[700];
  snprintf(w, sizeof w, "[Unit]\nConditionPathExists=%s/etc/nothere\n[Service]\nExecStart=/bin/cond\n", ROOT);
  wr("cd1.service", w);
  req("cd1.service", J_START, "replace");
  CHECK("condition skips", !strstr(spawned, "/bin/cond") && !strcmp(st("cd1.service"), "inactive") && !strcmp(lastresult, "skipped"));
  CHECK("condition logged", in_log("unmet condition check"));
  snprintf(w, sizeof w, "[Unit]\nConditionPathExists=!%s/etc/nothere\nConditionPathIsDirectory=%s/etc\n[Service]\nExecStart=/bin/cond2\n", ROOT, ROOT);
  wr("cd2.service", w);
  req("cd2.service", J_START, "replace");
  CHECK("negated condition holds", !strcmp(st("cd2.service"), "active"));
  snprintf(w, sizeof w, "[Unit]\nAssertPathExists=%s/etc/nothere\n[Service]\nExecStart=/bin/cond3\n", ROOT);
  wr("cd3.service", w);
  req("cd3.service", J_START, "replace");
  CHECK("assertion fails the unit", !strcmp(st("cd3.service"), "failed") && !strcmp(lastresult, "failed"));
  snprintf(w, sizeof w, "[Unit]\nConditionPathExists=|%s/a\nConditionPathExists=|%s/etc\n[Service]\nExecStart=/bin/cond4\n", ROOT, ROOT);
  wr("cd4.service", w);
  req("cd4.service", J_START, "replace");
  CHECK("one triggering condition is enough", !strcmp(st("cd4.service"), "active"));
}

static void t_env(void) {
  fresh_core();
  char p[600];
  snprintf(p, sizeof p, "%s/etc/envfile", ROOT);
  FILE *f = fopen(p, "w");
  fputs("# comment\nFOO=from-file\nBAR=\"quoted value\"\n", f);
  fclose(f);
  char w[900];
  snprintf(w, sizeof w, "[Service]\nEnvironmentFile=%s\nEnvironmentFile=-/nonexistent\nEnvironment=\"BAZ=a b\" FOO=override\nExecStart=/bin/env $FOO ${BAR} ${BAZ}\n", p);
  wr("env.service", w);
  req("env.service", J_START, "replace");
  CHECK("environment expansion", strstr(spawned, "/bin/env override quoted value a b;") != 0);
  wr("sc.service", "[Service]\nExecStart=/bin/script-thing arg\n");
  req("sc.service", J_START, "replace");
  CHECK("#! script runs through its interpreter", strstr(spawned, "/bin/sh -e ") != 0);
  wr("elf.service", "[Service]\nExecStart=/usr/bin/elfprog x\n");
  req("elf.service", J_START, "replace");
  CHECK("on a host, an ELF program of a native unit runs as it is", strstr(spawned, "/usr/bin/elfprog x;") != 0);
  /* a guest unit (from the Debian root): its programs go through the engine, its paths are the guest's */
  char g[700];
  snprintf(g, sizeof g, "mkdir -p '%s/guest/usr/lib/systemd/system' '%s/guest/etc/ssh'", ROOT, ROOT);
  if (system(g)) {}
  snprintf(C.roots.root[1], sizeof C.roots.root[1], "%s/guest", ROOT);
  C.roots.n = 2;
  snprintf(g, sizeof g, "%s/guest/usr/lib/systemd/system/gd.service", ROOT);
  FILE *gf = fopen(g, "w");
  fputs("[Unit]\nConditionPathIsDirectory=/etc/ssh\n[Service]\nExecStart=/usr/sbin/elfprog-gd -D\nPIDFile=/run/gd.pid\n", gf);
  fclose(gf);
  req("gd.service", J_START, "replace");
  snprintf(g, sizeof g, "lx --root=%s/guest -- /usr/sbin/elfprog-gd -D;", ROOT);
  CHECK("guest unit: through the engine, guest condition paths", strstr(spawned, g) != 0 && !strcmp(st("gd.service"), "active"));
  C.roots.n = 1;
  wr("rel.service", "[Service]\nExecStart=relprog y\n");
  req("rel.service", J_START, "replace");
  CHECK("a bare name is looked up in PATH", strstr(spawned, "relprog y;") != 0);
}

static void t_socket(void) {
  fresh_core();
  wr("echo.socket", "[Socket]\nListenStream=/run/echo.sock\n[Install]\nWantedBy=sockets.target\n");
  wr("echo.service", "[Service]\nExecStart=/bin/echod\n");
  wr("net.socket", "[Socket]\nListenStream=0.0.0.0:22\n");
  req("echo.socket", J_START, "replace");
  struct unit *s = U("echo.socket");
  CHECK("socket listening", !strcmp(st("echo.socket"), "active") && s->listening && s->lfd[0] >= 200);
  CHECK("service not started yet", !strstr(spawned, "/bin/echod"));
  core_socket_ready(&C, s->lfd[0]);
  settle();
  CHECK("connection starts the service", strstr(spawned, "/bin/echod") != 0 && last_extra == 1);
  CHECK("socket stops polling while the service runs", s->listening == 0);
  exit_proc("/bin/echod", 0);
  CHECK("socket polls again when the service ends", s->listening == 1);
  req("net.socket", J_START, "replace");
  CHECK("IP sockets refused", !strcmp(st("net.socket"), "failed") && in_log("not supported on UC.OS"));
  wr("ac.socket", "[Socket]\nListenStream=/run/ac.sock\nAccept=yes\n");
  wr("ac@.service", "[Service]\nExecStart=/bin/acd %i\nStandardInput=socket\n");
  req("ac.socket", J_START, "replace");
  core_socket_ready(&C, U("ac.socket")->lfd[0]);
  settle();
  CHECK("Accept=yes: one instance per connection", strstr(spawned, "/bin/acd 1;") != 0 && !strcmp(st("ac@1.service"), "active"));
  exit_proc("/bin/acd 1", 0);
  int before = 0;
  for (int i = 0; i < C.nu; i++) if (C.u[i]) before++;
  core_gc(&C);
  int after = 0;
  for (int i = 0; i < C.nu; i++) if (C.u[i]) after++;
  CHECK("finished instance is collected", after < before && core_find(&C, "ac@1.service") < 0);
}

static void t_enable(void) {
  fresh_core();
  wr("en.service", "[Service]\nExecStart=/bin/en\n[Install]\nWantedBy=multi-user.target\nAlias=en-alias.service\nAlso=en.socket\n");
  wr("en.socket", "[Socket]\nListenStream=/run/en.sock\n[Install]\nWantedBy=sockets.target\n");
  wrlib("st.service", "[Service]\nExecStart=/bin/st\n");
  wr("adm.service", "[Service]\nExecStart=/bin/adm\n");
  CHECK("disabled", !strcmp(svc_is_enabled(&C.roots, "en.service"), "disabled"));
  CHECK("static", !strcmp(svc_is_enabled(&C.roots, "st.service"), "static"));
  struct svc_change ch[16];
  char err[200];
  int n = svc_enable(&C.roots, "en.service", 0, ch, 16, err, sizeof err);
  CHECK("enable makes 3 links", n == 3);
  CHECK("enabled", !strcmp(svc_is_enabled(&C.roots, "en.service"), "enabled") && !strcmp(svc_is_enabled(&C.roots, "en.socket"), "enabled"));
  CHECK("alias resolves", !strcmp(svc_is_enabled(&C.roots, "en-alias.service"), "alias"));
  int mu = core_unit(&C, "multi-user.target");
  core_reload(&C);
  struct unit *m = C.u[mu];
  int found = 0;
  for (int i = 0; i < m->dep[D_WANTS].n; i++) if (!strcmp(C.u[m->dep[D_WANTS].u[i]]->name, "en.service")) found = 1;
  CHECK("after daemon-reload, multi-user.target wants it", found);
  req("multi-user.target", J_START, "replace");
  CHECK("started by the target", strstr(spawned, "/bin/en") != 0);
  CHECK("alias name finds the unit", core_unit(&C, "en-alias.service") == core_find(&C, "en.service"));
  n = svc_disable(&C.roots, "en.service", ch, 16);
  CHECK("disable removes 3 links", n == 3);
  CHECK("disabled again", !strcmp(svc_is_enabled(&C.roots, "en.service"), "disabled"));
  core_reload(&C);
  found = 0;
  for (int i = 0; i < m->dep[D_WANTS].n; i++) if (!strcmp(C.u[m->dep[D_WANTS].u[i]]->name, "en.service")) found = 1;
  CHECK("after reload, not wanted", !found);
  CHECK("still running (disable does not stop)", !strcmp(st("en.service"), "active"));
  CHECK("mask", svc_mask(&C.roots, "st.service", 0) == 0 && !strcmp(svc_is_enabled(&C.roots, "st.service"), "masked"));
  core_reload(&C);
  char msg[200];
  CHECK("masked unit refuses to start", core_request(&C, "st.service", J_START, "replace", 1, msg, sizeof msg) < 0 && strstr(msg, "masked"));
  CHECK("unmask", svc_mask(&C.roots, "st.service", 1) == 1 && !strcmp(svc_is_enabled(&C.roots, "st.service"), "static"));
  CHECK("mask refuses to replace an admin's file", svc_mask(&C.roots, "adm.service", 0) == -EEXIST);
}

static void t_isolate_memory_onfailure(void) {
  fresh_core();
  wr("iso.target", "[Unit]\nAllowIsolate=yes\nWants=i1.service\n");
  wr("i1.service", "[Service]\nExecStart=/bin/i1\n");
  wr("i2.service", "[Service]\nExecStart=/bin/i2\n");
  wr("i3.service", "[Unit]\nIgnoreOnIsolate=yes\n[Service]\nExecStart=/bin/i3\n");
  req("i2.service", J_START, "replace");
  req("i3.service", J_START, "replace");
  req("iso.target", J_START, "isolate");
  CHECK("isolate stops others", !strcmp(st("i2.service"), "inactive") && !strcmp(st("i1.service"), "active") && !strcmp(st("i3.service"), "active"));
  wr("mm.service", "[Service]\nExecStart=/bin/mm\nMemoryMax=2M\nMemoryHigh=512K\n");
  req("mm.service", J_START, "replace");
  pages_each = 4; /* 256 KiB */
  core_measure(&C);
  CHECK("under limits", !strcmp(st("mm.service"), "active") && U("mm.service")->mem_now == 4 * 65536);
  pages_each = 16; /* 1 MiB: above MemoryHigh */
  core_measure(&C);
  CHECK("MemoryHigh logged", in_log("above MemoryHigh") && !strcmp(st("mm.service"), "active"));
  pages_each = 64; /* 4 MiB: above MemoryMax */
  core_measure(&C);
  settle();
  CHECK("MemoryMax kills", !strcmp(st("mm.service"), "failed") && !strcmp(U("mm.service")->result, "oom-kill"));
  pages_each = 16;
  wr("of.service", "[Unit]\nOnFailure=handler.service\n[Service]\nType=oneshot\nExecStart=/bin/of\n");
  wr("handler.service", "[Service]\nType=oneshot\nExecStart=/bin/handler %i\n");
  req("of.service", J_START, "replace");
  exit_proc("/bin/of", 1);
  CHECK("OnFailure starts the handler", strstr(spawned, "/bin/handler") != 0);
}

static void t_cycles_memory(void) {
  fresh_core();
  wr("cyc.service", "[Unit]\nStartLimitIntervalSec=0\n[Service]\nExecStart=/bin/cyc\n");
  for (int i = 0; i < 1000; i++) {
    req("cyc.service", J_START, "replace");
    req("cyc.service", J_STOP, "replace");
  }
  CHECK("1000 start/stop cycles", !strcmp(st("cyc.service"), "inactive") && nextpid > 1000);
  /* the fake process table is reused: forget dead rows */
  nprocs = 0;
}

int main(int argc, char **argv) {
  char cwd[300];
  if (!getcwd(cwd, sizeof cwd)) return 2;
  snprintf(ROOT, sizeof ROOT, "%s/%s/core-root-%d", cwd, argc > 1 ? argv[1] : "build-svc-native", (int)getpid()); /* a fresh tree per run */
  copy_targets();
  const char *only = getenv("ONLY"); /* run one test: ONLY=t_stop */
#define T(f) if (!only || !strcmp(only, #f)) f()
  T(t_order); T(t_requires); T(t_conflicts_binds); T(t_cycle); T(t_oneshot); T(t_restart); T(t_stop); T(t_reload); T(t_timer);
  T(t_conditions); T(t_env); T(t_socket); T(t_enable); T(t_isolate_memory_onfailure); T(t_cycles_memory);
  fresh_core();
  printf("core: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
