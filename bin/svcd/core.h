// SPDX-License-Identifier: GPL-2.0-or-later
/* The service manager's core: units, dependencies, jobs, the service state machine,
 * timers, sockets, paths. Plain C with no system calls of its own: everything that
 * touches processes, time or the outside goes through struct svc_ops, so the same
 * code runs under UC.OS (main.c), on a POSIX host, and under the deterministic fake
 * of the native tests (libsvc/tests/test_core.c). */
#ifndef SVCD_CORE_H
#define SVCD_CORE_H
#include "../../libsvc/svc.h"

enum { L_STUB, L_LOADED, L_NOTFOUND, L_MASKED, L_BAD };
enum { A_INACTIVE, A_ACTIVATING, A_ACTIVE, A_RELOADING, A_DEACTIVATING, A_FAILED };
/* dependency kinds; the second half are the reverse edges, filled in by the core */
enum { D_REQUIRES, D_WANTS, D_REQUISITE, D_BINDSTO, D_PARTOF, D_CONFLICTS, D_AFTER, D_BEFORE, D_ONFAILURE,
       D_TRIGGERS, D_PROPRELOAD,
       D_REQUIREDBY, D_WANTEDBY, D_REQUISITEOF, D_BOUNDBY, D_CONSISTSOF, D_CONFLICTEDBY, D_TRIGGEREDBY,
       D_RELOADPROPTO, D_NKINDS };
enum { J_START, J_STOP, J_RESTART, J_RELOAD, J_TRYRESTART, J_VERIFY };
enum { ST_SIMPLE, ST_EXEC, ST_FORKING, ST_ONESHOT, ST_NOTIFY, ST_DBUS, ST_IDLE };
enum { RS_NO, RS_ALWAYS, RS_SUCCESS, RS_FAILURE, RS_ABNORMAL, RS_ABORT, RS_WATCHDOG };
enum { KM_CGROUP, KM_MIXED, KM_PROCESS, KM_NONE };
enum { X_PRE, X_START, X_POST, X_RELOAD, X_STOP, X_STOPPOST, X_CONDITION, X_N };
enum { OUT_JOURNAL, OUT_NULL, OUT_INHERIT, OUT_FILE, OUT_APPEND, OUT_CONSOLE_TOO, OUT_SOCKET };

struct unit;
struct svc_ops {
  int64_t (*now)(void);   /* monotonic ms */
  int64_t (*epoch)(void); /* UTC ms */
  /* Start a process for u. out, err: descriptors for stdout and stderr (-1: /dev/null);
   * in: for stdin (-1: /dev/null, -2: the manager's own stdin, the console). pgid: 0 =
   * new group, else join. Returns pid or -errno. */
  int (*spawn)(struct unit *u, const char *path, char *const argv[], char *const envp[], int in, int out, int err,
               int pgid, const int *extra, int nextra);
  int (*kill)(int pid, int sig);
  int (*members)(int pgid, int main_pid, int *pids, int max); /* live processes of a group */
  uint32_t (*pages)(int pid);                                  /* memory of one process, 64 KiB pages */
  void (*log)(const char *unit, int prio, const char *msg);     /* the journal */
  /* Descriptor for a new process's stdout (which 1) or stderr (2): a fresh journal
   * pipe, a file, the console, or -1 for /dev/null. *keep: do not close it after the
   * spawn (the console, a connection). */
  int (*open_output)(struct unit *u, int which, int *keep);
  void (*spawned)(struct unit *u, int pid, const char *ident); /* the streams just opened belong to pid */
  int (*listen)(const char *path);                              /* socket units: listening AF_UNIX socket */
  int (*accept)(int lfd);                                       /* Accept=yes: one connection */
  void (*closefd)(int fd);
  int (*file_exists)(const char *path, int what);               /* 0 exists, 1 dir, 2 x, 3 nonempty dir, 4 nonempty file, 5 symlink, 6 writable */
  int64_t (*mtime)(const char *path);
  void (*mkdirs)(const char *path);
  void (*rmtree)(const char *path);
  int (*read_file)(const char *path, char *buf, int len);
  uint32_t (*random)(void);
};

struct cmdlist { char **v; int n; };   /* raw Exec values, expanded when run */
struct dep { int *u; int n, cap; };

struct unit {
  char name[128];
  char desc[160];
  int type, load, root;
  char path[256];                 /* fragment */
  char *dropins;                  /* newline separated, for status/cat */
  struct svc_file f;
  struct dep dep[D_NKINDS];
  int default_deps, ignore_on_isolate, allow_isolate, refuse_manual_start, refuse_manual_stop;
  char *conditions;               /* "Key\tvalue\n" lines, Condition* and Assert* */
  /* service */
  int stype, restart, remain, killmode, killsig, finalsig, sendkill, guess;
  int notify_access;
  int64_t restart_ms, restart_max_ms, tstart_ms, tstop_ms;
  int restart_steps;
  int64_t limit_interval_ms; int limit_burst;
  struct cmdlist exec[X_N];
  char *env;                      /* Environment= words, "\n" separated */
  char *envfiles;                 /* EnvironmentFile= values */
  char workdir[200], pidfile[200];
  int out, err, in;
  char outpath[200], errpath[200];
  int succ_status[8], nsucc, prevent_status[8], nprevent, force_status[8], nforce;
  char *rtdirs, *statedirs, *otherdirs;
  int rt_preserve;
  uint64_t mem_high, mem_max;     /* bytes, 0 = none */
  /* timer */
  int64_t on_boot, on_startup, on_active, on_unit_active, on_unit_inactive, rand_delay;
  struct svc_cal cal[4]; int ncal;
  /* socket */
  char listen[4][108]; int nlisten, accept;
  int lfd[4];
  char svcname[128];
  /* path */
  char watch[4][200]; int watchkind[4], nwatch; int64_t watch_mtime[4]; int makedir;

  /* runtime */
  int active, sub;                /* sub: a string index, see core.c */
  int main_pid, ctl_pid, pgid;
  int ctl_kind, ctl_idx;          /* which Exec list, which entry */
  int64_t deadline;               /* -1 none */
  char result[24];                /* "success", "exit-code", "signal", "timeout", ... */
  int exit_code, exit_status;     /* CLD_EXITED (1) / CLD_KILLED (2), and the code or signal */
  int job;                        /* index into jobs, -1 none */
  int64_t restart_at;
  int nrestarts;
  int64_t starts[16]; int nstarts; /* start times for the start limit */
  int outfd, outkeep;             /* journal pipe read end (in main.c's table) */
  int64_t t_active, t_inactive, t_enter, t_activated;
  int64_t next_elapse, last_trigger; /* timers: monotonic deadline; -1 none */
  int64_t next_real;              /* timers: UTC ms of a calendar elapse, for display */
  int listening;                  /* sockets: polling the listeners */
  int notify_ready, reload_pending, stop_after_start;
  uint64_t mem_now, mem_peak; int mem_high_logged;
  int accept_n;
  int transient;
  int success_action, failure_action; /* 0 none, 1 poweroff (also halt, exit), 2 reboot */                  /* made for one connection; dropped when inactive */
  int status_errno;
  char status_text[100];
};

struct job {
  int used, unit, type, running, essential, anchor;
  int waiters[8]; int nwait;      /* client ids to tell when done */
  char result[16];
  int64_t t;
};

#define MAXUNITS 768
#define MAXJOBS 256
struct core {
  struct svc_ops *ops;
  struct svc_roots roots;
  struct unit *u[MAXUNITS]; int nu;
  struct job jobs[MAXJOBS];
  int64_t boot_ms;
  int have_signals;
  int64_t def_tstart, def_tstop, def_restart, def_limit_interval; int def_limit_burst;
  char engine[64];                /* guest program engine, "/bin/lx" */
  char default_target[128];
  int shutting_down;              /* 1 poweroff, 2 reboot (soft) */
  int quit;                       /* the loop should end (after poweroff) */
  void (*job_done)(struct core *c, int client, const char *unit, int type, const char *result);
  char cycle_msg[512];
  int nodes_loaded;
  int64_t next_path;
};

void core_init(struct core *c, struct svc_ops *ops, const char *debian_root);
int core_unit(struct core *c, const char *name);              /* find or load; index or -1 */
int core_find(struct core *c, const char *name);              /* index or -1, no loading */
/* Queue a job (mode: "replace", "fail", "isolate"); client gets job_done. Returns 0, or
 * -errno with msg filled (systemctl prints it). */
int core_request(struct core *c, const char *name, int type, const char *mode, int client, char *msg, int ml);
void core_run(struct core *c);                                /* dispatch runnable jobs, timers, deadlines */
int64_t core_next_wakeup(struct core *c);                     /* ms from now, -1 for none */
void core_child_exit(struct core *c, int pid, int status);    /* a waitpid result */
int core_notify(struct core *c, int pid, const char *msg);    /* READY=1 etc.; -1 if no unit takes it */
void core_socket_ready(struct core *c, int fd);               /* a listener has a connection */
void core_reload(struct core *c);                             /* daemon-reload */
void core_reset_failed(struct core *c, const char *name);
int core_kill(struct core *c, const char *name, int sig, const char *who);
void core_poweroff(struct core *c, int reboot);
void core_measure(struct core *c);                            /* memory of every running unit */
const char *core_active_str(int a);
const char *core_sub_str(struct unit *u);
const char *core_load_str(int l);
int core_unit_tracks(struct unit *u, int pid);
/* Properties for `show` and `status`: calls emit for each "Key=Value". */
void core_show(struct core *c, struct unit *u, void (*emit)(void *ctx, const char *line), void *ctx);
int core_gc(struct core *c);
void core_log(struct core *c, struct unit *u, int prio, const char *fmt, ...);
#endif
