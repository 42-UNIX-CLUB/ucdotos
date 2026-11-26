// SPDX-License-Identifier: GPL-2.0-or-later
/* The service manager's core. See core.h. Behaviour follows systemd's published
 * documentation (systemd.unit(5), systemd.service(5), systemd.kill(5),
 * systemd.timer(5), systemd.socket(5), systemd.path(5)) for the subset listed in
 * docs/SERVICES.md; what differs on UC.OS is said where it happens. */
#include "core.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIFEXITED
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#endif

enum { S_DEAD, S_CONDITION, S_START_PRE, S_START, S_START_POST, S_RUNNING, S_EXITED, S_RELOAD, S_STOP,
       S_STOP_SIGTERM, S_STOP_SIGKILL, S_STOP_POST, S_FAILED, S_AUTO_RESTART, S_WAITING, S_ELAPSED,
       S_LISTENING, S_ACTIVE, S_MOUNTED_NA };
static const char *const subnames[] = {"dead", "condition", "start-pre", "start", "start-post", "running", "exited",
                                       "reload", "stop", "stop-sigterm", "stop-sigkill", "stop-post", "failed",
                                       "auto-restart", "waiting", "elapsed", "listening", "active", "dead"};

static const struct { const char *n; int s; } signames[] = {
  {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"ILL", 4}, {"ABRT", 6}, {"KILL", 9}, {"USR1", 10}, {"SEGV", 11}, {"USR2", 12},
  {"PIPE", 13}, {"ALRM", 14}, {"TERM", 15}, {"CHLD", 17}, {"CONT", 18}, {"STOP", 19}, {"TSTP", 20}, {"WINCH", 28},
  {"PWR", 30}, {0, 0}};
static int parse_signal(const char *s, int dflt) {
  if (!s || !*s) return dflt;
  if (*s >= '0' && *s <= '9') return atoi(s);
  if (!strncasecmp(s, "SIG", 3)) s += 3;
  for (int i = 0; signames[i].n; i++) if (!strcasecmp(s, signames[i].n)) return signames[i].s;
  return dflt;
}
static const char *signame(int sig) {
  static char b[16];
  for (int i = 0; signames[i].n; i++) if (signames[i].s == sig) { snprintf(b, sizeof b, "SIG%s", signames[i].n); return b; }
  snprintf(b, sizeof b, "%d", sig);
  return b;
}

const char *core_active_str(int a) {
  static const char *const n[] = {"inactive", "activating", "active", "reloading", "deactivating", "failed"};
  return a >= 0 && a <= 5 ? n[a] : "?";
}
const char *core_sub_str(struct unit *u) { return u->sub >= 0 && u->sub <= S_MOUNTED_NA ? subnames[u->sub] : "?"; }
const char *core_load_str(int l) {
  static const char *const n[] = {"stub", "loaded", "not-found", "masked", "bad-setting"};
  return l >= 0 && l <= 4 ? n[l] : "?";
}

void core_log(struct core *c, struct unit *u, int prio, const char *fmt, ...) {
  char m[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof m, fmt, ap);
  va_end(ap);
  c->ops->log(u ? u->name : 0, prio, m);
}

/* ---------- loading ---------- */
static void dep_add_idx(struct dep *d, int i) {
  for (int k = 0; k < d->n; k++) if (d->u[k] == i) return;
  if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 4; d->u = realloc(d->u, d->cap * sizeof *d->u); }
  d->u[d->n++] = i;
}
static const int reverse_of[D_NKINDS] = {
  [D_REQUIRES] = D_REQUIREDBY, [D_WANTS] = D_WANTEDBY, [D_REQUISITE] = D_REQUISITEOF, [D_BINDSTO] = D_BOUNDBY,
  [D_PARTOF] = D_CONSISTSOF, [D_CONFLICTS] = D_CONFLICTEDBY, [D_AFTER] = D_BEFORE, [D_BEFORE] = D_AFTER,
  [D_ONFAILURE] = -1, [D_TRIGGERS] = D_TRIGGEREDBY, [D_PROPRELOAD] = D_RELOADPROPTO,
  [D_REQUIREDBY] = D_REQUIRES, [D_WANTEDBY] = D_WANTS, [D_REQUISITEOF] = D_REQUISITE, [D_BOUNDBY] = D_BINDSTO,
  [D_CONSISTSOF] = D_PARTOF, [D_CONFLICTEDBY] = D_CONFLICTS, [D_TRIGGEREDBY] = D_TRIGGERS, [D_RELOADPROPTO] = D_PROPRELOAD};
static int self_idx(struct core *c, struct unit *u) {
  for (int i = 0; i < c->nu; i++) if (c->u[i] == u) return i;
  return -1;
}
static void dep_add(struct core *c, struct unit *u, int kind, const char *name) {
  int j = core_unit(c, name);
  int i = self_idx(c, u);
  if (j < 0 || i < 0 || j == i) return;
  dep_add_idx(&u->dep[kind], j);
  if (reverse_of[kind] >= 0) dep_add_idx(&c->u[j]->dep[reverse_of[kind]], i);
}
static void dep_list(struct core *c, struct unit *u, int kind, const char *sec, const char *key) {
  const char *v[64];
  int n = svc_getall(&u->f, sec, key, v, 64);
  struct svc_spec sp = {u->name, "/home/user", "user", "42lisboa", "0.1.0", 1000};
  for (int i = 0; i < n; i++) {
    char *x = svc_specifiers(v[i], &sp, 0);
    char **w;
    int nw = svc_words(x, &w);
    free(x);
    for (int k = 0; k < nw; k++) if (svc_name_valid(w[k])) dep_add(c, u, kind, w[k]);
    svc_free_words(w, nw);
  }
}

static char *spec(struct unit *u, const char *v) {
  struct svc_spec sp = {u->name, "/home/user", "user", "42lisboa", "0.1.0", 1000};
  return svc_specifiers(v ? v : "", &sp, 0);
}
static void join_all(struct unit *u, const char *sec, const char *key, char **out) {
  const char *v[64];
  int n = svc_getall(&u->f, sec, key, v, 64);
  size_t len = 1;
  char *parts[64];
  for (int i = 0; i < n; i++) { parts[i] = spec(u, v[i]); len += strlen(parts[i]) + 1; }
  free(*out);
  *out = 0;
  if (!n) return;
  *out = malloc(len);
  (*out)[0] = 0;
  for (int i = 0; i < n; i++) { strcat(*out, parts[i]); strcat(*out, "\n"); free(parts[i]); }
}
static int64_t span(struct unit *u, const char *sec, const char *key, int64_t dflt) {
  const char *v = svc_get(&u->f, sec, key);
  int64_t ms;
  if (!v || !*v) return dflt;
  if (svc_timespan(v, &ms, 1000)) { u->load = L_BAD; return dflt; }
  return ms;
}
static uint64_t bytes(const char *v) {
  if (!v || !*v || !strcmp(v, "infinity")) return 0;
  char *e;
  double d = strtod(v, &e);
  if (*e == '%') return 0; /* percentages of memory: not supported, treated as none */
  switch (*e) {
  case 'K': case 'k': d *= 1024; break;
  case 'M': d *= 1024 * 1024; break;
  case 'G': d *= 1024.0 * 1024 * 1024; break;
  case 'T': d *= 1024.0 * 1024 * 1024 * 1024; break;
  }
  return (uint64_t)d;
}
static void statuses(const char *v, int *arr, int *n, int max) {
  *n = 0;
  if (!v) return;
  char **w;
  int nw = svc_words(v, &w);
  for (int i = 0; i < nw && *n < max; i++) {
    if (w[i][0] >= '0' && w[i][0] <= '9') arr[(*n)++] = atoi(w[i]);
    else arr[(*n)++] = 1000 + parse_signal(w[i], 0); /* a signal name: stored as 1000 + number */
  }
  svc_free_words(w, nw);
}

static void unit_free_config(struct unit *u) {
  svc_file_free(&u->f);
  for (int k = 0; k < D_NKINDS; k++) { free(u->dep[k].u); memset(&u->dep[k], 0, sizeof u->dep[k]); }
  for (int k = 0; k < X_N; k++) { for (int i = 0; i < u->exec[k].n; i++) free(u->exec[k].v[i]); free(u->exec[k].v); u->exec[k].v = 0; u->exec[k].n = 0; }
  free(u->env); free(u->envfiles); free(u->conditions); free(u->rtdirs); free(u->statedirs); free(u->otherdirs); free(u->dropins);
  u->env = u->envfiles = u->conditions = u->rtdirs = u->statedirs = u->otherdirs = u->dropins = 0;
}

static void warn_cb(void *ctx, const char *src, int line, const char *msg) {
  struct core *c = ctx;
  core_log(c, 0, 4, "%s:%d: %s", src, line, msg);
}

static void exec_list(struct unit *u, int k, const char *key) {
  const char *v[32];
  int n = svc_getall(&u->f, "Service", key, v, 32);
  u->exec[k].v = n ? calloc(n, sizeof(char *)) : 0;
  u->exec[k].n = n;
  for (int i = 0; i < n; i++) u->exec[k].v[i] = spec(u, v[i]);
}

static void unit_parse(struct core *c, struct unit *u) {
  const char *v;
  char *x = spec(u, svc_get(&u->f, "Unit", "Description"));
  snprintf(u->desc, sizeof u->desc, "%s", *x ? x : u->name);
  free(x);
  u->default_deps = svc_bool(svc_get(&u->f, "Unit", "DefaultDependencies"), 1);
  u->ignore_on_isolate = svc_bool(svc_get(&u->f, "Unit", "IgnoreOnIsolate"), 0);
  u->allow_isolate = svc_bool(svc_get(&u->f, "Unit", "AllowIsolate"), 0);
  u->refuse_manual_start = svc_bool(svc_get(&u->f, "Unit", "RefuseManualStart"), 0);
  u->refuse_manual_stop = svc_bool(svc_get(&u->f, "Unit", "RefuseManualStop"), 0);
  for (int k = 0; k < 2; k++) {
    const char *a = svc_get(&u->f, "Unit", k ? "FailureAction" : "SuccessAction");
    int act = !a || !strcmp(a, "none") ? 0 : strstr(a, "reboot") ? 2 : (strstr(a, "poweroff") || strstr(a, "halt") || strstr(a, "exit")) ? 1 : 0;
    if (k) u->failure_action = act; else u->success_action = act;
  }
  u->limit_interval_ms = span(u, "Unit", "StartLimitIntervalSec", span(u, "Service", "StartLimitInterval", c->def_limit_interval));
  v = svc_get(&u->f, "Unit", "StartLimitBurst");
  if (!v) v = svc_get(&u->f, "Service", "StartLimitBurst");
  u->limit_burst = v ? atoi(v) : c->def_limit_burst;
  /* conditions and assertions: "Key\tvalue" lines */
  {
    size_t len = 1;
    for (int i = 0; i < u->f.n; i++)
      if (!strcmp(u->f.kv[i].sec, "Unit") && (!strncmp(u->f.kv[i].key, "Condition", 9) || !strncmp(u->f.kv[i].key, "Assert", 6)))
        len += strlen(u->f.kv[i].key) + strlen(u->f.kv[i].val) + 600;
    u->conditions = calloc(1, len);
    for (int i = 0; i < u->f.n; i++)
      if (!strcmp(u->f.kv[i].sec, "Unit") && (!strncmp(u->f.kv[i].key, "Condition", 9) || !strncmp(u->f.kv[i].key, "Assert", 6))) {
        char *e = spec(u, u->f.kv[i].val);
        char line[800];
        snprintf(line, sizeof line, "%s\t%s\n", u->f.kv[i].key, e);
        strcat(u->conditions, line);
        free(e);
      }
  }
  switch (u->type) {
  case SVC_T_SERVICE: {
    exec_list(u, X_PRE, "ExecStartPre");
    exec_list(u, X_START, "ExecStart");
    exec_list(u, X_POST, "ExecStartPost");
    exec_list(u, X_RELOAD, "ExecReload");
    exec_list(u, X_STOP, "ExecStop");
    exec_list(u, X_STOPPOST, "ExecStopPost");
    exec_list(u, X_CONDITION, "ExecCondition");
    v = svc_get(&u->f, "Service", "Type");
    const char *bus = svc_get(&u->f, "Service", "BusName");
    if (!v) u->stype = bus ? ST_DBUS : u->exec[X_START].n ? ST_SIMPLE : ST_ONESHOT;
    else if (!strcmp(v, "simple")) u->stype = ST_SIMPLE;
    else if (!strcmp(v, "exec")) u->stype = ST_EXEC;
    else if (!strcmp(v, "forking")) u->stype = ST_FORKING;
    else if (!strcmp(v, "oneshot")) u->stype = ST_ONESHOT;
    else if (!strcmp(v, "notify") || !strcmp(v, "notify-reload")) u->stype = ST_NOTIFY;
    else if (!strcmp(v, "dbus")) u->stype = ST_DBUS;
    else if (!strcmp(v, "idle")) u->stype = ST_IDLE;
    else { core_log(c, u, 3, "%s: unknown Type=%s", u->path, v); u->load = L_BAD; }
    if (u->stype != ST_ONESHOT && u->exec[X_START].n != 1) {
      core_log(c, u, 3, "%s: Service has %s ExecStart= setting, which is only allowed for Type=oneshot services. Refusing.", u->name,
               u->exec[X_START].n ? "more than one" : "no");
      u->load = L_BAD;
    }
    if (!u->exec[X_START].n && !u->exec[X_STOP].n && u->stype == ST_ONESHOT && !svc_bool(svc_get(&u->f, "Service", "RemainAfterExit"), 0)) {
      core_log(c, u, 3, "%s: Service has no ExecStart=, ExecStop=, or SuccessAction=. Refusing.", u->name);
      u->load = L_BAD;
    }
    v = svc_get(&u->f, "Service", "Restart");
    u->restart = !v || !strcmp(v, "no") ? RS_NO : !strcmp(v, "always") ? RS_ALWAYS : !strcmp(v, "on-success") ? RS_SUCCESS
               : !strcmp(v, "on-failure") ? RS_FAILURE : !strcmp(v, "on-abnormal") ? RS_ABNORMAL : !strcmp(v, "on-abort") ? RS_ABORT
               : !strcmp(v, "on-watchdog") ? RS_WATCHDOG : RS_NO;
    u->restart_ms = span(u, "Service", "RestartSec", c->def_restart);
    u->restart_max_ms = span(u, "Service", "RestartMaxDelaySec", 0);
    v = svc_get(&u->f, "Service", "RestartSteps");
    u->restart_steps = v ? atoi(v) : 0;
    int64_t both = span(u, "Service", "TimeoutSec", -1);
    u->tstart_ms = span(u, "Service", "TimeoutStartSec", both >= 0 ? both : (u->stype == ST_ONESHOT ? SVC_INFINITY : c->def_tstart));
    u->tstop_ms = span(u, "Service", "TimeoutStopSec", both >= 0 ? both : c->def_tstop);
    if (u->tstart_ms == 0) u->tstart_ms = SVC_INFINITY;
    if (u->tstop_ms == 0) u->tstop_ms = SVC_INFINITY;
    u->remain = svc_bool(svc_get(&u->f, "Service", "RemainAfterExit"), 0);
    v = svc_get(&u->f, "Service", "KillMode");
    u->killmode = !v || !strcmp(v, "control-group") ? KM_CGROUP : !strcmp(v, "mixed") ? KM_MIXED : !strcmp(v, "process") ? KM_PROCESS : KM_NONE;
    u->killsig = parse_signal(svc_get(&u->f, "Service", "KillSignal"), 15);
    u->finalsig = parse_signal(svc_get(&u->f, "Service", "FinalKillSignal"), 9);
    u->sendkill = svc_bool(svc_get(&u->f, "Service", "SendSIGKILL"), 1);
    u->guess = svc_bool(svc_get(&u->f, "Service", "GuessMainPID"), 1);
    join_all(u, "Service", "Environment", &u->env);
    join_all(u, "Service", "EnvironmentFile", &u->envfiles);
    x = spec(u, svc_get(&u->f, "Service", "WorkingDirectory"));
    snprintf(u->workdir, sizeof u->workdir, "%s", x); free(x);
    x = spec(u, svc_get(&u->f, "Service", "PIDFile"));
    snprintf(u->pidfile, sizeof u->pidfile, "%s", x); free(x);
    for (int k = 0; k < 2; k++) {
      v = svc_get(&u->f, "Service", k ? "StandardError" : "StandardOutput");
      int o = !v ? (k ? -1 : OUT_JOURNAL) : !strcmp(v, "null") ? OUT_NULL : !strcmp(v, "inherit") || !strcmp(v, "tty") || !strcmp(v, "console") ? OUT_INHERIT
            : !strncmp(v, "file:", 5) || !strncmp(v, "truncate:", 9) ? OUT_FILE : !strncmp(v, "append:", 7) ? OUT_APPEND
            : !strcmp(v, "journal+console") || !strcmp(v, "kmsg+console") ? OUT_CONSOLE_TOO : !strcmp(v, "socket") ? OUT_SOCKET : OUT_JOURNAL;
      if (k) u->err = o; else u->out = o;
      char *path = k ? u->errpath : u->outpath;
      path[0] = 0;
      if (o == OUT_FILE || o == OUT_APPEND) { x = spec(u, strchr(v, ':') + 1); snprintf(path, 200, "%s", x); free(x); }
    }
    v = svc_get(&u->f, "Service", "StandardInput");
    u->in = v && !strcmp(v, "socket") ? OUT_SOCKET : v && (!strcmp(v, "tty") || !strcmp(v, "tty-force")) ? OUT_INHERIT : OUT_NULL;
    statuses(svc_get(&u->f, "Service", "SuccessExitStatus"), u->succ_status, &u->nsucc, 8);
    statuses(svc_get(&u->f, "Service", "RestartPreventExitStatus"), u->prevent_status, &u->nprevent, 8);
    statuses(svc_get(&u->f, "Service", "RestartForceExitStatus"), u->force_status, &u->nforce, 8);
    join_all(u, "Service", "RuntimeDirectory", &u->rtdirs);
    join_all(u, "Service", "StateDirectory", &u->statedirs);
    {
      char *a = 0, *b = 0, *d = 0;
      join_all(u, "Service", "CacheDirectory", &a);
      join_all(u, "Service", "LogsDirectory", &b);
      join_all(u, "Service", "ConfigurationDirectory", &d);
      size_t l = (a ? strlen(a) + 64 * 4 : 0) + (b ? strlen(b) + 64 * 4 : 0) + (d ? strlen(d) + 64 * 4 : 0) + 1;
      u->otherdirs = calloc(1, l);
      /* each word gets its base directory */
      const char *bases[3] = {"/var/cache/", "/var/log/", "/etc/"};
      char *lists[3] = {a, b, d};
      for (int k = 0; k < 3; k++) {
        if (!lists[k]) continue;
        char **w;
        int nw = svc_words(lists[k], &w);
        for (int i = 0; i < nw; i++) { strcat(u->otherdirs, bases[k]); strcat(u->otherdirs, w[i]); strcat(u->otherdirs, "\n"); }
        svc_free_words(w, nw);
        free(lists[k]);
      }
    }
    v = svc_get(&u->f, "Service", "RuntimeDirectoryPreserve");
    u->rt_preserve = v && (svc_bool(v, 0) || !strcmp(v, "restart"));
    v = svc_get(&u->f, "Service", "NotifyAccess");
    u->notify_access = v && !strcmp(v, "none") ? 0 : 1;
    u->mem_high = bytes(svc_get(&u->f, "Service", "MemoryHigh"));
    u->mem_max = bytes(svc_get(&u->f, "Service", "MemoryMax"));
    if (!u->mem_max) u->mem_max = bytes(svc_get(&u->f, "Service", "MemoryLimit"));
    break;
  }
  case SVC_T_TIMER: {
    u->on_boot = span(u, "Timer", "OnBootSec", -1);
    u->on_startup = span(u, "Timer", "OnStartupSec", -1);
    u->on_active = span(u, "Timer", "OnActiveSec", -1);
    u->on_unit_active = span(u, "Timer", "OnUnitActiveSec", -1);
    u->on_unit_inactive = span(u, "Timer", "OnUnitInactiveSec", -1);
    u->rand_delay = span(u, "Timer", "RandomizedDelaySec", 0);
    const char *cv[8];
    int n = svc_getall(&u->f, "Timer", "OnCalendar", cv, 8);
    u->ncal = 0;
    for (int i = 0; i < n && u->ncal < 4; i++) {
      if (svc_cal_parse(cv[i], &u->cal[u->ncal]) == 0) u->ncal++;
      else { core_log(c, u, 3, "%s: OnCalendar=%s is not understood", u->name, cv[i]); u->load = L_BAD; }
    }
    v = svc_get(&u->f, "Timer", "Unit");
    char tgt[128];
    if (v) snprintf(tgt, sizeof tgt, "%s", v);
    else { snprintf(tgt, sizeof tgt, "%s", u->name); strcpy(strrchr(tgt, '.'), ".service"); }
    dep_add(c, u, D_TRIGGERS, tgt);
    dep_add(c, u, D_BEFORE, tgt);
    break;
  }
  case SVC_T_SOCKET: {
    const char *lv[8];
    int n = svc_getall(&u->f, "Socket", "ListenStream", lv, 8);
    u->nlisten = 0;
    for (int i = 0; i < n && u->nlisten < 4; i++) { x = spec(u, lv[i]); snprintf(u->listen[u->nlisten++], 108, "%s", x); free(x); }
    static const char *const others[] = {"ListenDatagram", "ListenSequentialPacket", "ListenFIFO", "ListenSpecial", "ListenNetlink",
                                         "ListenMessageQueue", "ListenUSBFunction", 0};
    for (int k = 0; others[k]; k++)
      if (svc_get(&u->f, "Socket", others[k]) && u->nlisten < 4) snprintf(u->listen[u->nlisten++], 108, "!%s", others[k]);
    u->accept = svc_bool(svc_get(&u->f, "Socket", "Accept"), 0);
    v = svc_get(&u->f, "Socket", "Service");
    if (v) snprintf(u->svcname, sizeof u->svcname, "%s", v);
    else {
      snprintf(u->svcname, sizeof u->svcname, "%s", u->name);
      strcpy(strrchr(u->svcname, '.'), u->accept ? "@.service" : ".service");
    }
    for (int i = 0; i < 4; i++) u->lfd[i] = -1;
    if (!u->accept) { dep_add(c, u, D_TRIGGERS, u->svcname); dep_add(c, u, D_BEFORE, u->svcname); }
    break;
  }
  case SVC_T_PATH: {
    static const char *const kinds[] = {"PathExists", "PathExistsGlob", "PathChanged", "PathModified", "DirectoryNotEmpty", 0};
    u->nwatch = 0;
    for (int k = 0; kinds[k]; k++) {
      const char *pv[4];
      int n = svc_getall(&u->f, "Path", kinds[k], pv, 4);
      for (int i = 0; i < n && u->nwatch < 4; i++) {
        x = spec(u, pv[i]);
        snprintf(u->watch[u->nwatch], 200, "%s", x);
        free(x);
        u->watchkind[u->nwatch] = k;
        u->watch_mtime[u->nwatch] = -2;
        u->nwatch++;
      }
    }
    u->makedir = svc_bool(svc_get(&u->f, "Path", "MakeDirectory"), 0);
    v = svc_get(&u->f, "Path", "Unit");
    char tgt[128];
    if (v) snprintf(tgt, sizeof tgt, "%s", v);
    else { snprintf(tgt, sizeof tgt, "%s", u->name); strcpy(strrchr(tgt, '.'), ".service"); }
    dep_add(c, u, D_TRIGGERS, tgt);
    dep_add(c, u, D_BEFORE, tgt);
    break;
  }
  default: break;
  }
}

static void unit_load(struct core *c, struct unit *u) {
  char path[512];
  int root = 0;
  svc_file_free(&u->f);
  int e = svc_load(&c->roots, u->name, &u->f, path, sizeof path, &root, warn_cb, c);
  u->root = root;
  snprintf(u->path, sizeof u->path, "%s", e == 0 || e == 1 ? path : "");
  if (e == 1) { u->load = L_MASKED; snprintf(u->desc, sizeof u->desc, "%s", u->name); return; }
  if (e) {
    u->load = L_NOTFOUND;
    snprintf(u->desc, sizeof u->desc, "%s", u->name);
    return;
  }
  u->load = L_LOADED;
  c->nodes_loaded++;
  /* drop-in list, for status */
  size_t dl = 1;
  for (int i = 1; i < u->f.nsrc; i++) dl += strlen(u->f.srcs[i]) + 1;
  u->dropins = calloc(1, dl);
  for (int i = 1; i < u->f.nsrc; i++) { strcat(u->dropins, u->f.srcs[i]); strcat(u->dropins, "\n"); }
  if (u->type == SVC_T_DEVICE || u->type == SVC_T_MOUNT || u->type == SVC_T_SWAP || u->type == SVC_T_AUTOMOUNT || u->type == SVC_T_SCOPE) {
    core_log(c, u, 4, "%s: %s units are not supported on UC.OS; the unit is treated as not found", u->name, svc_type_name(u->type));
    u->load = L_NOTFOUND;
    return;
  }
  unit_parse(c, u);
  if (u->load == L_BAD) return;
  dep_list(c, u, D_REQUIRES, "Unit", "Requires");
  dep_list(c, u, D_WANTS, "Unit", "Wants");
  dep_list(c, u, D_REQUISITE, "Unit", "Requisite");
  dep_list(c, u, D_BINDSTO, "Unit", "BindsTo");
  dep_list(c, u, D_PARTOF, "Unit", "PartOf");
  dep_list(c, u, D_CONFLICTS, "Unit", "Conflicts");
  dep_list(c, u, D_AFTER, "Unit", "After");
  dep_list(c, u, D_BEFORE, "Unit", "Before");
  dep_list(c, u, D_ONFAILURE, "Unit", "OnFailure");
  dep_list(c, u, D_PROPRELOAD, "Unit", "ReloadPropagatedFrom");
  {
    const char *v[16];
    int n = svc_getall(&u->f, "Unit", "PropagatesReloadTo", v, 16);
    for (int i = 0; i < n; i++) {
      char **w;
      int nw = svc_words(v[i], &w);
      for (int k = 0; k < nw; k++) {
        int j = core_unit(c, w[k]);
        if (j >= 0) { int me = self_idx(c, u); dep_add_idx(&c->u[j]->dep[D_PROPRELOAD], me); dep_add_idx(&u->dep[D_RELOADPROPTO], j); }
      }
      svc_free_words(w, nw);
    }
  }
  char **names;
  int n = svc_dirdeps(&c->roots, u->name, "wants", &names);
  for (int i = 0; i < n; i++) { dep_add(c, u, D_WANTS, names[i]); free(names[i]); }
  free(names);
  n = svc_dirdeps(&c->roots, u->name, "requires", &names);
  for (int i = 0; i < n; i++) { dep_add(c, u, D_REQUIRES, names[i]); free(names[i]); }
  free(names);
  char tmpl[128];
  if (svc_name_split(u->name, tmpl, sizeof tmpl, 0, 0) == 1) {
    n = svc_dirdeps(&c->roots, tmpl, "wants", &names);
    for (int i = 0; i < n; i++) { dep_add(c, u, D_WANTS, names[i]); free(names[i]); }
    free(names);
  }
  if (u->default_deps) {
    switch (u->type) {
    case SVC_T_SERVICE: case SVC_T_SOCKET: case SVC_T_TIMER: case SVC_T_PATH:
      dep_add(c, u, D_REQUIRES, "sysinit.target");
      dep_add(c, u, D_AFTER, "sysinit.target");
      if (u->type == SVC_T_SERVICE) dep_add(c, u, D_AFTER, "basic.target");
      if (u->type == SVC_T_SOCKET) dep_add(c, u, D_BEFORE, "sockets.target");
      if (u->type == SVC_T_TIMER) { dep_add(c, u, D_BEFORE, "timers.target"); dep_add(c, u, D_AFTER, "time-set.target"); }
      if (u->type == SVC_T_PATH) dep_add(c, u, D_BEFORE, "paths.target");
      dep_add(c, u, D_CONFLICTS, "shutdown.target");
      dep_add(c, u, D_BEFORE, "shutdown.target");
      break;
    case SVC_T_TARGET: {
      dep_add(c, u, D_CONFLICTS, "shutdown.target");
      dep_add(c, u, D_BEFORE, "shutdown.target");
      int me = self_idx(c, u);
      for (int k = 0; k < 2; k++) {
        struct dep *d = &u->dep[k ? D_WANTS : D_REQUIRES];
        for (int i = 0; i < d->n; i++) {
          struct unit *t = c->u[d->u[i]];
          if (!t) continue;
          /* a target is ordered after what it pulls in, unless that unit says otherwise */
          int before_me = 0;
          for (int q = 0; q < t->dep[D_AFTER].n; q++) if (t->dep[D_AFTER].u[q] == me) before_me = 1;
          if (!before_me) { dep_add_idx(&u->dep[D_AFTER], d->u[i]); dep_add_idx(&t->dep[D_BEFORE], me); }
        }
      }
      break;
    }
    default: break;
    }
  }
}

int core_find(struct core *c, const char *name) {
  for (int i = 0; i < c->nu; i++) if (c->u[i] && !strcmp(c->u[i]->name, name)) return i;
  return -1;
}

int core_unit(struct core *c, const char *name) {
  int i = core_find(c, name);
  if (i >= 0) return i;
  if (!svc_name_valid(name)) return -1;
  /* an alias resolves to the unit it names */
  char path[512], real[128];
  int root;
  if (svc_find(&c->roots, name, path, sizeof path, &root, real, sizeof real) == 0 && strcmp(real, name)) {
    i = core_find(c, real);
    if (i >= 0) return i;
    name = real;
  }
  int slot = -1;
  for (int k = 0; k < c->nu; k++) if (!c->u[k]) { slot = k; break; }
  if (slot < 0) { if (c->nu >= MAXUNITS) return -1; slot = c->nu++; }
  struct unit *u = calloc(1, sizeof *u);
  snprintf(u->name, sizeof u->name, "%s", name);
  u->type = svc_name_type(name);
  u->job = -1; u->deadline = -1; u->restart_at = -1; u->next_elapse = -1; u->outfd = -1; u->last_trigger = -1;
  u->next_real = -1;
  strcpy(u->result, "success");
  c->u[slot] = u;
  unit_load(c, u);
  return slot;
}

/* ---------- conditions ---------- */
static int glob_match(const char *p, const char *s) {
  for (; *p; p++, s++) {
    if (*p == '*') { for (const char *t = s;; t++) { if (glob_match(p + 1, t)) return 1; if (!*t) return 0; } }
    if (!*s || (*p != '?' && *p != *s)) return 0;
  }
  return !*s;
}
static int glob_exists(struct core *c, const char *pat) {
  /* only the last path component may hold wildcards; enough for the units surveyed */
  char dir[256];
  const char *sl = strrchr(pat, '/');
  if (!sl) return 0;
  snprintf(dir, sizeof dir, "%.*s", (int)(sl - pat), pat);
  if (!strpbrk(sl + 1, "*?[")) return c->ops->file_exists(pat, 0) == 0;
  (void)glob_match;
  return c->ops->file_exists(dir, 1) == 0; /* coarse: the directory exists */
}
/* 1 holds, 0 fails; *assert_failed set if an Assert failed */
static int check_conditions(struct core *c, struct unit *u, int *assert_failed, char *which, size_t wl) {
  *assert_failed = 0;
  if (!u->conditions || !*u->conditions) return 1;
  int any_trigger = 0, trigger_ok = 0, ok = 1;
  char *copy = strdup(u->conditions), *save = 0;
  for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    const char *key = line, *v = tab + 1;
    int isassert = !strncmp(key, "Assert", 6);
    const char *kind = key + (isassert ? 6 : 9);
    int trig = 0, neg = 0;
    if (*v == '|') { trig = 1; v++; }
    if (*v == '!') { neg = 1; v++; }
    int r = 1;
    if (!*v) continue;
    char hv[400];
    if (*v == '/') { svc_host_path(&c->roots, u->root, v, hv, sizeof hv); v = hv; }
    if (!strcmp(kind, "PathExists")) r = c->ops->file_exists(v, 0) == 0;
    else if (!strcmp(kind, "PathExistsGlob")) r = glob_exists(c, v);
    else if (!strcmp(kind, "PathIsDirectory")) r = c->ops->file_exists(v, 1) == 0;
    else if (!strcmp(kind, "FileIsExecutable")) r = c->ops->file_exists(v, 2) == 0;
    else if (!strcmp(kind, "DirectoryNotEmpty")) r = c->ops->file_exists(v, 3) == 0;
    else if (!strcmp(kind, "FileNotEmpty")) r = c->ops->file_exists(v, 4) == 0;
    else if (!strcmp(kind, "PathIsSymbolicLink")) r = c->ops->file_exists(v, 5) == 0;
    else if (!strcmp(kind, "PathIsReadWrite")) r = c->ops->file_exists(v, 6) == 0;
    else if (!strcmp(kind, "Virtualization")) r = !strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0"); /* assumed: not virtualised */
    else if (!strcmp(kind, "ACPower")) r = 1;          /* no power supply is known: systemd's rule makes both values hold... */
    else if (!strcmp(kind, "Capability")) r = 1;       /* no capability model: the one user may do everything */
    else if (!strcmp(kind, "FirstBoot")) r = svc_bool(v, 1) == (c->ops->file_exists("/etc/machine-id", 4) != 0);
    else if (!strcmp(kind, "CPUs")) { const char *p = v; while (*p == '<' || *p == '>' || *p == '=' || *p == '!') p++; int n = atoi(p);
      r = !strncmp(v, ">=", 2) ? 1 >= n : !strncmp(v, "<=", 2) ? 1 <= n : *v == '>' ? 1 > n : *v == '<' ? 1 < n : !strncmp(v, "!=", 2) ? 1 != n : 1 == n; }
    else if (!strcmp(kind, "Host")) r = !strcmp(v, "42lisboa");
    else if (!strcmp(kind, "KernelCommandLine") || !strcmp(kind, "Security") || !strcmp(kind, "NeedsUpdate") ||
             !strcmp(kind, "ControlGroupController")) r = 0;
    else r = 1; /* not understood: holds (logged by `systemd-analyze`-style checks, not here) */
    if (!strcmp(kind, "ACPower") && neg) { /* ConditionACPower=false: holds only if a connector is known: never */ }
    if (!strcmp(kind, "ACPower") && !svc_bool(v - neg, 1)) r = 0;
    if (neg) r = !r;
    if (isassert && !r) { *assert_failed = 1; snprintf(which, wl, "%s=%s%s%s", key, trig ? "|" : "", neg ? "!" : "", v); ok = 0; break; }
    if (trig) { any_trigger = 1; if (r) trigger_ok = 1; }
    else if (!r) { snprintf(which, wl, "%s=%s%s", key, neg ? "!" : "", v); ok = 0; break; }
  }
  free(copy);
  if (ok && any_trigger && !trigger_ok) { snprintf(which, wl, "a triggering condition"); ok = 0; }
  return ok;
}

/* ---------- jobs ---------- */
static int startish(int t) { return t != J_STOP; }
static void job_finish(struct core *c, int j, const char *result);

static int new_job(struct core *c, int unit, int type) {
  for (int j = 0; j < MAXJOBS; j++)
    if (!c->jobs[j].used) {
      memset(&c->jobs[j], 0, sizeof c->jobs[j]);
      c->jobs[j].used = 1; c->jobs[j].unit = unit; c->jobs[j].type = type; c->jobs[j].t = c->ops->now();
      c->u[unit]->job = j;
      return j;
    }
  return -1;
}
static int in_dep(struct unit *u, int kind, int idx) {
  for (int i = 0; i < u->dep[kind].n; i++) if (u->dep[kind].u[i] == idx) return 1;
  return 0;
}
/* Does job a have to wait for job b? (systemd.unit(5), After=/Before=) */
static int waits_for(struct core *c, int ua, int ta, int ub, int tb) {
  struct unit *A = c->u[ua];
  int after = in_dep(A, D_AFTER, ub), before = in_dep(A, D_BEFORE, ub);
  if (!after && !before) return 0;
  int sa = startish(ta), sb = startish(tb);
  if (sa != sb) return sa; /* a stop goes before a start, whatever the direction */
  if (sa) return after;    /* both start: the After= side waits */
  return before;           /* both stop: inverse order */
}

struct tx { int unit[MAXJOBS], type[MAXJOBS], ess[MAXJOBS], n; char err[200]; int anchor; };

static int tx_find(struct tx *t, int u) { for (int i = 0; i < t->n; i++) if (t->unit[i] == u) return i; return -1; }
static int active_ish(struct unit *u) { return u->active == A_ACTIVE || u->active == A_ACTIVATING || u->active == A_RELOADING || u->active == A_DEACTIVATING || u->sub == S_AUTO_RESTART; }

static int tx_add(struct core *c, struct tx *t, int ui, int type, int ess, int depth) {
  struct unit *u = c->u[ui];
  if (depth > 64) return 0;
  int k = tx_find(t, ui);
  if (k >= 0) {
    if (startish(t->type[k]) == startish(type)) { t->ess[k] |= ess; return 0; }
    if (ess && t->ess[k]) { snprintf(t->err, sizeof t->err, "Transaction contains conflicting jobs '%s' and '%s' for %s. Probably contradicting requirement dependencies configured.",
                                     startish(t->type[k]) ? "start" : "stop", startish(type) ? "start" : "stop", u->name); return -1; }
    if (!ess) return 0; /* keep what is there */
    t->type[k] = type; t->ess[k] = ess;
  } else {
    if (t->n >= MAXJOBS) { snprintf(t->err, sizeof t->err, "Too many jobs."); return -1; }
    if (startish(type) && type != J_VERIFY && u->load != L_LOADED) {
      if (!ess) return 0;
      snprintf(t->err, sizeof t->err, u->load == L_MASKED ? "Unit %s is masked." : u->load == L_BAD ? "Unit %s has a bad unit file setting." : "Unit %s not found.", u->name);
      return -1;
    }
    if (depth > 0 && type == J_START && u->active == A_ACTIVE && u->job < 0) return 0;   /* nothing to do, and nothing to pull */
    if (depth > 0 && type == J_STOP && !active_ish(u) && u->job < 0) return 0;
    k = t->n++;
    t->unit[k] = ui; t->type[k] = type; t->ess[k] = ess;
  }
  if (type == J_START || type == J_RESTART) {
    for (int i = 0; i < u->dep[D_REQUIRES].n; i++) if (tx_add(c, t, u->dep[D_REQUIRES].u[i], J_START, ess, depth + 1)) return -1;
    for (int i = 0; i < u->dep[D_BINDSTO].n; i++) if (tx_add(c, t, u->dep[D_BINDSTO].u[i], J_START, ess, depth + 1)) return -1;
    for (int i = 0; i < u->dep[D_WANTS].n; i++) if (tx_add(c, t, u->dep[D_WANTS].u[i], J_START, 0, depth + 1)) return -1;
    for (int i = 0; i < u->dep[D_REQUISITE].n; i++) if (tx_add(c, t, u->dep[D_REQUISITE].u[i], J_VERIFY, ess, depth + 1)) return -1;
    for (int kk = 0; kk < 2; kk++) {
      struct dep *d = &u->dep[kk ? D_CONFLICTEDBY : D_CONFLICTS];
      for (int i = 0; i < d->n; i++) {
        struct unit *o = c->u[d->u[i]];
        if (o && (active_ish(o) || o->job >= 0)) if (tx_add(c, t, d->u[i], J_STOP, ess, depth + 1)) return -1;
      }
    }
  }
  if (type == J_STOP) {
    static const int kinds[] = {D_REQUIREDBY, D_BOUNDBY, D_CONSISTSOF};
    for (int q = 0; q < 3; q++)
      for (int i = 0; i < u->dep[kinds[q]].n; i++) {
        struct unit *o = c->u[u->dep[kinds[q]].u[i]];
        if (o && (active_ish(o) || o->job >= 0)) if (tx_add(c, t, u->dep[kinds[q]].u[i], J_STOP, ess, depth + 1)) return -1;
      }
  }
  if (type == J_RESTART) {
    static const int kinds[] = {D_REQUIREDBY, D_BOUNDBY, D_CONSISTSOF};
    for (int q = 0; q < 3; q++)
      for (int i = 0; i < u->dep[kinds[q]].n; i++) {
        struct unit *o = c->u[u->dep[kinds[q]].u[i]];
        if (o && o->active == A_ACTIVE && tx_find(t, u->dep[kinds[q]].u[i]) < 0) tx_add(c, t, u->dep[kinds[q]].u[i], J_TRYRESTART, 0, depth + 1);
      }
  }
  if (type == J_RELOAD)
    for (int i = 0; i < u->dep[D_RELOADPROPTO].n; i++) {
      struct unit *o = c->u[u->dep[D_RELOADPROPTO].u[i]];
      if (o && o->active == A_ACTIVE) tx_add(c, t, u->dep[D_RELOADPROPTO].u[i], J_RELOAD, 0, depth + 1);
    }
  return 0;
}

/* Ordering cycle check over the jobs of a transaction. On a cycle,
 * log it the way systemd does, drop a job that was only wanted, and try again. */
static int tx_cycles(struct core *c, struct tx *t) {
  for (int round = 0; round < 32; round++) {
    /* nodes: pending jobs (not in t) and t's entries */
    int nu[MAXJOBS * 2], ty[MAXJOBS * 2], txi[MAXJOBS * 2], n = 0;
    /* as in systemd, only the jobs of this transaction (queued jobs of earlier ones that
     * end up waiting on each other are handled in core_run) */
    for (int i = 0; i < t->n; i++) { nu[n] = t->unit[i]; ty[n] = t->type[i]; txi[n] = i; n++; }
    int color[MAXJOBS * 2] = {0}, stack[MAXJOBS * 2], it[MAXJOBS * 2], sp;
    int cyc_from = -1, cyc_to = -1;
    for (int s = 0; s < n && cyc_from < 0; s++) {
      if (color[s]) continue;
      sp = 0; stack[sp] = s; it[sp] = 0; color[s] = 1;
      while (sp >= 0 && cyc_from < 0) {
        int v = stack[sp];
        if (it[sp] >= n) { color[v] = 2; sp--; continue; }
        int w = it[sp]++;
        if (w == v || !waits_for(c, nu[v], ty[v], nu[w], ty[w])) continue;
        if (color[w] == 1) { cyc_from = w; cyc_to = v; break; }
        if (!color[w]) { color[w] = 1; sp++; stack[sp] = w; it[sp] = 0; }
      }
      if (cyc_from >= 0) {
        /* the cycle is stack[k..sp] where stack[k] == cyc_from */
        int k = 0;
        while (stack[k] != cyc_from) k++;
        core_log(c, c->u[nu[cyc_from]], 4, "%s: Found ordering cycle on %s/%s", c->u[nu[cyc_from]]->name, c->u[nu[cyc_from]]->name, startish(ty[cyc_from]) ? "start" : "stop");
        for (int q = k + 1; q <= sp; q++)
          core_log(c, c->u[nu[cyc_from]], 4, "%s: Found dependency on %s/%s", c->u[nu[cyc_from]]->name, c->u[nu[stack[q]]]->name, startish(ty[stack[q]]) ? "start" : "stop");
        core_log(c, c->u[nu[cyc_from]], 4, "%s: Found dependency on %s/%s", c->u[nu[cyc_from]]->name, c->u[nu[cyc_from]]->name, startish(ty[cyc_from]) ? "start" : "stop");
        (void)cyc_to;
        /* drop a job that is not essential and not the anchor */
        int victim = -1;
        for (int q = k; q <= sp; q++) { int x = txi[stack[q]]; if (x >= 0 && !t->ess[x] && x != t->anchor) { victim = x; break; } }
        if (victim < 0) {
          core_log(c, c->u[nu[cyc_from]], 3, "%s: Unable to break cycle starting with %s/%s", c->u[nu[cyc_from]]->name, c->u[nu[cyc_from]]->name, startish(ty[cyc_from]) ? "start" : "stop");
          snprintf(c->cycle_msg, sizeof c->cycle_msg, "Transaction order is cyclic. See system logs for details.");
          return -1;
        }
        core_log(c, c->u[nu[cyc_from]], 4, "%s: Job %s/%s deleted to break ordering cycle starting with %s/%s", c->u[nu[cyc_from]]->name,
                 c->u[t->unit[victim]]->name, startish(t->type[victim]) ? "start" : "stop", c->u[nu[cyc_from]]->name, startish(ty[cyc_from]) ? "start" : "stop");
        if (victim < t->anchor) t->anchor--;
        for (int q = victim; q < t->n - 1; q++) { t->unit[q] = t->unit[q + 1]; t->type[q] = t->type[q + 1]; t->ess[q] = t->ess[q + 1]; }
        t->n--;
      }
    }
    if (cyc_from < 0) return 0;
  }
  return -1;
}

static const char *const jtnames[] = {"start", "stop", "restart", "reload", "try-restart", "verify-active"};

int core_request(struct core *c, const char *name, int type, const char *mode, int client, char *msg, int ml) {
  msg[0] = 0;
  int ui = core_unit(c, name);
  if (ui < 0) { snprintf(msg, ml, "Unit name %s is not valid.", name); return -EINVAL; }
  struct unit *u = c->u[ui];
  if (u->load == L_NOTFOUND && type != J_STOP) { snprintf(msg, ml, "Unit %s not found.", name); return -ENOENT; }
  if (u->load == L_MASKED && type != J_STOP) { snprintf(msg, ml, "Unit %s is masked.", name); return -ESHUTDOWN; }
  if (u->load == L_BAD && type != J_STOP) { snprintf(msg, ml, "Unit %s has a bad unit file setting.", name); return -ENOEXEC; }
  if (type == J_RELOAD && u->type == SVC_T_SERVICE && !u->exec[X_RELOAD].n) {
    snprintf(msg, ml, "Job type reload is not applicable for unit %s.", name);
    return -ENOEXEC;
  }
  struct tx *t = calloc(1, sizeof *t);
  t->anchor = 0;
  int isolate = mode && !strcmp(mode, "isolate");
  if (isolate && !u->allow_isolate) { snprintf(msg, ml, "Operation refused, unit may not be isolated."); free(t); return -EPERM; }
  if (tx_add(c, t, ui, type, 1, 0) < 0) { snprintf(msg, ml, "%s", t->err); free(t); return -EIO; }
  if (isolate) {
    /* keep everything the target pulls in, active or not; stop the rest */
    char *keep = calloc(c->nu + 1, 1);
    int stack[MAXUNITS], sp = 0;
    stack[sp++] = ui; keep[ui] = 1;
    while (sp) {
      struct unit *o = c->u[stack[--sp]];
      static const int kinds[] = {D_REQUIRES, D_WANTS, D_BINDSTO, D_REQUISITE};
      for (int q = 0; q < 4; q++)
        for (int i = 0; i < o->dep[kinds[q]].n; i++) {
          int x = o->dep[kinds[q]].u[i];
          if (!keep[x]) { keep[x] = 1; stack[sp++] = x; }
        }
    }
    for (int i = 0; i < c->nu; i++) {
      struct unit *o = c->u[i];
      if (!o || keep[i] || !active_ish(o) || tx_find(t, i) >= 0 || o->ignore_on_isolate) continue;
      tx_add(c, t, i, J_STOP, 0, 1);
    }
    free(keep);
  }
  c->cycle_msg[0] = 0;
  if (tx_cycles(c, t) < 0) { snprintf(msg, ml, "%s", c->cycle_msg); free(t); return -EDEADLK; }
  int fail_mode = mode && !strcmp(mode, "fail");
  if (fail_mode)
    for (int i = 0; i < t->n; i++) {
      struct unit *o = c->u[t->unit[i]];
      if (o->job >= 0 && startish(c->jobs[o->job].type) != startish(t->type[i])) {
        snprintf(msg, ml, "Transaction for %s/%s is destructive (%s has '%s' job queued, but '%s' is included in transaction).",
                 u->name, jtnames[type], o->name, jtnames[c->jobs[o->job].type], jtnames[t->type[i]]);
        free(t);
        return -EBUSY;
      }
    }
  for (int i = 0; i < t->n; i++) {
    struct unit *o = c->u[t->unit[i]];
    int j = o->job;
    if (j >= 0 && c->jobs[j].type != t->type[i]) {
      if (startish(c->jobs[j].type) == startish(t->type[i]) && !(t->type[i] == J_RESTART && c->jobs[j].type == J_START && !c->jobs[j].running)) {
        /* compatible: keep the queued job */
      } else if (c->jobs[j].running && startish(c->jobs[j].type) == startish(t->type[i])) {
      } else {
        job_finish(c, j, "canceled");
        j = -1;
      }
    }
    if (j < 0) j = new_job(c, t->unit[i], t->type[i]);
    if (j < 0) continue;
    c->jobs[j].essential |= t->ess[i];
    if (i == t->anchor && client >= 0 && c->jobs[j].nwait < 8) c->jobs[j].waiters[c->jobs[j].nwait++] = client;
  }
  free(t);
  return 0;
}

static void job_finish(struct core *c, int j, const char *result) {
  struct job *jb = &c->jobs[j];
  if (!jb->used) return;
  int ui = jb->unit, type = jb->type;
  struct unit *u = c->u[ui];
  int waiters[8], nw = jb->nwait;
  memcpy(waiters, jb->waiters, sizeof waiters);
  jb->used = 0;
  if (u && u->job == j) u->job = -1;
  for (int i = 0; i < nw; i++) if (c->job_done) c->job_done(c, waiters[i], u ? u->name : "?", type, result);
  if (nw == 0 && c->job_done) c->job_done(c, -1, u ? u->name : "?", type, result);
  /* a failed start fails the start of what requires it and is ordered after it */
  if (startish(type) && strcmp(result, "done") && strcmp(result, "skipped") && u) {
    for (int k = 0; k < MAXJOBS; k++) {
      if (!c->jobs[k].used || !startish(c->jobs[k].type) || c->jobs[k].running) continue;
      struct unit *o = c->u[c->jobs[k].unit];
      if ((in_dep(o, D_REQUIRES, ui) || in_dep(o, D_BINDSTO, ui) || in_dep(o, D_REQUISITE, ui)) && in_dep(o, D_AFTER, ui)) {
        core_log(c, o, 4, "Dependency failed for %s.", o->desc);
        core_log(c, o, 4, "%s: Job %s/%s failed with result 'dependency'.", o->name, o->name, jtnames[c->jobs[k].type]);
        job_finish(c, k, "dependency");
      }
    }
  }
}

/* ---------- service state machine ---------- */
static void set_state(struct core *c, struct unit *u, int active, int sub);
static void svc_enter_stop(struct core *c, struct unit *u, const char *result);
static void svc_enter_dead(struct core *c, struct unit *u, const char *result);
static int run_next(struct core *c, struct unit *u, int kind, int idx);

static int64_t restart_delay(struct unit *u) {
  if (u->restart_steps <= 0 || u->restart_max_ms <= u->restart_ms || u->restart_ms <= 0) return u->restart_ms;
  int n = u->nrestarts < u->restart_steps ? u->nrestarts : u->restart_steps;
  return (int64_t)(u->restart_ms * pow((double)u->restart_max_ms / (double)u->restart_ms, (double)n / u->restart_steps));
}

static void trigger_changed(struct core *c, struct unit *svc);

static void set_state(struct core *c, struct unit *u, int active, int sub) {
  int old = u->active;
  u->active = active; u->sub = sub;
  int64_t now = c->ops->now();
  if (old != active) {
    u->t_enter = now;
    if (active == A_ACTIVE && old != A_ACTIVE && old != A_RELOADING) u->t_active = now;
    if ((active == A_ACTIVATING || active == A_ACTIVE) && (old == A_INACTIVE || old == A_FAILED)) u->t_activated = now;
    if ((active == A_INACTIVE || active == A_FAILED) && old != A_INACTIVE && old != A_FAILED) u->t_inactive = now;
  }
  /* jobs */
  if (u->job >= 0) {
    struct job *j = &c->jobs[u->job];
    if (j->running) {
      if (j->type == J_START || j->type == J_VERIFY || (j->type == J_RESTART && j->running == 2) || (j->type == J_TRYRESTART && j->running == 2)) {
        if (active == A_ACTIVE) job_finish(c, u->job, "done");
        else if (active == A_FAILED) job_finish(c, u->job, "failed");
        else if (active == A_INACTIVE && old != A_INACTIVE) job_finish(c, u->job, strcmp(u->result, "success") ? "failed" : "done");
      } else if (j->type == J_STOP) {
        if (active == A_INACTIVE || active == A_FAILED) job_finish(c, u->job, "done");
      } else if (j->type == J_RESTART || j->type == J_TRYRESTART) {
        if (active == A_INACTIVE || active == A_FAILED) j->running = 3; /* stop half done: runs again as a start */
      } else if (j->type == J_RELOAD) {
        if (active == A_ACTIVE && old == A_RELOADING) job_finish(c, u->job, strcmp(u->result, "success") ? "failed" : "done");
        else if (active != A_RELOADING && active != A_ACTIVE) job_finish(c, u->job, "failed");
      }
    }
  }
  if (old == active) return;
  int me = self_idx(c, u);
  if (active == A_FAILED) {
    for (int i = 0; i < u->dep[D_ONFAILURE].n; i++) {
      char m[200];
      core_request(c, c->u[u->dep[D_ONFAILURE].u[i]]->name, J_START, "replace", -1, m, sizeof m);
    }
  }
  if ((active == A_INACTIVE || active == A_FAILED) && (old == A_ACTIVE || old == A_DEACTIVATING || old == A_RELOADING)) {
    /* BindsTo: what is bound to us stops with us */
    for (int i = 0; i < u->dep[D_BOUNDBY].n; i++) {
      struct unit *o = c->u[u->dep[D_BOUNDBY].u[i]];
      if (o && active_ish(o) && (o->job < 0 || c->jobs[o->job].type != J_STOP)) {
        char m[200];
        core_log(c, o, 6, "%s: Unit is bound to inactive unit %s. Stopping, too.", o->name, u->name);
        core_request(c, o->name, J_STOP, "replace", -1, m, sizeof m);
      }
    }
  }
  (void)me;
  if (u->type == SVC_T_SERVICE) trigger_changed(c, u);
}

static void svc_log_start_fail(struct core *c, struct unit *u) {
  core_log(c, u, 3, "%s: Failed with result '%s'.", u->name, u->result);
}

/* Build the environment for a process of u. Returns a malloc'ed NULL-terminated array
 * whose strings live in *blob. */
static char **build_env(struct core *c, struct unit *u, int listen_n, char **blob) {
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  int cnt = 0;
#define PUT(...) do { char t[512]; int l = snprintf(t, sizeof t, __VA_ARGS__); if (l < 0) l = 0; if (l >= (int)sizeof t) l = sizeof t - 1; \
  if (n + l + 1 > cap) { cap = (n + l + 1) * 2; b = realloc(b, cap); } memcpy(b + n, t, l + 1); n += l + 1; cnt++; } while (0)
  PUT("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
  PUT("LANG=C.UTF-8");
  PUT("HOME=/home/user");
  PUT("USER=user");
  PUT("LOGNAME=user");
  PUT("SHELL=/bin/sh");
  PUT("TERM=xterm-256color");
  if (u->main_pid > 0) PUT("MAINPID=%d", u->main_pid);
  PUT("MANAGERPID=1");
  PUT("INVOCATION_ID=%016llx", (unsigned long long)u->t_enter ^ (unsigned long long)(uintptr_t)u);
  if (u->stype == ST_NOTIFY || svc_get(&u->f, "Service", "NotifyAccess")) PUT("NOTIFY_SOCKET=/run/systemd/notify");
  if (listen_n > 0) { PUT("LISTEN_FDS=%d", listen_n); PUT("LISTEN_FDNAMES=%s", u->name); }
  if (u->rtdirs) { char d[256]; snprintf(d, sizeof d, "%s", u->rtdirs); char *nl = strchr(d, '\n'); if (nl) *nl = 0; char *sp = strchr(d, ' '); if (sp) *sp = 0; PUT("RUNTIME_DIRECTORY=/run/%s", d); }
  if (u->statedirs) { char d[256]; snprintf(d, sizeof d, "%s", u->statedirs); char *nl = strchr(d, '\n'); if (nl) *nl = 0; char *sp = strchr(d, ' '); if (sp) *sp = 0; PUT("STATE_DIRECTORY=/var/lib/%s", d); }
  /* EnvironmentFile=, then Environment= (later wins: duplicates are resolved below) */
  if (u->envfiles) {
    char *copy = strdup(u->envfiles), *sv = 0;
    for (char *line = strtok_r(copy, "\n", &sv); line; line = strtok_r(0, "\n", &sv)) {
      int optional = line[0] == '-';
      char path[256];
      svc_host_path(&c->roots, u->root, line + optional, path, sizeof path);
      char buf[4096];
      int l = c->ops->read_file(path, buf, sizeof buf - 1);
      if (l < 0) { if (!optional) core_log(c, u, 3, "%s: Failed to load environment files: %s", u->name, path); continue; }
      buf[l] = 0;
      char *s2 = 0;
      for (char *el = strtok_r(buf, "\n", &s2); el; el = strtok_r(0, "\n", &s2)) {
        while (*el == ' ' || *el == '\t') el++;
        if (!*el || *el == '#' || *el == ';' || !strchr(el, '=')) continue;
        if (!strncmp(el, "export ", 7)) el += 7;
        char *eq = strchr(el, '=');
        char val[512];
        char *v = eq + 1;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) vl--;
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v++; vl -= 2; }
        snprintf(val, sizeof val, "%.*s", (int)vl, v);
        PUT("%.*s=%s", (int)(eq - el), el, val);
      }
    }
    free(copy);
  }
  if (u->env) {
    char *copy = strdup(u->env), *sv = 0;
    for (char *line = strtok_r(copy, "\n", &sv); line; line = strtok_r(0, "\n", &sv)) {
      char **w;
      int nw = svc_words(line, &w);
      for (int i = 0; i < nw; i++) if (strchr(w[i], '=')) PUT("%s", w[i]);
      svc_free_words(w, nw);
    }
    free(copy);
  }
#undef PUT
  char **env = malloc((cnt + 1) * sizeof *env);
  int k = 0;
  for (size_t p = 0; p < n; p += strlen(b + p) + 1) {
    /* a later assignment of the same name replaces an earlier one */
    const char *eq = strchr(b + p, '=');
    size_t nl = eq ? (size_t)(eq - (b + p)) : strlen(b + p);
    int dup = -1;
    for (int q = 0; q < k; q++) if (!strncmp(env[q], b + p, nl) && env[q][nl] == '=') dup = q;
    if (dup >= 0) env[dup] = b + p; else env[k++] = b + p;
  }
  env[k] = 0;
  *blob = b;
  return env;
}
static const char *env_lookup(void *ctx, const char *name) {
  char **env = ctx;
  size_t l = strlen(name);
  for (int i = 0; env[i]; i++) if (!strncmp(env[i], name, l) && env[i][l] == '=') return env[i] + l + 1;
  return 0;
}

/* Spawn command idx of list kind. Returns 0 started, 1 nothing to run, <0 failed. */
static int exec_cmd(struct core *c, struct unit *u, int kind, int idx, int *pid_out) {
  if (idx >= u->exec[kind].n) return 1;
  struct unit *sock = 0;
  int listen_n = 0, extra[4];
  if (kind == X_START) {
    for (int i = 0; i < u->dep[D_TRIGGEREDBY].n; i++) {
      struct unit *s = c->u[u->dep[D_TRIGGEREDBY].u[i]];
      if (s && s->type == SVC_T_SOCKET && s->active == A_ACTIVE) { sock = s; break; }
    }
    if (sock) for (int i = 0; i < sock->nlisten; i++) if (sock->lfd[i] >= 0) extra[listen_n++] = sock->lfd[i];
  }
  char *blob;
  char **env = build_env(c, u, listen_n, &blob);
  struct svc_cmd *cmd;
  char err[100];
  int n = svc_cmdline(u->exec[kind].v[idx], env_lookup, env, &cmd, err, sizeof err);
  if (n <= 0) {
    core_log(c, u, 3, "%s: Invalid command line: %s", u->name, n < 0 ? err : "empty");
    free(env); free(blob);
    return -EINVAL;
  }
  struct svc_cmd *x = &cmd[0];
  int flags = x->flags;
  /* Find the program: a native one runs as is; a guest one (from the Debian root, or
   * any ELF file) through the engine; "#!" scripts through their interpreter. */
  char host[512], head[160];
  const char *p = x->path;
  char found[512];
  if (p[0] != '/') {
    static const char *const dirs[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin", 0};
    found[0] = 0;
    for (int i = 0; dirs[i] && !found[0]; i++) {
      char t[256];
      snprintf(t, sizeof t, "%s/%s", dirs[i], p);
      svc_host_path(&c->roots, u->root, t, host, sizeof host);
      if (c->ops->file_exists(host, 0) == 0) snprintf(found, sizeof found, "%s", t);
    }
    if (found[0]) p = found;
  }
  svc_host_path(&c->roots, u->root, p, host, sizeof host);
  int hl = c->ops->read_file(host, head, sizeof head - 1);
  char *argv[80];
  int ac = 0;
  char interp[128], iarg[128];
  char root_opt[160];
  if (hl < 0) {
    core_log(c, u, 3, "%s: Failed to locate executable %s: No such file or directory", u->name, p);
    core_log(c, u, 3, "%s: Failed at step EXEC spawning %s: No such file or directory", u->name, p);
    svc_cmds_free(cmd, n); free(env); free(blob);
    return -ENOENT;
  }
  head[hl] = 0;
  const char *exe = host;
  if (hl >= 4 && !memcmp(head, "\0asm", 4)) {
    for (int i = 0; i < x->argc && ac < 78; i++) argv[ac++] = x->argv[i];
#ifndef __wasm__
  } else if (hl >= 4 && !memcmp(head, "\177ELF", 4) && u->root == 0) {
    /* on a host (native tests) an ELF program is a native one */
    for (int i = 0; i < x->argc && ac < 78; i++) argv[ac++] = x->argv[i];
#endif
  } else if ((hl >= 4 && !memcmp(head, "\177ELF", 4)) || (u->root > 0 && hl >= 2 && head[0] == '#' && head[1] == '!')) {
    /* the engine resolves paths and "#!" inside the guest root itself */
    exe = c->engine;
    argv[ac++] = (char *)"lx";
    snprintf(root_opt, sizeof root_opt, "--root=%s", u->root > 0 ? c->roots.root[u->root] : "/");
    argv[ac++] = root_opt;
    argv[ac++] = (char *)"--";
    argv[ac++] = (char *)p;
    for (int i = 1; i < x->argc && ac < 78; i++) argv[ac++] = x->argv[i];
  } else if (hl >= 2 && head[0] == '#' && head[1] == '!') {
    char *s = head + 2;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
    snprintf(interp, sizeof interp, "%.*s", (int)(e - s), s);
    s = e;
    while (*s == ' ' || *s == '\t') s++;
    e = s;
    while (*e && *e != '\n') e++;
    snprintf(iarg, sizeof iarg, "%.*s", (int)(e - s), s);
    exe = interp;
    argv[ac++] = interp;
    if (iarg[0]) argv[ac++] = iarg;
    argv[ac++] = host;
    for (int i = 1; i < x->argc && ac < 78; i++) argv[ac++] = x->argv[i];
  } else {
    core_log(c, u, 3, "%s: Failed at step EXEC spawning %s: Exec format error", u->name, p);
    svc_cmds_free(cmd, n); free(env); free(blob);
    return -ENOEXEC;
  }
  argv[ac] = 0;
  int k1 = 0, k2 = 0, ofd, efd;
  if (u->outfd >= 0 && u->outkeep) { ofd = efd = u->outfd; k1 = k2 = 1; } /* Accept=yes: the connection */
  else {
    ofd = c->ops->open_output(u, 1, &k1);
    efd = u->err < 0 ? ofd : c->ops->open_output(u, 2, &k2);
    if (u->err < 0) k2 = 1;
  }
  int pid = c->ops->spawn(u, exe, argv, env, u->in == OUT_SOCKET ? u->accept_n : u->in == OUT_INHERIT ? -2 : -1, ofd, efd, u->pgid,
                          extra, kind == X_START ? listen_n : 0);
  if (pid > 0 && c->ops->spawned) {
    const char *b = strrchr(x->argv[0], '/');
    c->ops->spawned(u, pid, b ? b + 1 : x->argv[0]);
  }
  if (!k1 && ofd >= 0) c->ops->closefd(ofd);
  if (!k2 && efd >= 0 && efd != ofd) c->ops->closefd(efd);
  if (pid <= 0 && c->ops->spawned) c->ops->spawned(u, -1, 0);
  svc_cmds_free(cmd, n); free(env); free(blob);
  if (pid < 0) {
    core_log(c, u, 3, "%s: Failed at step EXEC spawning %s: %s", u->name, p, strerror(-pid));
    return pid;
  }
  if (!u->pgid) u->pgid = pid;
  *pid_out = pid;
  return (flags & SVC_EX_IGNORE) ? 2 : 0;
}

static int ignore_flag(struct unit *u, int kind, int idx) {
  if (idx >= u->exec[kind].n) return 0;
  for (const char *p = u->exec[kind].v[idx]; *p && strchr("-@:+!", *p); p++) if (*p == '-') return 1;
  return 0;
}

static void svc_running(struct core *c, struct unit *u);
static void svc_after_start(struct core *c, struct unit *u);

/* Run control command idx of kind, or move on to what follows that list. */
static int run_next(struct core *c, struct unit *u, int kind, int idx) {
  for (;; idx++) {
    int pid = 0;
    int r = exec_cmd(c, u, kind, idx, &pid);
    if (r == 1) return 1; /* the list is done */
    if (r < 0) {
      if (ignore_flag(u, kind, idx)) continue;
      u->exit_code = 1; u->exit_status = 203;
      strcpy(u->result, "exit-code");
      return -1;
    }
    if (kind == X_START) { u->main_pid = pid; u->ctl_pid = 0; }
    else u->ctl_pid = pid;
    u->ctl_kind = kind; u->ctl_idx = idx;
    return 0;
  }
}

static void arm(struct core *c, struct unit *u, int64_t ms) {
  u->deadline = ms == SVC_INFINITY || ms < 0 ? -1 : c->ops->now() + ms;
}

static void make_dirs(struct core *c, struct unit *u) {
  const char *lists[3] = {u->rtdirs, u->statedirs, u->otherdirs};
  const char *bases[3] = {"/run/", "/var/lib/", ""};
  for (int k = 0; k < 3; k++) {
    if (!lists[k]) continue;
    char **w;
    char *copy = strdup(lists[k]);
    for (char *p = copy; *p; p++) if (*p == '\n') *p = ' ';
    int nw = svc_words(copy, &w);
    free(copy);
    for (int i = 0; i < nw; i++) {
      char d[300], h[400];
      snprintf(d, sizeof d, "%s%s", bases[k], w[i]);
      svc_host_path(&c->roots, u->root, d, h, sizeof h);
      c->ops->mkdirs(h);
    }
    svc_free_words(w, nw);
  }
}
static void remove_rtdirs(struct core *c, struct unit *u) {
  if (!u->rtdirs || u->rt_preserve) return;
  char **w;
  char *copy = strdup(u->rtdirs);
  for (char *p = copy; *p; p++) if (*p == '\n') *p = ' ';
  int nw = svc_words(copy, &w);
  free(copy);
  for (int i = 0; i < nw; i++) {
    if (!w[i][0] || strstr(w[i], "..")) continue;
    char d[300], h[400];
    snprintf(d, sizeof d, "/run/%s", w[i]);
    svc_host_path(&c->roots, u->root, d, h, sizeof h);
    c->ops->rmtree(h);
  }
  svc_free_words(w, nw);
}

static int svc_start(struct core *c, struct unit *u) {
  int64_t now = c->ops->now();
  /* start rate limit (StartLimitIntervalSec=, StartLimitBurst=) */
  if (u->limit_interval_ms > 0 && u->limit_burst > 0) {
    int k = 0;
    for (int i = 0; i < u->nstarts; i++) if (now - u->starts[i] < u->limit_interval_ms) u->starts[k++] = u->starts[i];
    u->nstarts = k;
    if (u->nstarts >= u->limit_burst) {
      core_log(c, u, 3, "%s: Start request repeated too quickly.", u->name);
      strcpy(u->result, "start-limit-hit");
      svc_log_start_fail(c, u);
      core_log(c, u, 3, "Failed to start %s.", u->desc);
      set_state(c, u, A_FAILED, S_FAILED);
      return -1;
    }
    if (u->nstarts < 16) u->starts[u->nstarts++] = now;
  }
  int af;
  char which[300];
  if (!check_conditions(c, u, &af, which, sizeof which)) {
    if (af) {
      core_log(c, u, 3, "%s: Assertion failed on job for %s.", u->name, u->name);
      strcpy(u->result, "assert");
      set_state(c, u, A_FAILED, S_FAILED);
      return -1;
    }
    core_log(c, u, 6, "%s was skipped because of an unmet condition check (%s).", u->desc, which);
    strcpy(u->result, "success");
    if (u->job >= 0) job_finish(c, u->job, "skipped");
    set_state(c, u, A_INACTIVE, S_DEAD);
    return 0;
  }
  strcpy(u->result, "success");
  u->exit_code = u->exit_status = 0;
  u->main_pid = u->ctl_pid = 0; u->pgid = 0;
  u->notify_ready = 0; u->stop_after_start = 0;
  u->mem_peak = 0; u->mem_high_logged = 0;
  u->status_text[0] = 0;
  make_dirs(c, u);
  core_log(c, u, 6, "Starting %s...", u->desc);
  set_state(c, u, A_ACTIVATING, S_CONDITION);
  arm(c, u, u->tstart_ms);
  int r = run_next(c, u, X_CONDITION, 0);
  if (r == 0) return 0;
  if (r < 0) { svc_enter_dead(c, u, "exit-code"); return -1; }
  set_state(c, u, A_ACTIVATING, S_START_PRE);
  r = run_next(c, u, X_PRE, 0);
  if (r == 0) return 0;
  if (r < 0) { core_log(c, u, 3, "Failed to start %s.", u->desc); svc_enter_dead(c, u, "exit-code"); return -1; }
  svc_after_start(c, u);
  return 0;
}

/* Pre commands are done: run ExecStart= */
static void svc_after_start(struct core *c, struct unit *u) {
  set_state(c, u, A_ACTIVATING, S_START);
  int r = run_next(c, u, X_START, 0);
  if (r < 0) { core_log(c, u, 3, "Failed to start %s.", u->desc); svc_enter_stop(c, u, "exit-code"); return; }
  if (r == 1) { /* oneshot with no ExecStart (RemainAfterExit=yes): straight to the post commands */
    svc_running(c, u);
    return;
  }
  switch (u->stype) {
  case ST_SIMPLE: case ST_EXEC: case ST_IDLE: case ST_DBUS:
    svc_running(c, u);
    break;
  default: break; /* forking, oneshot: wait for the exit; notify: for READY=1 */
  }
}

static void svc_running(struct core *c, struct unit *u) {
  set_state(c, u, A_ACTIVATING, S_START_POST);
  int r = run_next(c, u, X_POST, 0);
  if (r == 0) return;
  if (r < 0) { svc_enter_stop(c, u, "exit-code"); return; }
  u->deadline = -1;
  if (u->stype == ST_ONESHOT && !u->main_pid) {
    if (u->remain) { core_log(c, u, 6, "Finished %s.", u->desc); set_state(c, u, A_ACTIVE, S_EXITED); }
    else { core_log(c, u, 6, "Finished %s.", u->desc); svc_enter_stop(c, u, "success"); }
    return;
  }
  if (!u->main_pid && u->remain) { set_state(c, u, A_ACTIVE, S_EXITED); return; }
  core_log(c, u, 6, "Started %s.", u->desc);
  set_state(c, u, A_ACTIVE, S_RUNNING);
  if (u->reload_pending) { u->reload_pending = 0; }
}

static int kill_group(struct core *c, struct unit *u, int sig, int main_only) {
  int pids[64], n = 0;
  if (main_only) { if (u->main_pid > 0) pids[n++] = u->main_pid; }
  else n = c->ops->members(u->pgid, u->main_pid, pids, 64);
  if (u->ctl_pid > 0 && !main_only) {
    int seen = 0;
    for (int i = 0; i < n; i++) if (pids[i] == u->ctl_pid) seen = 1;
    if (!seen && n < 64) pids[n++] = u->ctl_pid;
  }
  int sent = 0;
  for (int i = 0; i < n; i++) if (c->ops->kill(pids[i], sig) == 0) sent++;
  return sent;
}
static int alive(struct core *c, struct unit *u) {
  int pids[64];
  int n = c->ops->members(u->pgid, u->main_pid, pids, 64);
  return n + (u->ctl_pid > 0);
}

static void svc_stop_post(struct core *c, struct unit *u);

/* Kill what is left according to KillMode=, then wait for it. */
static void svc_enter_signal(struct core *c, struct unit *u, int final) {
  int sig = final ? u->finalsig : u->killsig;
  int sent = 0;
  if (u->killmode == KM_NONE) sent = 0;
  else if (u->killmode == KM_PROCESS || (u->killmode == KM_MIXED && !final)) sent = kill_group(c, u, sig, 1);
  else sent = kill_group(c, u, sig, 0);
  if (!sent || u->killmode == KM_NONE) { svc_stop_post(c, u); return; }
  set_state(c, u, A_DEACTIVATING, final ? S_STOP_SIGKILL : S_STOP_SIGTERM);
  arm(c, u, u->tstop_ms);
}

static void svc_enter_stop(struct core *c, struct unit *u, const char *result) {
  if (strcmp(result, "success") && !strcmp(u->result, "success")) snprintf(u->result, sizeof u->result, "%s", result);
  u->deadline = -1;
  /* ExecStop= runs only if the service had started */
  int had_started = u->active == A_ACTIVE || u->active == A_RELOADING || u->sub == S_RUNNING || u->sub == S_EXITED;
  set_state(c, u, A_DEACTIVATING, S_STOP);
  arm(c, u, u->tstop_ms);
  if (had_started) {
    int r = run_next(c, u, X_STOP, 0);
    if (r == 0) return;
  }
  svc_enter_signal(c, u, 0);
}

static void svc_stop_post(struct core *c, struct unit *u) {
  set_state(c, u, A_DEACTIVATING, S_STOP_POST);
  arm(c, u, u->tstop_ms);
  u->main_pid = 0;
  int r = run_next(c, u, X_STOPPOST, 0);
  if (r == 0) return;
  svc_enter_dead(c, u, "success");
}

static int restart_wanted(struct unit *u) {
  int clean = !strcmp(u->result, "success");
  for (int i = 0; i < u->nprevent; i++)
    if ((u->exit_code == 1 && u->prevent_status[i] == u->exit_status) || (u->exit_code == 2 && u->prevent_status[i] == 1000 + u->exit_status)) return 0;
  for (int i = 0; i < u->nforce; i++)
    if ((u->exit_code == 1 && u->force_status[i] == u->exit_status) || (u->exit_code == 2 && u->force_status[i] == 1000 + u->exit_status)) return 1;
  if (!strcmp(u->result, "start-limit-hit")) return 0;
  switch (u->restart) {
  case RS_ALWAYS: return 1;
  case RS_SUCCESS: return clean;
  case RS_FAILURE: return !clean;
  case RS_ABNORMAL: return !strcmp(u->result, "signal") || !strcmp(u->result, "core-dump") || !strcmp(u->result, "timeout") || !strcmp(u->result, "watchdog");
  case RS_ABORT: return !strcmp(u->result, "signal") || !strcmp(u->result, "core-dump");
  case RS_WATCHDOG: return !strcmp(u->result, "watchdog");
  }
  return 0;
}

static void svc_enter_dead(struct core *c, struct unit *u, const char *result) {
  if (strcmp(result, "success") && !strcmp(u->result, "success")) snprintf(u->result, sizeof u->result, "%s", result);
  u->deadline = -1;
  u->main_pid = 0; u->ctl_pid = 0;
  /* forget the process group once it is empty: pids (and so group numbers) are reused at once */
  if (u->pgid > 0) { int pids[4]; if (c->ops->members(u->pgid, 0, pids, 4) == 0) u->pgid = 0; }
  remove_rtdirs(c, u);
  int explicit_stop = u->job >= 0 && (c->jobs[u->job].type == J_STOP || ((c->jobs[u->job].type == J_RESTART || c->jobs[u->job].type == J_TRYRESTART) && c->jobs[u->job].running == 1));
  int failed = strcmp(u->result, "success") != 0;
  if (!explicit_stop && !c->shutting_down && restart_wanted(u)) {
    int64_t d = restart_delay(u);
    u->restart_at = c->ops->now() + d;
    if (failed) svc_log_start_fail(c, u);
    /* the start job fails now; the restart is a job of its own later (as systemd does) */
    if (failed && u->job >= 0 && startish(c->jobs[u->job].type) && c->jobs[u->job].running) job_finish(c, u->job, "failed");
    set_state(c, u, A_ACTIVATING, S_AUTO_RESTART);
    return;
  }
  int act = failed ? u->failure_action : u->success_action;
  if (act && !explicit_stop && !c->shutting_down) {
    core_log(c, u, 5, "%s: %s action %s triggered by the unit.", u->name, failed ? "Failure" : "Success", act == 2 ? "reboot" : "poweroff");
    core_poweroff(c, act == 2);
  }
  if (failed) {
    svc_log_start_fail(c, u);
    set_state(c, u, A_FAILED, S_FAILED);
  } else {
    if (u->stype != ST_ONESHOT || u->exec[X_STOP].n) core_log(c, u, 6, "%s: Deactivated successfully.", u->name);
    if (explicit_stop) core_log(c, u, 6, "Stopped %s.", u->desc);
    set_state(c, u, A_INACTIVE, S_DEAD);
  }
}

static int status_clean(struct unit *u, int code, int st, int main) {
  if (code == 1) {
    if (st == 0) return 1;
    for (int i = 0; i < u->nsucc; i++) if (u->succ_status[i] == st) return 1;
    return 0;
  }
  for (int i = 0; i < u->nsucc; i++) if (u->succ_status[i] == 1000 + st) return 1;
  if (main && u->stype != ST_ONESHOT && (st == 1 || st == 2 || st == 15 || st == 13)) return 1;
  return 0;
}

static int read_pidfile(struct core *c, struct unit *u) {
  if (!u->pidfile[0]) return 0;
  char h[300], b[32];
  svc_host_path(&c->roots, u->root, u->pidfile, h, sizeof h);
  int l = c->ops->read_file(h, b, sizeof b - 1);
  if (l <= 0) return 0;
  b[l] = 0;
  int pid = atoi(b);
  return pid > 0 ? pid : 0;
}

void core_child_exit(struct core *c, int pid, int status) {
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || u->type != SVC_T_SERVICE) continue;
    int is_main = pid == u->main_pid, is_ctl = pid == u->ctl_pid;
    if (!is_main && !is_ctl) continue;
    int code = WIFEXITED(status) ? 1 : 2;
    int st = code == 1 ? WEXITSTATUS(status) : WTERMSIG(status);
    int clean = status_clean(u, code, st, is_main);
    if (is_main) { u->exit_code = code; u->exit_status = st; }
    const char *res = clean ? "success" : code == 1 ? "exit-code" : (st == 11 || st == 6 || st == 4) ? "core-dump" : "signal";
    if (!clean || is_main) {
      if (code == 1) core_log(c, u, clean ? 7 : 5, "%s: %s process exited, code=exited, status=%d/%s", u->name, is_main ? "Main" : "Control", st,
                              st == 0 ? "SUCCESS" : st == 1 ? "FAILURE" : st == 2 ? "INVALIDARGUMENT" : st == 203 ? "EXEC" : st == 127 ? "n/a" : "n/a");
      else core_log(c, u, clean ? 7 : 5, "%s: %s process exited, code=killed, status=%d/%s", u->name, is_main ? "Main" : "Control", st, signame(st) + 3);
    }
    if (is_ctl) {
      u->ctl_pid = 0;
      int kind = u->ctl_kind, idx = u->ctl_idx;
      int ign = ignore_flag(u, kind, idx);
      if (!clean && !ign && (kind == X_PRE || kind == X_POST || kind == X_CONDITION)) {
        if (kind == X_CONDITION && code == 1 && st < 255) {
          core_log(c, u, 6, "%s: Skipped due to 'exec-condition'.", u->name);
          svc_enter_dead(c, u, "success");
          return;
        }
        u->exit_code = code; u->exit_status = st;
        core_log(c, u, 3, "Failed to start %s.", u->desc);
        if (u->main_pid > 0) svc_enter_stop(c, u, res);
        else svc_enter_dead(c, u, res);
        return;
      }
      int r = run_next(c, u, kind, idx + 1);
      if (r == 0) return;
      switch (kind) {
      case X_CONDITION:
        set_state(c, u, A_ACTIVATING, S_START_PRE);
        r = run_next(c, u, X_PRE, 0);
        if (r == 0) return;
        if (r < 0) { svc_enter_dead(c, u, "exit-code"); return; }
        svc_after_start(c, u);
        return;
      case X_PRE: if (r < 0) { svc_enter_dead(c, u, "exit-code"); return; } svc_after_start(c, u); return;
      case X_POST:
        u->deadline = -1;
        if (u->stype == ST_ONESHOT && !u->main_pid) {
          core_log(c, u, 6, "Finished %s.", u->desc);
          if (u->remain) set_state(c, u, A_ACTIVE, S_EXITED); else svc_enter_stop(c, u, "success");
        } else if (!u->main_pid && (u->remain || u->stype == ST_FORKING)) {
          set_state(c, u, A_ACTIVE, S_EXITED);
        } else { core_log(c, u, 6, "Started %s.", u->desc); set_state(c, u, A_ACTIVE, S_RUNNING); }
        return;
      case X_RELOAD:
        if (!clean && !ign) { strcpy(u->result, "exit-code"); core_log(c, u, 3, "Reload failed for %s.", u->desc); }
        else core_log(c, u, 6, "Reloaded %s.", u->desc);
        u->deadline = -1;
        set_state(c, u, A_ACTIVE, u->main_pid ? S_RUNNING : S_EXITED);
        strcpy(u->result, "success");
        return;
      case X_STOP: svc_enter_signal(c, u, 0); return;
      case X_STOPPOST: svc_enter_dead(c, u, "success"); return;
      }
      return;
    }
    /* the main process */
    u->main_pid = 0;
    if (u->sub == S_START && u->stype == ST_ONESHOT) {
      if (!clean) { core_log(c, u, 3, "Failed to start %s.", u->desc); svc_enter_stop(c, u, res); return; }
      int r = run_next(c, u, X_START, u->ctl_idx + 1);
      if (r == 0) return;
      if (r < 0) { svc_enter_stop(c, u, "exit-code"); return; }
      u->main_pid = 0;
      svc_running(c, u);
      return;
    }
    if (u->sub == S_START && u->stype == ST_FORKING) {
      if (!clean) { core_log(c, u, 3, "Failed to start %s.", u->desc); svc_enter_stop(c, u, res); return; }
      int np = read_pidfile(c, u);
      if (!np && u->guess) {
        int pids[64];
        int n = c->ops->members(u->pgid, 0, pids, 64);
        if (n == 1) np = pids[0];
      }
      if (u->pidfile[0] && !np) {
        core_log(c, u, 4, "%s: Can't open PID file %s (yet?) after start: No such file or directory", u->name, u->pidfile);
        strcpy(u->result, "protocol");
        svc_enter_stop(c, u, "protocol");
        return;
      }
      u->main_pid = np;
      if (!np) core_log(c, u, 4, "%s: Supervising process could not be determined; the service has no main process.", u->name);
      svc_running(c, u);
      return;
    }
    if (u->sub == S_START && u->stype == ST_NOTIFY) {
      core_log(c, u, 3, "Failed to start %s.", u->desc);
      svc_enter_stop(c, u, clean ? "protocol" : res);
      return;
    }
    if (u->sub == S_STOP_SIGTERM || u->sub == S_STOP_SIGKILL) {
      if (!alive(c, u)) svc_stop_post(c, u);
      return;
    }
    if (u->sub == S_STOP || u->sub == S_STOP_POST) return; /* ExecStop/StopPost still running */
    /* exited while running, starting (post) or reloading */
    if (clean && u->remain) { set_state(c, u, A_ACTIVE, S_EXITED); return; }
    svc_enter_stop(c, u, res);
    return;
  }
}

int core_notify(struct core *c, int pid, const char *msg) {
  /* the unit whose main or control process sent it; else one whose live group holds it */
  struct unit *u = 0;
  for (int i = 0; i < c->nu && !u; i++)
    if (c->u[i] && c->u[i]->type == SVC_T_SERVICE && core_unit_tracks(c->u[i], pid)) u = c->u[i];
  for (int i = 0; i < c->nu && !u; i++) {
    struct unit *x = c->u[i];
    if (!x || x->type != SVC_T_SERVICE || x->pgid <= 0 || !active_ish(x)) continue;
    int pids[64], n = c->ops->members(x->pgid, x->main_pid, pids, 64);
    for (int k = 0; k < n; k++) if (pids[k] == pid) u = x;
  }
  if (!u || !u->notify_access) return -1;
  char *copy = strdup(msg), *sv = 0;
  for (char *l = strtok_r(copy, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
    if (!strcmp(l, "READY=1")) {
      u->notify_ready = 1;
      if (u->sub == S_START && u->stype == ST_NOTIFY) svc_running(c, u);
      else if (u->active == A_RELOADING && u->stype == ST_NOTIFY && !u->ctl_pid) { set_state(c, u, A_ACTIVE, S_RUNNING); core_log(c, u, 6, "Reloaded %s.", u->desc); }
    } else if (!strncmp(l, "STATUS=", 7)) snprintf(u->status_text, sizeof u->status_text, "%s", l + 7);
    else if (!strncmp(l, "MAINPID=", 8)) { int p = atoi(l + 8); if (p > 0) u->main_pid = p; }
    else if (!strcmp(l, "RELOADING=1")) { if (u->active == A_ACTIVE) set_state(c, u, A_RELOADING, S_RELOAD); }
    else if (!strcmp(l, "STOPPING=1")) { if (u->active == A_ACTIVE) set_state(c, u, A_DEACTIVATING, S_STOP_SIGTERM); }
    else if (!strncmp(l, "ERRNO=", 6)) u->status_errno = atoi(l + 6);
  }
  free(copy);
  return 0;
}

int core_unit_tracks(struct unit *u, int pid) {
  if (pid <= 0) return 0;
  if (pid == u->main_pid || pid == u->ctl_pid) return 1;
  return 0;
}

static void svc_reload(struct core *c, struct unit *u) {
  /* On a kernel without signal delivery, "kill -HUP $MAINPID" terminates the service.
   * Such a reload is turned into a restart, and said so. */
  int by_signal = 0;
  for (int i = 0; i < u->exec[X_RELOAD].n; i++) {
    const char *v = u->exec[X_RELOAD].v[i];
    while (*v == '-' || *v == '@' || *v == '+' || *v == '!' || *v == ':') v++;
    if ((!strncmp(v, "/bin/kill ", 10) || !strncmp(v, "/usr/bin/kill ", 14) || !strncmp(v, "kill ", 5)) && strstr(v, "MAINPID")) by_signal = 1;
  }
  if (!c->have_signals && by_signal) {
    {
      core_log(c, u, 4, "%s: Reload by signal is not possible on this kernel (signals end processes); restarting instead.", u->name);
      if (u->job >= 0) { c->jobs[u->job].type = J_RESTART; c->jobs[u->job].running = 1; }
      svc_enter_stop(c, u, "success");
      return;
    }
  }
  core_log(c, u, 6, "Reloading %s...", u->desc);
  set_state(c, u, A_RELOADING, S_RELOAD);
  arm(c, u, u->tstart_ms);
  int r = run_next(c, u, X_RELOAD, 0);
  if (r == 1) { set_state(c, u, A_ACTIVE, u->main_pid ? S_RUNNING : S_EXITED); return; }
  if (r < 0) { core_log(c, u, 3, "Reload failed for %s.", u->desc); set_state(c, u, A_ACTIVE, u->main_pid ? S_RUNNING : S_EXITED); if (u->job >= 0) job_finish(c, u->job, "failed"); }
}

/* ---------- timers, sockets, paths ---------- */
static void timer_arm(struct core *c, struct unit *u) {
  int64_t now = c->ops->now(), best = -1;
  struct unit *svc = u->dep[D_TRIGGERS].n ? c->u[u->dep[D_TRIGGERS].u[0]] : 0;
#define CAND(t) do { int64_t _t = (t); if (_t > u->last_trigger && (best < 0 || _t < best)) best = _t; } while (0)
  if (u->on_boot >= 0) CAND(c->boot_ms + u->on_boot);
  if (u->on_startup >= 0) CAND(c->boot_ms + u->on_startup);
  if (u->on_active >= 0) CAND(u->t_active + u->on_active);
  if (u->on_unit_active >= 0 && svc && svc->t_activated > 0) CAND(svc->t_activated + u->on_unit_active);
  if (u->on_unit_inactive >= 0 && svc && svc->t_inactive > 0) CAND(svc->t_inactive + u->on_unit_inactive);
  u->next_real = -1;
  for (int i = 0; i < u->ncal; i++) {
    int64_t ep = c->ops->epoch();
    int64_t nx = svc_cal_next(&u->cal[i], ep / 1000);
    if (nx < 0) continue;
    int64_t mono = now + (nx * 1000 - ep);
    if (u->next_real < 0 || nx * 1000 < u->next_real) u->next_real = nx * 1000;
    if (best < 0 || mono < best) best = mono;
  }
#undef CAND
  if (best >= 0 && u->rand_delay > 0) best += c->ops->random() % (uint32_t)(u->rand_delay > 0x7fffffff ? 0x7fffffff : u->rand_delay);
  if (best >= 0 && best < now) best = now;
  u->next_elapse = best;
  if (u->active == A_ACTIVE) u->sub = best >= 0 ? (svc && active_ish(svc) ? S_RUNNING : S_WAITING) : S_ELAPSED;
}

static void trigger_changed(struct core *c, struct unit *svc) {
  for (int i = 0; i < svc->dep[D_TRIGGEREDBY].n; i++) {
    struct unit *t = c->u[svc->dep[D_TRIGGEREDBY].u[i]];
    if (!t || t->active != A_ACTIVE) continue;
    if (t->type == SVC_T_TIMER) timer_arm(c, t);
    if (t->type == SVC_T_SOCKET) {
      t->listening = !(svc->active == A_ACTIVE || svc->active == A_ACTIVATING || svc->active == A_RELOADING);
      t->sub = t->listening ? S_LISTENING : S_RUNNING;
    }
  }
}

static int unit_start(struct core *c, struct unit *u) {
  switch (u->type) {
  case SVC_T_SERVICE:
    if (u->stype == ST_DBUS) core_log(c, u, 4, "%s: D-Bus is not available on UC.OS; started as Type=simple.", u->name);
    return svc_start(c, u);
  case SVC_T_TIMER:
    set_state(c, u, A_ACTIVE, S_WAITING);
    timer_arm(c, u);
    return 0;
  case SVC_T_SOCKET: {
    for (int i = 0; i < u->nlisten; i++) {
      const char *l = u->listen[i];
      if (l[0] != '/') {
        core_log(c, u, 3, "%s: %s%s: not supported on UC.OS (only AF_UNIX stream sockets at a path exist; no network, no abstract names).", u->name,
                 l[0] == '!' ? "" : "ListenStream=", l[0] == '!' ? l + 1 : l);
        for (int k = 0; k < i; k++) if (u->lfd[k] >= 0) { c->ops->closefd(u->lfd[k]); u->lfd[k] = -1; }
        strcpy(u->result, "resources");
        core_log(c, u, 3, "Failed to listen on %s.", u->desc);
        set_state(c, u, A_FAILED, S_FAILED);
        return -1;
      }
      u->lfd[i] = c->ops->listen(l);
      if (u->lfd[i] < 0) {
        core_log(c, u, 3, "%s: Failed to create listening socket (%s): %s", u->name, l, strerror(-u->lfd[i]));
        u->lfd[i] = -1;
        strcpy(u->result, "resources");
        set_state(c, u, A_FAILED, S_FAILED);
        return -1;
      }
    }
    u->listening = 1;
    core_log(c, u, 6, "Listening on %s.", u->desc);
    set_state(c, u, A_ACTIVE, S_LISTENING);
    return 0;
  }
  case SVC_T_PATH:
    for (int i = 0; i < u->nwatch; i++) {
      if (u->makedir && u->watchkind[i] == 4) c->ops->mkdirs(u->watch[i]);
      u->watch_mtime[i] = c->ops->mtime(u->watch[i]);
    }
    set_state(c, u, A_ACTIVE, S_WAITING);
    return 0;
  default:
    if (u->type == SVC_T_TARGET) core_log(c, u, 6, "Reached target %s.", u->desc);
    set_state(c, u, A_ACTIVE, S_ACTIVE);
    return 0;
  }
}

static int unit_stop(struct core *c, struct unit *u) {
  switch (u->type) {
  case SVC_T_SERVICE:
    if (u->sub == S_AUTO_RESTART) { u->restart_at = -1; set_state(c, u, A_INACTIVE, S_DEAD); return 0; }
    if (u->active == A_INACTIVE || u->active == A_FAILED) return 0;
    core_log(c, u, 6, "Stopping %s...", u->desc);
    if (u->sub == S_STOP || u->sub == S_STOP_SIGTERM || u->sub == S_STOP_SIGKILL || u->sub == S_STOP_POST) return 0;
    svc_enter_stop(c, u, "success");
    return 0;
  case SVC_T_SOCKET:
    for (int i = 0; i < u->nlisten; i++) if (u->lfd[i] >= 0) { c->ops->closefd(u->lfd[i]); u->lfd[i] = -1; }
    u->listening = 0;
    core_log(c, u, 6, "Closed %s.", u->desc);
    set_state(c, u, A_INACTIVE, S_DEAD);
    return 0;
  case SVC_T_TIMER:
    u->next_elapse = -1;
    core_log(c, u, 6, "Stopped %s.", u->desc);
    set_state(c, u, A_INACTIVE, S_DEAD);
    return 0;
  default:
    if (u->type == SVC_T_TARGET && u->active == A_ACTIVE) core_log(c, u, 6, "Stopped target %s.", u->desc);
    set_state(c, u, A_INACTIVE, S_DEAD);
    return 0;
  }
}

static int job_blocked(struct core *c, int j) {
  struct job *jb = &c->jobs[j];
  for (int k = 0; k < MAXJOBS; k++) {
    if (k == j || !c->jobs[k].used) continue;
    if (waits_for(c, jb->unit, jb->type, c->jobs[k].unit, c->jobs[k].type)) return 1;
  }
  return 0;
}

static void job_run(struct core *c, int j) {
  struct job *jb = &c->jobs[j];
  struct unit *u = c->u[jb->unit];
  switch (jb->type) {
  case J_VERIFY:
    job_finish(c, j, u->active == A_ACTIVE ? "done" : "dependency");
    return;
  case J_START:
    if (u->active == A_ACTIVE || u->active == A_RELOADING) { job_finish(c, j, "done"); return; }
    if (u->active == A_ACTIVATING && u->sub != S_AUTO_RESTART) { jb->running = 1; return; }
    if (u->active == A_DEACTIVATING) return; /* wait for it to finish stopping */
    if (u->load != L_LOADED) { job_finish(c, j, "failed"); return; }
    if (u->sub == S_AUTO_RESTART) u->restart_at = -1;
    jb->running = 1;
    unit_start(c, u);
    if (u->job == j && u->active == A_ACTIVE) job_finish(c, j, "done");
    return;
  case J_STOP:
    if ((u->active == A_INACTIVE || u->active == A_FAILED) && u->sub != S_AUTO_RESTART) { job_finish(c, j, "done"); return; }
    jb->running = 1;
    unit_stop(c, u);
    if (u->job == j && (u->active == A_INACTIVE || u->active == A_FAILED)) job_finish(c, j, "done");
    return;
  case J_TRYRESTART:
    if (jb->running == 0 && u->active != A_ACTIVE && u->active != A_RELOADING) { job_finish(c, j, "done"); return; }
    /* fall through */
  case J_RESTART:
    if (jb->running == 0 && (u->active == A_ACTIVE || u->active == A_ACTIVATING || u->active == A_RELOADING) && u->sub != S_AUTO_RESTART) {
      jb->running = 1;
      unit_stop(c, u);
      if (u->job == j && (u->active == A_INACTIVE || u->active == A_FAILED)) jb->running = 3;
      else return;
    }
    if (u->active == A_DEACTIVATING) return;
    jb->running = 2;
    if (u->sub == S_AUTO_RESTART) u->restart_at = -1;
    /* a restart clears a failed state's counter only through reset-failed, like systemd */
    unit_start(c, u);
    if (u->job == j && u->active == A_ACTIVE) job_finish(c, j, "done");
    return;
  case J_RELOAD:
    if (u->active != A_ACTIVE) { job_finish(c, j, "failed"); return; }
    jb->running = 1;
    if (u->type == SVC_T_SERVICE) svc_reload(c, u);
    else job_finish(c, j, "done");
    return;
  }
}

void core_run(struct core *c) {
  int64_t now = c->ops->now();
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u) continue;
    /* deadlines */
    if (u->deadline >= 0 && now >= u->deadline) {
      u->deadline = -1;
      switch (u->sub) {
      case S_CONDITION: case S_START_PRE: case S_START: case S_START_POST:
        core_log(c, u, 3, "%s: %s operation timed out. Terminating.", u->name, u->sub == S_START_PRE ? "start-pre" : u->sub == S_START_POST ? "start-post" : "start");
        core_log(c, u, 3, "Failed to start %s.", u->desc);
        if (u->ctl_pid > 0) c->ops->kill(u->ctl_pid, u->killsig);
        u->ctl_pid = 0;
        svc_enter_signal(c, u, 0);
        strcpy(u->result, "timeout");
        break;
      case S_RELOAD:
        core_log(c, u, 3, "%s: Reload operation timed out. Killing reload process.", u->name);
        if (u->ctl_pid > 0) c->ops->kill(u->ctl_pid, 9);
        u->ctl_pid = 0;
        set_state(c, u, A_ACTIVE, u->main_pid ? S_RUNNING : S_EXITED);
        if (u->job >= 0) job_finish(c, u->job, "timeout");
        break;
      case S_STOP:
        core_log(c, u, 3, "%s: Stopping timed out. Terminating.", u->name);
        if (u->ctl_pid > 0) c->ops->kill(u->ctl_pid, u->killsig);
        u->ctl_pid = 0;
        strcpy(u->result, "timeout");
        svc_enter_signal(c, u, 0);
        break;
      case S_STOP_SIGTERM:
        if (u->sendkill) { core_log(c, u, 3, "%s: State 'stop-sigterm' timed out. Killing.", u->name); strcpy(u->result, "timeout"); svc_enter_signal(c, u, 1); }
        else { core_log(c, u, 3, "%s: State 'stop-sigterm' timed out. Skipping SIGKILL.", u->name); svc_stop_post(c, u); }
        break;
      case S_STOP_SIGKILL:
        core_log(c, u, 3, "%s: Processes still around after SIGKILL. Ignoring.", u->name);
        svc_stop_post(c, u);
        break;
      case S_STOP_POST:
        core_log(c, u, 3, "%s: State 'stop-post' timed out.", u->name);
        if (u->ctl_pid > 0) c->ops->kill(u->ctl_pid, 9);
        u->ctl_pid = 0;
        svc_enter_dead(c, u, "timeout");
        break;
      }
    }
    /* processes that left without us seeing the exit (not our children) */
    if ((u->sub == S_STOP_SIGTERM || u->sub == S_STOP_SIGKILL) && !alive(c, u)) svc_stop_post(c, u);
    /* forking: a PID file names the main process before the starter exits */
    if (u->type == SVC_T_SERVICE && u->stype == ST_FORKING && u->sub == S_START && u->pidfile[0]) {
      int np = read_pidfile(c, u);
      if (np > 0) {
        int pids[64];
        int n = c->ops->members(0, np, pids, 64);
        if (n > 0) { u->main_pid = np; svc_running(c, u); }
      }
    }
    /* automatic restarts */
    if (u->sub == S_AUTO_RESTART && u->restart_at >= 0 && now >= u->restart_at) {
      u->restart_at = -1;
      u->nrestarts++;
      core_log(c, u, 6, "%s: Scheduled restart job, restart counter is at %d.", u->name, u->nrestarts);
      set_state(c, u, A_INACTIVE, S_DEAD);
      char m[200];
      core_request(c, u->name, J_START, "replace", -1, m, sizeof m);
    }
    /* timers */
    if (u->type == SVC_T_TIMER && u->active == A_ACTIVE && u->next_elapse >= 0 && now >= u->next_elapse) {
      u->last_trigger = now;
      u->next_elapse = -1;
      if (u->dep[D_TRIGGERS].n) {
        struct unit *s = c->u[u->dep[D_TRIGGERS].u[0]];
        if (s && !active_ish(s)) { char m[200]; core_request(c, s->name, J_START, "replace", -1, m, sizeof m); }
      }
      timer_arm(c, u);
    }
  }
  /* path units: polled */
  if (now >= c->next_path) {
    c->next_path = now + 1000;
    for (int i = 0; i < c->nu; i++) {
      struct unit *u = c->u[i];
      if (!u || u->type != SVC_T_PATH || u->active != A_ACTIVE || !u->dep[D_TRIGGERS].n) continue;
      struct unit *s = c->u[u->dep[D_TRIGGERS].u[0]];
      int fire = 0;
      for (int k = 0; k < u->nwatch; k++) {
        int kind = u->watchkind[k];
        if (kind == 0) fire |= c->ops->file_exists(u->watch[k], 0) == 0;
        else if (kind == 1) fire |= glob_exists(c, u->watch[k]);
        else if (kind == 4) fire |= c->ops->file_exists(u->watch[k], 3) == 0;
        else { int64_t m = c->ops->mtime(u->watch[k]); if (m != u->watch_mtime[k]) { u->watch_mtime[k] = m; fire = 1; } }
      }
      if (fire && s && !active_ish(s)) { char m[200]; core_request(c, s->name, J_START, "replace", -1, m, sizeof m); }
    }
  }
  /* jobs: run every one that is not waiting for another (repeat: one finishing frees others) */
  for (int pass = 0; pass < 64; pass++) {
    int ran = 0;
    for (int j = 0; j < MAXJOBS; j++) {
      if (!c->jobs[j].used || c->jobs[j].running == 1 || c->jobs[j].running == 2) continue;
      if (job_blocked(c, j)) continue;
      job_run(c, j);
      ran = 1;
    }
    if (!ran) break;
  }
  /* Jobs of separate requests can wait on each other in a circle. If nothing runs and
   * nothing can, start the oldest waiting job anyway. */
  {
    int running = 0, oldest = -1;
    for (int j = 0; j < MAXJOBS; j++) {
      if (!c->jobs[j].used) continue;
      if (c->jobs[j].running == 1 || c->jobs[j].running == 2) running = 1;
      else if (oldest < 0 || c->jobs[j].t < c->jobs[oldest].t) oldest = j;
    }
    if (!running && oldest >= 0) {
      core_log(c, c->u[c->jobs[oldest].unit], 4, "%s: Jobs are waiting on each other in a cycle; running %s/%s first.", c->u[c->jobs[oldest].unit]->name,
               c->u[c->jobs[oldest].unit]->name, jtnames[c->jobs[oldest].type]);
      job_run(c, oldest);
    }
  }
  /* idle services start when no other job is waiting */
  if (c->shutting_down) {
    int any = 0;
    for (int j = 0; j < MAXJOBS; j++) if (c->jobs[j].used) any = 1;
    if (!any) {
      if (c->shutting_down == 2) {
        c->shutting_down = 0;
        char m[200];
        core_log(c, 0, 6, "Soft reboot: starting %s again.", c->default_target);
        core_request(c, c->default_target, J_START, "replace", -1, m, sizeof m);
      } else c->quit = 1;
    }
  }
}

int64_t core_next_wakeup(struct core *c) {
  int64_t now = c->ops->now(), best = -1;
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u) continue;
    int64_t t[3] = {u->deadline, u->restart_at, u->type == SVC_T_TIMER && u->active == A_ACTIVE ? u->next_elapse : -1};
    for (int k = 0; k < 3; k++) if (t[k] >= 0 && (best < 0 || t[k] < best)) best = t[k];
    if (u->type == SVC_T_PATH && u->active == A_ACTIVE && (best < 0 || now + 1000 < best)) best = now + 1000;
    if (u->type == SVC_T_SERVICE && u->stype == ST_FORKING && u->sub == S_START && u->pidfile[0] && (best < 0 || now + 200 < best)) best = now + 200;
  }
  if (best < 0) return -1;
  return best > now ? best - now : 0;
}

void core_socket_ready(struct core *c, int fd) {
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || u->type != SVC_T_SOCKET || u->active != A_ACTIVE) continue;
    for (int k = 0; k < u->nlisten; k++) {
      if (u->lfd[k] != fd) continue;
      if (!u->accept) {
        u->listening = 0;
        u->sub = S_RUNNING;
        char m[200];
        if (core_request(c, u->svcname, J_START, "replace", -1, m, sizeof m) < 0) {
          core_log(c, u, 3, "%s: Failed to queue service startup job: %s", u->name, m);
          u->listening = 1; u->sub = S_LISTENING;
        }
        return;
      }
      /* Accept=yes: one instance per connection, with the connection on stdin/stdout */
      int conn = c->ops->accept(fd);
      if (conn < 0) return;
      char inst[160], tmpl[128];
      snprintf(tmpl, sizeof tmpl, "%s", u->svcname);
      char *at = strchr(tmpl, '@');
      if (!at) { c->ops->closefd(conn); return; }
      snprintf(inst, sizeof inst, "%.*s%d%s", (int)(at - tmpl + 1), tmpl, u->accept_n++ + 1, strrchr(tmpl, '.'));
      int si = core_unit(c, inst);
      if (si < 0) { c->ops->closefd(conn); return; }
      struct unit *s = c->u[si];
      s->transient = 1;
      s->in = OUT_SOCKET;
      s->accept_n = conn;
      if (s->out == OUT_JOURNAL) s->out = OUT_SOCKET;
      s->outfd = conn;
      s->outkeep = 1;
      char m[200];
      core_request(c, inst, J_START, "replace", -1, m, sizeof m);
      return;
    }
  }
}

/* ---------- reload, gc, kill, shutdown ---------- */
void core_reload(struct core *c) {
  core_log(c, 0, 6, "Reloading.");
  core_gc(c);
  for (int i = 0; i < c->nu; i++) if (c->u[i]) unit_free_config(c->u[i]);
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u) continue;
    /* keep runtime state; the socket listeners stay open */
    int lfd[4];
    memcpy(lfd, u->lfd, sizeof lfd);
    unit_load(c, u);
    if (u->type == SVC_T_SOCKET) memcpy(u->lfd, lfd, sizeof lfd);
    if (u->type == SVC_T_TIMER && u->active == A_ACTIVE) timer_arm(c, u);
  }
  core_gc(c);
}

int core_gc(struct core *c) {
  /* Units made for one connection (Accept=yes) are dropped once they are inactive, so
   * a busy socket does not grow the table. Other units stay, as loaded units do in
   * systemd while anything refers to them. */
  int freed = 0;
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || !u->transient || u->job >= 0 || u->active != A_INACTIVE) continue;
    for (int k = 0; k < c->nu; k++) {
      struct unit *o = c->u[k];
      if (!o || o == u) continue;
      for (int d = 0; d < D_NKINDS; d++) {
        int w = 0;
        for (int q = 0; q < o->dep[d].n; q++) if (o->dep[d].u[q] != i) o->dep[d].u[w++] = o->dep[d].u[q];
        o->dep[d].n = w;
      }
    }
    if (u->outfd >= 0 && u->outkeep) c->ops->closefd(u->outfd);
    unit_free_config(u);
    free(u);
    c->u[i] = 0;
    freed++;
  }
  return freed;
}

void core_reset_failed(struct core *c, const char *name) {
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || (name && strcmp(u->name, name))) continue;
    u->nstarts = 0; u->nrestarts = 0;
    if (u->active == A_FAILED) { strcpy(u->result, "success"); set_state(c, u, A_INACTIVE, S_DEAD); }
  }
}

int core_kill(struct core *c, const char *name, int sig, const char *who) {
  int i = core_find(c, name);
  if (i < 0) return -ENOENT;
  struct unit *u = c->u[i];
  if (u->type != SVC_T_SERVICE || (!u->main_pid && !u->pgid)) return -ESRCH;
  return kill_group(c, u, sig, who && !strcmp(who, "main")) ? 0 : -ESRCH;
}

void core_poweroff(struct core *c, int reboot) {
  c->shutting_down = reboot ? 2 : 1;
  core_log(c, 0, 6, reboot ? "Soft reboot: stopping all units." : "Powering off: stopping all units.");
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || !(active_ish(u))) continue;
    char m[200];
    core_request(c, u->name, J_STOP, "replace", -1, m, sizeof m);
  }
}

void core_measure(struct core *c) {
  for (int i = 0; i < c->nu; i++) {
    struct unit *u = c->u[i];
    if (!u || u->type != SVC_T_SERVICE || !(u->active == A_ACTIVE || u->active == A_RELOADING || u->active == A_ACTIVATING)) continue;
    int pids[64];
    int n = c->ops->members(u->pgid, u->main_pid, pids, 64);
    uint64_t total = 0;
    for (int k = 0; k < n; k++) total += (uint64_t)c->ops->pages(pids[k]) * 65536;
    u->mem_now = total;
    if (total > u->mem_peak) u->mem_peak = total;
    /* hooks for a memory policy (docs/SERVICES.md, "Memory"): today, log and enforce MemoryMax= */
    if (u->mem_high && total > u->mem_high && !u->mem_high_logged) {
      u->mem_high_logged = 1;
      core_log(c, u, 4, "%s: Memory use %llu bytes is above MemoryHigh=%llu.", u->name, (unsigned long long)total, (unsigned long long)u->mem_high);
    }
    if (u->mem_high && total <= u->mem_high) u->mem_high_logged = 0;
    if (u->mem_max && total > u->mem_max && u->active == A_ACTIVE) {
      core_log(c, u, 3, "%s: Memory use %llu bytes is above MemoryMax=%llu: killing the service.", u->name, (unsigned long long)total, (unsigned long long)u->mem_max);
      strcpy(u->result, "oom-kill");
      svc_enter_signal(c, u, 1);
    }
  }
}

/* ---------- show ---------- */
static void fmt_dep(struct core *c, struct unit *u, int kind, const char *key, void (*emit)(void *, const char *), void *ctx) {
  char line[2048];
  int n = snprintf(line, sizeof line, "%s=", key);
  for (int i = 0; i < u->dep[kind].n && n < (int)sizeof line - 130; i++) {
    struct unit *o = c->u[u->dep[kind].u[i]];
    if (o) n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", o->name);
  }
  emit(ctx, line);
}
void core_show(struct core *c, struct unit *u, void (*emit)(void *ctx, const char *line), void *ctx) {
  char l[1024];
#define E(...) do { snprintf(l, sizeof l, __VA_ARGS__); emit(ctx, l); } while (0)
  int64_t now = c->ops->now(), ep = c->ops->epoch();
  E("Id=%s", u->name);
  E("Names=%s", u->name);
  E("Description=%s", u->desc);
  E("LoadState=%s", core_load_str(u->load));
  E("ActiveState=%s", core_active_str(u->active));
  E("SubState=%s", core_sub_str(u));
  E("FragmentPath=%s", u->path);
  if (u->dropins) { char d[900]; snprintf(d, sizeof d, "%s", u->dropins); for (char *p = d; *p; p++) if (*p == '\n') *p = ' '; size_t dl = strlen(d); if (dl && d[dl - 1] == ' ') d[dl - 1] = 0; E("DropInPaths=%s", d); }
  else E("DropInPaths=");
  E("UnitFileState=%s", u->load == L_LOADED ? (svc_is_enabled(&c->roots, u->name) ? svc_is_enabled(&c->roots, u->name) : "") : "");
  E("Result=%s", u->result);
  E("Guest=%s", u->root > 0 ? "yes" : "no");
  /* times: monotonic ms since manager start and wall clock (UTC ms) */
  if (u->t_enter) E("StateChangeTimestamp=%lld", (long long)(ep - (now - u->t_enter)));
  if (u->t_active) E("ActiveEnterTimestamp=%lld", (long long)(ep - (now - u->t_active)));
  if (u->t_inactive) E("InactiveEnterTimestamp=%lld", (long long)(ep - (now - u->t_inactive)));
  fmt_dep(c, u, D_REQUIRES, "Requires", emit, ctx);
  fmt_dep(c, u, D_WANTS, "Wants", emit, ctx);
  fmt_dep(c, u, D_REQUISITE, "Requisite", emit, ctx);
  fmt_dep(c, u, D_BINDSTO, "BindsTo", emit, ctx);
  fmt_dep(c, u, D_PARTOF, "PartOf", emit, ctx);
  fmt_dep(c, u, D_CONFLICTS, "Conflicts", emit, ctx);
  fmt_dep(c, u, D_AFTER, "After", emit, ctx);
  fmt_dep(c, u, D_BEFORE, "Before", emit, ctx);
  fmt_dep(c, u, D_ONFAILURE, "OnFailure", emit, ctx);
  fmt_dep(c, u, D_TRIGGERS, "Triggers", emit, ctx);
  fmt_dep(c, u, D_TRIGGEREDBY, "TriggeredBy", emit, ctx);
  fmt_dep(c, u, D_REQUIREDBY, "RequiredBy", emit, ctx);
  fmt_dep(c, u, D_WANTEDBY, "WantedBy", emit, ctx);
  fmt_dep(c, u, D_CONFLICTEDBY, "ConflictedBy", emit, ctx);
  if (u->job >= 0) E("Job=%d %s", u->job, jtnames[c->jobs[u->job].type]);
  if (u->type == SVC_T_SERVICE) {
    static const char *const tn[] = {"simple", "exec", "forking", "oneshot", "notify", "dbus", "idle"};
    static const char *const rn[] = {"no", "always", "on-success", "on-failure", "on-abnormal", "on-abort", "on-watchdog"};
    static const char *const kn[] = {"control-group", "mixed", "process", "none"};
    E("Type=%s", tn[u->stype]);
    E("Restart=%s", rn[u->restart]);
    E("RestartUSec=%lld", (long long)u->restart_ms * 1000);
    E("NRestarts=%d", u->nrestarts);
    E("RemainAfterExit=%s", u->remain ? "yes" : "no");
    E("KillMode=%s", kn[u->killmode]);
    E("KillSignal=%d", u->killsig);
    E("TimeoutStartUSec=%s%lld", u->tstart_ms == SVC_INFINITY ? "infinity" : "", u->tstart_ms == SVC_INFINITY ? 0LL : (long long)u->tstart_ms * 1000);
    E("TimeoutStopUSec=%s%lld", u->tstop_ms == SVC_INFINITY ? "infinity" : "", u->tstop_ms == SVC_INFINITY ? 0LL : (long long)u->tstop_ms * 1000);
    E("MainPID=%d", u->main_pid);
    E("ControlPID=%d", u->ctl_pid);
    E("ControlGroup=pgid:%d", u->pgid);
    E("ExecMainCode=%d", u->exit_code);
    E("ExecMainStatus=%d", u->exit_status);
    E("StatusText=%s", u->status_text);
    E("MemoryCurrent=%llu", (unsigned long long)u->mem_now);
    E("MemoryPeak=%llu", (unsigned long long)u->mem_peak);
    E("MemoryHigh=%s%llu", u->mem_high ? "" : "infinity", u->mem_high ? (unsigned long long)u->mem_high : 0ULL);
    E("MemoryMax=%s%llu", u->mem_max ? "" : "infinity", u->mem_max ? (unsigned long long)u->mem_max : 0ULL);
    static const char *const xn[] = {"ExecStartPre", "ExecStart", "ExecStartPost", "ExecReload", "ExecStop", "ExecStopPost", "ExecCondition"};
    for (int k = 0; k < X_N; k++)
      for (int i = 0; i < u->exec[k].n; i++) E("%s=%s", xn[k], u->exec[k].v[i]);
    if (u->pidfile[0]) E("PIDFile=%s", u->pidfile);
    if (u->pgid > 0) {
      int pids[64];
      int n = c->ops->members(u->pgid, u->main_pid, pids, 64);
      char b[600];
      int bl = 0;
      for (int i = 0; i < n && bl < 500; i++) bl += snprintf(b + bl, sizeof b - bl, "%s%d", i ? " " : "", pids[i]);
      b[bl] = 0;
      E("Processes=%s", b);
    }
  }
  if (u->type == SVC_T_TIMER) {
    if (u->next_elapse >= 0) E("NextElapseUSecMonotonic=%lld", (long long)(u->next_elapse - c->boot_ms) * 1000);
    if (u->next_elapse >= 0) E("NextElapse=%lld", (long long)(ep + (u->next_elapse - now)));
    if (u->last_trigger >= 0) E("LastTrigger=%lld", (long long)(ep - (now - u->last_trigger)));
  }
  if (u->type == SVC_T_SOCKET) {
    for (int i = 0; i < u->nlisten; i++) E("Listen=%s (Stream)", u->listen[i]);
    E("Accept=%s", u->accept ? "yes" : "no");
  }
#undef E
}

void core_init(struct core *c, struct svc_ops *ops, const char *debian_root) {
  memset(c, 0, sizeof *c);
  c->ops = ops;
  svc_roots_default(&c->roots, debian_root);
  c->boot_ms = ops->now();
  c->def_tstart = 90000; c->def_tstop = 90000; c->def_restart = 100;
  c->def_limit_interval = 10000; c->def_limit_burst = 5;
  strcpy(c->engine, "/bin/lx");
  strcpy(c->default_target, "default.target");
}
