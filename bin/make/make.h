// SPDX-License-Identifier: GPL-2.0-or-later
/* make: a make for UC.OS that aims at compatibility with GNU Make.
 * Shared declarations. The program is plain C plus a thin system layer
 * (sys.c) so that it builds natively for testing and for UC.OS. */
#ifndef MAKE_H
#define MAKE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

/* ---- memory and strings (util.c) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t a, size_t b);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xconcat(const char *a, const char *b, const char *c);

typedef struct buf { char *s; size_t n, cap; } buf;
void bgrow(buf *b, size_t extra);
void bput(buf *b, const char *s, size_t n);
void bputs(buf *b, const char *s);
void bputc(buf *b, int c);
char *bstr(buf *b); /* NUL-terminate and return the text (still owned by b) */
char *bdetach(buf *b); /* NUL-terminate and hand the text to the caller */
void bfree(buf *b);

typedef struct strlist { char **v; int n, cap; } strlist;
void sl_add(strlist *l, char *s); /* takes ownership */
void sl_free(strlist *l);

/* open addressing hash table, string keys */
typedef struct hent { char *key; unsigned h; void *val; } hent;
typedef struct htab { hent *t; unsigned cap, n; } htab;
unsigned hhash(const char *k, size_t n);
void *hget(htab *h, const char *k, size_t n);
void **hput(htab *h, const char *k, size_t n, int *isnew); /* slot for the value */

static inline int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static inline int is_blank(int c) { return c == ' ' || c == '\t'; }
const char *skip_ws(const char *p);
const char *next_word(const char **p, size_t *len); /* NULL at end */
void strip_into(buf *out, const char *s, size_t n); /* collapse whitespace */
const char *find_percent(const char *s); /* first unescaped %, or NULL */
char *unescape_percent(const char *s, const char **pct); /* copy without \% escapes; *pct = the % */
int pattern_match(const char *pat, const char *pct, const char *s, size_t sn, size_t *stem_off, size_t *stem_len);
void pattern_subst(buf *out, const char *pat, const char *pct, const char *stem, size_t sl);
void path_dir(buf *out, const char *s, size_t n);    /* "dir/" or "./" */
void path_notdir(buf *out, const char *s, size_t n);
char *abspath_of(const char *p, const char *cwd);
int glob_expand(const char *pat, strlist *out); /* returns number of matches added (sorted) */
int has_glob_chars(const char *s);
int str_cmp(const void *a, const void *b);

/* ---- variables and expansion (var.c) ---- */
enum origin { O_DEFAULT, O_ENV, O_FILE, O_ENV_OVERRIDE, O_COMMAND, O_OVERRIDE, O_AUTOMATIC };
enum flavor { F_RECURSIVE, F_SIMPLE };
enum exp { EXP_DEFAULT, EXP_YES, EXP_NO };
typedef struct var {
  char *name, *value;
  size_t vlen, vcap; /* length of value, and its allocation (0: exact) */
  const char *file; int line;
  unsigned char flavor, origin, exp, append, priv, expanding, conditional, defined_in_define;
} var;
typedef struct vset { htab h; } vset;
typedef struct scope { vset *set; struct scope *next; int priv_ok; } scope;

extern vset globals;
extern scope *cur_scope;        /* target-specific chain while making a target */
extern struct file *auto_file;  /* automatic variables come from here */
extern int auto_2nd;            /* second expansion: limited automatic variables */

var *var_lookup(const char *name, size_t n);     /* whole visible chain */
var *var_lookup_global(const char *name, size_t n);
var *vset_lookup(vset *s, const char *name, size_t n);
var *var_set(vset *s, const char *name, const char *value, int flavor, int origin);
var *var_define(const char *name, const char *value, int flavor, int origin); /* global, honours priority */
/* op: '=' ':' (:= ::=) '+' '?' '!' and '3' for :::= */
var *var_assign(vset *s, const char *name, const char *rawval, int op, int origin, int is_target_var);
char *var_value(var *v); /* expanded value, malloc'ed */
void expand_into(buf *out, const char *s, size_t n);
char *expand(const char *s); /* malloc'ed */
char *expandn(const char *s, size_t n);
char *expand_var_named(const char *name); /* $(name), malloc'ed */
long find_close(const char *s, long i, char open); /* index of the matching close paren */
void var_undefine(const char *name, int origin);
int var_is_auto(const char *name, size_t n);

/* temporary variables: foreach, let, call arguments */
typedef struct tmpvar { const char *name; size_t nlen; var v; struct tmpvar *next; int is_call; int nargs; } tmpvar;
extern tmpvar *tmpvars;

/* ---- functions (func.c) ---- */
int call_function(buf *out, const char *name, size_t nlen, const char *args, size_t alen, char open);
int is_function_name(const char *s, size_t n);
char *run_shell_capture(const char *cmd, int *status, int one_newline);

/* ---- files and rules (rule.c) ---- */
typedef struct dep {
  struct dep *next;
  struct file *f;
  char *name;        /* pattern deps and 2nd expansion: the text */
  unsigned char oo;  /* order-only */
  unsigned char need2; /* needs second expansion */
  unsigned char wait; /* .WAIT before this one */
  unsigned char implicit; /* came from an implicit rule */
  unsigned char deferred; /* a missing intermediate, made only if needed */
} dep;

typedef struct cmds {
  const char *file;
  int line;          /* line of the first recipe line */
  int n;
  char **lines;      /* logical lines, unexpanded */
  int *lineno;
  int recipe_prefix;
} cmds;

enum { ST_NEW, ST_VISIT, ST_RUNNING, ST_DONE };
typedef int64_t mtime_t;
#define MT_UNKNOWN ((mtime_t)-1)
#define MT_MISSING ((mtime_t)1)
#define MT_OLD ((mtime_t)2)
#define MT_NEW INT64_MAX

typedef struct file {
  char *name;
  char *vpath;            /* the name found through directory search */
  dep *deps, **deptail;
  cmds *cmds;
  struct file *dcnext;    /* more double-colon rules for the same name */
  struct file *dchead;    /* for those entries: the main file */
  vset *vars;
  scope *sc;              /* built when the file is made */
  struct file *parent;
  char *stem;
  struct file **also; int nalso; /* made together (grouped / pattern rule) */
  struct rule *irule;
  mtime_t mtime;          /* MT_UNKNOWN until stat'ed */
  mtime_t mtime_before;
  int dcursor, dcursor2;
  unsigned char phase, must;
  unsigned char state, status;
  unsigned phony:1, precious:1, intermediate:1, secondary:1, notintermediate:1, is_target:1,
      mentioned:1, dc:1, tried_impl:1, ran:1, remade:1, silent:1, ignore:1, oldfile:1, newfile:1,
      grouped:1, is_makefile:1, dontcare:1, created_by_chain:1, changed:1, deleted_ok:1,
      cmd_target:1, also_done:1, explicit_dep:1, from_impl:1, in_db:1, being_made:1, needs2:1,
      skipped:1, grpdone:1, grpfail:1, is_goal:1, existed:1;
} file;

typedef struct rule {
  int nt;
  char **targets;       /* target patterns */
  const char **pcts;    /* the % in each */
  dep *deps;            /* names are patterns */
  cmds *cmds;
  unsigned terminal:1, builtin:1, in_use:1, from_suffix:1, need2:1;
  int seq;
} rule;

extern htab files;
extern rule **rules; extern int nrules;
file *file_lookup(const char *name);
file *file_lookupn(const char *name, size_t n);
file *file_enter(const char *name);
file *file_entern(const char *name, size_t n);
dep *dep_new(file *f);
void dep_append(file *t, dep *d);
mtime_t file_mtime(file *f);
mtime_t stat_mtime(const char *path);
void stat_forget(const char *path);
int file_exists_or_vpath(file *f);
void rule_add(rule *r, int override);
void rules_finish(void); /* suffix rules to pattern rules, built-in rules */
int implicit_search(file *f, int depth);
void vpath_add(const char *pattern, const char *dirs);
char *vpath_search(const char *name);
int is_suffix(const char *s, size_t n);
char *suffix_stem(const char *name); /* $* for explicit rules */
void build_scope(file *f);
void pattern_var_add(const char *pattern, char *line_rest, int origin, int export_kind, int priv);

/* ---- reading makefiles (read.c) ---- */
void read_makefile(const char *name, int flags);
void read_text(const char *text, const char *name, int line); /* $(eval) */
extern const char *read_file_name; extern int read_line;
extern strlist makefiles_read;
typedef struct incl { char *name; const char *from; int line; int optional; } incl;
extern incl *missing_incs; extern int nmissing_incs;
extern int all_silent, all_ignore;
extern int second_expansion, oneshell, posix_mode, export_all, notparallel, delete_on_error, all_secondary, all_notint;
extern int recipe_prefix;
extern char **suffixes; extern int nsuffixes;
void set_suffixes_default(void);
void record_default_goal(const char *name);
void parse_var_line(const char *s, vset *set, int origin, int is_target);
int find_assign(const char *s, size_t *name_end, size_t *val_start, int *op);
extern const char *builtin_makefile;

/* ---- making (run.c) ---- */
int update_goals(file **goals, int n, int makefiles_mode);
int run_jobs_until_idle(void);
void delete_intermediates(void);
void flush_deferred_msgs(void);
void die_cleanup(void);
char **build_env(file *f);
extern int commands_started;
extern int job_slots;
void print_targets_db(void);
void handle_interrupt(int sig);

/* ---- main.c: options and messages ---- */
extern const char *progname;
extern int makelevel;
extern int opt_n, opt_s, opt_k, opt_i, opt_B, opt_q, opt_t, opt_r, opt_R, opt_e, opt_p, opt_d, opt_w, opt_no_w, opt_trace, opt_warn_undef, opt_j_given, opt_print_dir;
extern char *opt_C_dirs;
extern char *make_cwd;
extern strlist include_dirs;
extern char *make_path;  /* $(MAKE) value */
extern int restarts;
void fatal_at(const char *file, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void error_at(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));       /* make: ... on stdout */
void errmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));    /* make: ... on stderr */
void warn_at(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void make_exit(int code) __attribute__((noreturn));
void enter_dir_msg(int leaving);
char *prefix_str(void); /* "make" or "make[1]" */
void make_flags_string(buf *out, int for_mflags);

/* ---- system layer (sys.c) ---- */
typedef struct sysstat { int exists, is_dir; mtime_t mtime; } sysstat;
int sys_stat(const char *path, sysstat *st);
int sys_spawn(char **argv, char **envp, int fd_out); /* fd_out: -1 inherit; returns pid or -errno */
int sys_spawn_path(char **argv, char **envp, const char *path_var); /* PATH search; -ENOENT if not found */
int sys_wait_any(int *status, int block); /* pid, 0 if none ready, -1 if no children */
int sys_waitpid(int pid, int *status);
int sys_wait_status(int status, int *exited, int *code, int *sig);
int sys_pipe(int fds[2]);
int sys_read(int fd, char *b, int n);
int sys_close(int fd);
int sys_chdir(const char *d);
char *sys_getcwd(void);
int sys_touch(const char *path); /* create or set mtime to now */
int sys_unlink(const char *path);
int sys_isdir(const char *path);
void sys_list_dir(const char *dir, strlist *out); /* names in dir, without . and .. */
void sys_install_signals(void);
int sys_interrupted(void);
int sys_kill(int pid, int sig);
void sys_reexec(char **argv) __attribute__((noreturn));
extern char **environ;
#endif
