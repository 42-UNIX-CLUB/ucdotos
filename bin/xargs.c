// SPDX-License-Identifier: GPL-2.0-or-later
/* xargs [-0rtx] [-d DELIM] [-E EOF] [-I REPL] [-L N] [-n N] [-s SIZE] [-P 1] [command [arg...]]
 * Reads arguments from standard input (blanks and newlines separate them;
 * quotes and backslashes escape, except with -0 or -d) and runs the command
 * (default echo) with as many as fit. Commands run one at a time: -P only
 * accepts 1 (there is no parallel execution here).
 * Exit status: 0, 123 a command failed, 124 a command exited 255,
 * 125 a command was killed, 126 cannot run, 127 not found, 1 other errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#ifndef __wasm__
#include <spawn.h>
#endif

static int zero, no_empty, trace, exit_on_size;
static int delim = -1;
static const char *eofstr, *repl;
static long max_args = -1, max_lines = -1, max_size;
static char **base;
static int nbase;

static pid_t run_cmd(char **argv) {
#ifdef __wasm__
  return spawnp(argv[0], argv, NULL);
#else
  extern char **environ;
  pid_t pid;
  int e = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
  if (e) { errno = e; return -1; }
  return pid;
#endif
}

static int status = 0;
static int execute(char **args, int n) {
  char **argv = malloc((nbase + n + 2) * sizeof *argv);
  int k = 0;
  if (repl) {
    /* -I: each initial argument with every REPL replaced by the input line */
    size_t rl = strlen(repl);
    for (int i = 0; i < nbase; i++) {
      const char *s = base[i];
      size_t cap = strlen(s) + 1, cnt = 0;
      for (const char *p = strstr(s, repl); p && rl; p = strstr(p + rl, repl)) cnt++;
      cap += cnt * strlen(args[0]);
      char *o = malloc(cap), *w = o;
      while (*s) {
        if (rl && !strncmp(s, repl, rl)) { strcpy(w, args[0]); w += strlen(args[0]); s += rl; }
        else *w++ = *s++;
      }
      *w = 0;
      argv[k++] = o;
    }
  } else {
    for (int i = 0; i < nbase; i++) argv[k++] = base[i];
    for (int i = 0; i < n; i++) argv[k++] = args[i];
  }
  argv[k] = NULL;
  if (trace) {
    for (int i = 0; i < k; i++) fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
    fputc('\n', stderr);
  }
  fflush(stdout);
  pid_t pid = run_cmd(argv);
  if (pid < 0) {
    int e = errno;
    fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(e));
    exit(e == ENOENT ? 127 : 126);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) { perror("xargs: waitpid"); exit(1); }
  if (repl) for (int i = 0; i < nbase; i++) free(argv[i]);
  free(argv);
  if (WIFSIGNALED(st)) { fprintf(stderr, "xargs: %s: terminated by signal %d\n", base[0], WTERMSIG(st)); exit(125); }
  int code = WEXITSTATUS(st);
  if (code == 255) { fprintf(stderr, "xargs: %s: exited with status 255; aborting\n", base[0]); exit(124); }
  if (code) status = 123;
  return 0;
}

/* read one argument; returns NULL at end of input. *eol is set when the
 * argument ended a line (for -L). */
static int eof_seen, bad_quote;
static char *read_arg(int *eol) {
  static char *buf;
  static size_t cap;
  size_t n = 0;
  int c, quote = 0, any = 0;
  *eol = 0;
  if (eof_seen) return NULL;
  if (zero || delim >= 0) {
    int d = zero ? 0 : delim;
    while ((c = getchar()) != EOF && c != d) {
      if (n + 2 > cap) buf = realloc(buf, cap = cap * 2 + 64);
      buf[n++] = c;
      any = 1;
    }
    if (c == EOF && !any) return NULL;
    if (n + 1 > cap) buf = realloc(buf, cap = cap * 2 + 64);
    buf[n] = 0;
    *eol = 1;
    return buf;
  }
  for (;;) {
    c = getchar();
    if (c == EOF) {
      if (quote) { fprintf(stderr, "xargs: unmatched %s quote; by default quotes are special to xargs unless you use the -0 option\n", quote == '"' ? "double" : "single"); bad_quote = eof_seen = 1; return NULL; }
      if (!any) return NULL;
      *eol = 1;
      break;
    }
    if (quote) {
      if (c == quote) { quote = 0; continue; }
      if (c == '\n') { fprintf(stderr, "xargs: unmatched %s quote; by default quotes are special to xargs unless you use the -0 option\n", quote == '"' ? "double" : "single"); bad_quote = eof_seen = 1; return NULL; }
    } else if (repl ? c == '\n' : (c == ' ' || c == '\t' || c == '\n')) {
      /* an argument ends a line only when a newline follows it directly:
         a trailing blank continues the line for -L, as POSIX says */
      if (c == '\n' && any) { *eol = 1; break; }
      if (any) break;
      continue;
    } else if (repl && !any && (c == ' ' || c == '\t')) {
      continue; /* -I: leading blanks are ignored */
    } else if (c == '\'' || c == '"') { quote = c; any = 1; continue; }
    else if (c == '\\') { c = getchar(); if (c == EOF) break; }
    if (n + 2 > cap) buf = realloc(buf, cap = cap * 2 + 64);
    buf[n++] = c;
    any = 1;
  }
  if (n + 1 > cap) buf = realloc(buf, cap = cap * 2 + 64);
  buf[n] = 0;
  if (eofstr && !strcmp(buf, eofstr)) { eof_seen = 1; return NULL; }
  return buf;
}

static long parse_num(const char *s, const char *opt, long min) {
  char *e;
  long v = strtol(s, &e, 10);
  if (*e || v < min) { fprintf(stderr, "xargs: invalid number \"%s\" for -%s option\n", s, opt); exit(1); }
  return v;
}

int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--null")) { zero = 1; continue; }
    if (!strcmp(a, "--no-run-if-empty")) { no_empty = 1; continue; }
    if (!strcmp(a, "--verbose")) { trace = 1; continue; }
    if (!strcmp(a, "--exit")) { exit_on_size = 1; continue; }
    if (!strncmp(a, "--delimiter=", 12)) { a = a + 12; delim = *a == '\\' && a[1] == 'n' ? '\n' : *a == '\\' && a[1] == 't' ? '\t' : *a == '\\' && a[1] == '0' ? 0 : *a; continue; }
    if (!strncmp(a, "--max-args=", 11)) { max_args = parse_num(a + 11, "n", 1); continue; }
    if (!strncmp(a, "--max-lines=", 12)) { max_lines = parse_num(a + 12, "L", 1); continue; }
    if (!strncmp(a, "--max-procs=", 12)) { if (parse_num(a + 12, "P", 0) > 1) fprintf(stderr, "xargs: warning: running one command at a time\n"); continue; }
    if (!strncmp(a, "--replace=", 10)) { repl = a + 10; max_lines = 1; exit_on_size = 1; continue; }
    if (!strncmp(a, "--eof=", 6)) { eofstr = a + 6; continue; }
    if (a[1] == '-') { fprintf(stderr, "xargs: unrecognized option '%s'\n", a); return 1; }
    for (char *f = a + 1; *f; f++) {
      if (*f == '0') zero = 1;
      else if (*f == 'r') no_empty = 1;
      else if (*f == 't') trace = 1;
      else if (*f == 'x') exit_on_size = 1;
      else if (*f == 'i') { repl = f[1] ? f + 1 : "{}"; max_lines = 1; exit_on_size = 1; break; }
      else if (*f == 'e') { eofstr = f[1] ? f + 1 : NULL; break; }
      else if (*f == 'l') { max_lines = f[1] ? parse_num(f + 1, "l", 1) : 1; break; }
      else if (strchr("dEILnsP", *f)) {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "xargs: option requires an argument -- '%c'\n", *f); return 1; }
        switch (*f) {
        case 'd':
          if (v[0] == '\\' && v[1]) {
            int c = v[1];
            delim = c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? 0 : c == '\\' ? '\\' : c == 'x' ? (int)strtol(v + 2, NULL, 16) : c;
          } else if (strlen(v) > 1) { fprintf(stderr, "xargs: invalid input delimiter specification %s\n", v); return 1; }
          else delim = (unsigned char)v[0];
          break;
        case 'E': eofstr = v; break;
        case 'I': repl = v; max_lines = 1; exit_on_size = 1; break;
        case 'L': max_lines = parse_num(v, "L", 1); break;
        case 'n': max_args = parse_num(v, "n", 1); break;
        case 's': max_size = parse_num(v, "s", 1); break;
        case 'P': if (parse_num(v, "P", 0) > 1) fprintf(stderr, "xargs: warning: running one command at a time\n"); break;
        }
        break;
      } else { fprintf(stderr, "xargs: invalid option -- '%c'\n", *f); return 1; }
    }
  }
  static char *dflt[] = {"echo"};
  base = i < argc ? argv + i : dflt;
  nbase = i < argc ? argc - i : 1;
  if (repl) max_args = -1;

  /* room for arguments: the system limit minus the environment, with a margin */
  long limit = sysconf(_SC_ARG_MAX);
  if (limit <= 0) limit = 8192;
  extern char **environ;
  for (char **e = environ; *e; e++) limit -= strlen(*e) + 1 + sizeof(char *);
  limit -= 2048;
  if (limit > 131072) limit = 131072;
  if (limit < 1024) limit = 1024;
  if (max_size) {
    if (max_size > limit) fprintf(stderr, "xargs: value for -s option should be <= %ld\n", limit), max_size = limit;
    limit = max_size;
  }
  long base_size = 0;
  for (int k = 0; k < nbase; k++) base_size += strlen(base[k]) + 1;
  if (base_size >= limit) { fprintf(stderr, "xargs: argument list too long\n"); return 1; }

  char **args = NULL;
  int nargs = 0, cap = 0, ran = 0, eol;
  long size = base_size, lines = 0;
  char *a;
  while ((a = read_arg(&eol))) {
    long len = strlen(a) + 1;
    if (repl) {
      char *one[1] = {a};
      execute(one, 1);
      ran = 1;
      continue;
    }
    if (nargs && size + len > limit) {
      execute(args, nargs);
      ran = 1;
      for (int k = 0; k < nargs; k++) free(args[k]);
      nargs = 0; size = base_size; lines = 0;
    }
    if (size + len > limit) { fprintf(stderr, "xargs: argument line too long\n"); return 1; }
    if (nargs == cap) args = realloc(args, (cap = cap * 2 + 16) * sizeof *args);
    args[nargs++] = strdup(a);
    size += len;
    if (eol) lines++;
    if ((max_args > 0 && nargs >= max_args) || (max_lines > 0 && lines >= max_lines)) {
      execute(args, nargs);
      ran = 1;
      for (int k = 0; k < nargs; k++) free(args[k]);
      nargs = 0; size = base_size; lines = 0;
    }
  }
  /* GNU runs what was read before an unmatched quote, then fails */
  if (nargs || (!ran && !no_empty && !repl && !bad_quote)) execute(args, nargs);
  return bad_quote ? 1 : status;
}
