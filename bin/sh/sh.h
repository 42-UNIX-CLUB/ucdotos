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
enum { R_IN, R_OUT, R_APPEND, R_DUPIN, R_DUPOUT, R_BOTH, R_BOTHAPP, R_RDWR, R_HEREDOC };
struct redir { int fd, type, quoted; char *word; struct redir *next; };
struct casei { char **pats; int npats; struct node *body; struct casei *next; };
typedef struct node {
  int type, bang, nwords, nassigns, nkids;
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
  int tok, tokfd, tokredir, peeked;
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
int wait_jobs(void);
extern int last_status, last_bg_pid, interactive, opt_x, brk, cont, retflag, in_func;
extern int exit_pending, exit_code, exec_keep, loop_depth, intr_pending;
int check_intr(void);

/* builtin.c */
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn find_builtin(const char *name);
const char *builtin_name(int i);
int is_special_word(const char *s);

/* vars (main.c) */
char *getvar(const char *name);
void setvar(const char *name, const char *val, int exp);
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
