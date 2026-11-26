// SPDX-License-Identifier: GPL-2.0-or-later
/* sh: a POSIX-style shell for a system without fork. Shared declarations. */
#ifndef SH_H
#define SH_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

/* ---- memory arena: a parse tree lives in one arena and is freed at once ---- */
struct achunk { struct achunk *next; size_t used, cap; char data[]; };
struct arena { struct achunk *head; };
void *amalloc(struct arena *a, size_t n);
char *astrndup(struct arena *a, const char *s, size_t n);
void afree(struct arena *a);

/* ---- syntax tree ---- */
enum { N_CMD, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_GROUP, N_SUBSHELL, N_FUNC };
enum { R_IN, R_OUT, R_APPEND, R_DUPIN, R_DUPOUT, R_BOTH, R_BOTHAPP, R_RDWR, R_HEREDOC, R_CLOBBER };
struct redir { int fd, type, quoted; char *word; struct redir *next; };
struct casei { char **pats; int npats; struct node *body; struct casei *next; };
typedef struct node {
  int type, bang, nwords, nassigns, nkids, lineno;
  char **words, **assigns; /* N_CMD argv and NAME=value prefixes; N_FOR word list */
  struct node *a, *b, *c;  /* children (see exec.c) */
  struct node **kids;      /* N_PIPE stages */
  char *name;              /* N_FOR variable, N_FUNC name */
  struct casei *cases;
  struct redir *redir;
} node;

/* parse.c */
enum { P_OK, P_END, P_INCOMPLETE, P_ERROR };
struct parser {
  const char *s;
  size_t pos;
  struct arena *ar;
  int status, keep, noalias; /* keep: the tree defines a function, so the arena must live on */
  int tok, tokfd, tokredir, peeked, alias_next;
  size_t tokstart, lnpos, alias_end; /* LINENO: newlines are counted up to tokstart */
  const char *lns;
  int lnline;
  char *tokstr;
  struct redir *heredocs[16];
  int nheredocs;
  char *alias_seen[16];
  int nalias_seen;
  char errmsg[128];
};
void parser_init(struct parser *p, const char *s, struct arena *ar);
node *parse_one(struct parser *p); /* one complete command; sets p->status */
long subst_end(const char *s, long i);

/* expand.c */
int expand_words(char **raw, int n, char ***out); /* split + glob; returns argc, out is malloc'ed */
char *expand_str(const char *raw, int heredoc);   /* no split, no glob; malloc'ed */
char *expand_pattern(const char *raw, unsigned char **q); /* keeps quote flags for matching */
int pmatch(const char *p, const char *s);
long long arith(const char *expr, int *err);
void free_argv(char **v);

/* exec.c */
int run(node *n);
int run_text(const char *text, const char *name);
char *capture(const char *text, int *status); /* $(...) */
void sh_write(int fd, const char *s, size_t n);
void outs(int fd, const char *s);
void outf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sh_exit(int code) __attribute__((noreturn));
char *find_in_path(const char *name);
void reap_jobs(int report);
extern int last_status, last_bg_pid, interactive, opt_x, brk, cont, retflag, in_func;
extern int exit_pending, exit_code, exec_keep, loop_depth, intr_pending;
extern int lineno, dot_depth, cmd_nest;
int check_intr(void);
int exec_cmd(int argc, char **argv);  /* the exec builtin with a command */
char *find_in_path2(const char *name, const char *path);
int wait_for(pid_t pid, int *found);  /* wait builtin: status of one child */
int wait_jobs(void);
int job_pids(const char *spec, pid_t *out, int max); /* %N job spec -> pids */
int job_kill(const char *spec, int sig);             /* -2: no such job */
int job_fg(const char *spec);
int job_bg(const char *spec);
int job_list(int argc, char **argv);
int job_disown(int argc, char **argv);
void jobs_init(int tty);
int job_control_active(void);
extern int jc_ok;
void sub_saw_cd(void);               /* before cd: let an enclosing subshell save the directory */
void sub_saw_umask(int old);         /* before umask: the same for the mask */

/* options (builtin.c): set -e -u -x -f -n -v -a -C, and set -o names */
extern int opt_e, opt_u, opt_f, opt_n, opt_v, opt_a, opt_C, opt_m, opt_pipefail, noerrexit;
int parse_args(int argc, char **argv, const char **cmd, int *force_i, int *read_stdin);
const char *dash_flags(void);
void shell_start(void);
int run_exit_trap(int code);
void run_pending_traps(void);
extern volatile int traps_waiting;
struct trapsave { char *t[65]; int owned; };
void traps_enter(struct trapsave *s);
void traps_leave(struct trapsave *s);
struct optsave { int v[16]; };
void opts_save(struct optsave *o);
void opts_restore(struct optsave *o);
void trap_forget_exit(void);

/* builtin.c */
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn find_builtin(const char *name);
int is_special_builtin(const char *name);
extern int special_error, assign_error;
const char *builtin_name(int i);
int is_special_word(const char *s);

/* vars (main.c) */
char *getvar(const char *name);
void setvar(const char *name, const char *val, int exp);
int is_readonly(const char *name);
void set_readonly(const char *name);
void list_readonly(void);
void unsetvar(const char *name);
void vars_enter(void);
void vars_leave(void);
void export_var(const char *name);
int valid_name(const char *s, size_t n);
char **build_env(void);
void list_vars(int exported_only);
node *getfunc(const char *name);
void setfunc(const char *name, node *body);
void unsetfunc(const char *name);
char *getalias(const char *name);
void setalias(const char *name, const char *val);
int unalias(const char *name);
void unalias_all(void);
void list_aliases(void);
struct frame { int argc; char **argv; struct local *locals; struct frame *up; };
extern struct frame *frame;
extern char *shell_name;
void local_var(const char *name);
void history_add(const char *line);
void history_list(void);
char *read_file(const char *path, size_t *len);
#define NHIST 500
extern char *hist[NHIST];
extern int nhist;

/* tty.c: interactive line editor front end */
int tty_init(void);
char *tty_readline(const char *prompt);
void tty_restore(void);
char *expand_prompt(const char *ps);

#endif
