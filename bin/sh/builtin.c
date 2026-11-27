// SPDX-License-Identifier: GPL-2.0-or-later
/* Builtin commands. They run inside the shell process; output goes through
 * outs/outf so that $(...) capture and redirections see it. */
#include "sh.h"
#include <signal.h>

extern int exit_pending, exit_code, exec_keep, loop_depth;
/* special_error: a special builtin failed in a way that ends a non-interactive
 * shell (POSIX 2.8.1); exec.c acts on it. assign_error: a read-only variable. */
int special_error, assign_error, dot_depth;
static int sperr(int st) { special_error = 1; return st; }

static int b_true(int argc, char **argv) { return 0; }
static int b_false(int argc, char **argv) { return 1; }

/* ---- options: set -abCefhmnuvx and set -o NAME ---- */
int opt_e, opt_u, opt_f, opt_n, opt_v, opt_a, opt_C, opt_m, opt_pipefail, noerrexit;
static int opt_b, opt_h, opt_ignoreeof, opt_nolog, opt_vi;
static const struct { char letter; const char *name; int *flag; } shopts[] = {
  {'a', "allexport", &opt_a}, {'b', "notify", &opt_b}, {'C', "noclobber", &opt_C}, {'e', "errexit", &opt_e},
  {'f', "noglob", &opt_f}, {'h', "hashall", &opt_h}, {0, "ignoreeof", &opt_ignoreeof}, {'m', "monitor", &opt_m},
  {'n', "noexec", &opt_n}, {0, "nolog", &opt_nolog}, {'u', "nounset", &opt_u}, {0, "pipefail", &opt_pipefail},
  {'v', "verbose", &opt_v}, {0, "vi", &opt_vi}, {'x', "xtrace", &opt_x},
};
#define NSHOPTS (int)(sizeof shopts / sizeof shopts[0])
static int set_letter(char c, int on) {
  for (int i = 0; i < NSHOPTS; i++) if (shopts[i].letter == c) { *shopts[i].flag = on; return 0; }
  return -1;
}
static int set_name(const char *name, int on) {
  for (int i = 0; i < NSHOPTS; i++) if (!strcmp(shopts[i].name, name)) { *shopts[i].flag = on; return 0; }
  return -1;
}
/* set -o (a table) and set +o (commands that restore the options) */
static void print_opts(int reusable) {
  for (int i = 0; i < NSHOPTS; i++) {
    if (reusable) outf(1, "set %co %s\n", *shopts[i].flag ? '-' : '+', shopts[i].name);
    else outf(1, "%-15s %s\n", shopts[i].name, *shopts[i].flag ? "on" : "off");
  }
}
const char *dash_flags(void) {
  static char buf[24];
  int n = 0;
  for (int i = 0; i < NSHOPTS; i++) if (shopts[i].letter && *shopts[i].flag) buf[n++] = shopts[i].letter;
  if (interactive) buf[n++] = 'i';
  buf[n] = 0;
  return buf;
}
void opts_save(struct optsave *o) { for (int i = 0; i < NSHOPTS && i < 16; i++) o->v[i] = *shopts[i].flag; }
void opts_restore(struct optsave *o) { for (int i = 0; i < NSHOPTS && i < 16; i++) *shopts[i].flag = o->v[i]; }

/* option arguments shared by sh and set: one argument starting with - or +.
 * Returns 1 if it was an option argument, 0 if not, -1 on error; *i moves past
 * a -o NAME pair. cmdline: also accept c, i, s (sh only). */
static int opt_arg(int argc, char **argv, int *i, int cmdline, int *flags) {
  char *a = argv[*i];
  int on = a[0] == '-';
  if ((a[0] != '-' && a[0] != '+') || !a[1]) return 0;
  if (!strcmp(a, "--")) return 0;
  for (char *f = a + 1; *f; f++) {
    if (*f == 'o') {
      if (*i + 1 >= argc) { if (!cmdline) print_opts(!on); else { outs(2, "sh: -o: option requires an argument\n"); return -1; } continue; }
      char *name = argv[++*i];
      if (set_name(name, on) < 0) { outf(2, "%s: %s: invalid option name\n", cmdline ? "sh" : "set", name); return -1; }
      continue;
    }
    if (cmdline && strchr("cisl", *f)) { if (on) *flags |= *f == 'c' ? 1 : *f == 'i' ? 2 : *f == 's' ? 4 : 0; continue; }
    if (set_letter(*f, on) < 0) { outf(2, "%s: %c%c: invalid option\n", cmdline ? "sh" : "set", a[0], *f); return -1; }
  }
  return 1;
}
/* sh [-abCefhimnuvx] [-o name] [+...] [-c command [name [arg...]] | -s [arg...] | file [arg...]] */
int parse_args(int argc, char **argv, const char **cmd, int *force_i, int *read_stdin) {
  int i = 1, flags = 0;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "--") || !strcmp(argv[i], "-")) { i++; break; }
    int r = opt_arg(argc, argv, &i, 1, &flags);
    if (r < 0) { outs(2, "usage: sh [-abCefimnuvx] [-o option] [-c command [name [arg...]] | -s [arg...] | file [arg...]]\n"); return -1; }
    if (!r) break;
  }
  *force_i = (flags & 2) != 0;
  *read_stdin = (flags & 4) != 0;
  if (opt_m) { jobs_init(0); if (!jc_ok) { outs(2, "sh: -m: job control needs kernel support this system does not have yet\n"); opt_m = 0; } }
  if (flags & 1) {
    if (i >= argc) { outs(2, "sh: -c: option requires an argument\n"); return -1; }
    *cmd = argv[i++];
  }
  return i;
}
static int b_set(int argc, char **argv) {
  if (argc == 1) { list_vars(0); return 0; }
  int i = 1, flags = 0, setargs = 0;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "--")) { i++; setargs = 1; break; }
    if (!strcmp(argv[i], "-")) { opt_x = opt_v = 0; i++; setargs = i < argc; break; }
    int r = opt_arg(argc, argv, &i, 0, &flags);
    if (r < 0) return sperr(2);
    if (!r) { setargs = 1; break; }
  }
  if (opt_m) { jobs_init(0); if (!jc_ok) { outs(2, "set: -m: job control needs kernel support this system does not have yet\n"); opt_m = 0; } }
  if (setargs) {
    int n = argc - i;
    char **v = calloc(n + 1, sizeof *v);
    for (int k = 0; k < n; k++) v[k] = strdup(argv[i + k]);
    frame->argc = n;
    frame->argv = v; /* the old vector is left alone: a caller may still hold it */
  }
  return 0;
}

/* ---- trap ----
 * traps[0] is EXIT. NULL: default action, "": ignored. A signal trap installs
 * a C handler that only marks the signal; the action runs at the next safe
 * point (check_intr, between commands). On a kernel that delivers no signals
 * the handler is recorded and never called, so only EXIT (and kill of this
 * shell's own pid, done in the kill builtin) fires. */
#define NTRAP 65
static char *traps[NTRAP];
static int traps_owned = 1; /* 0 in a subshell that has not changed a trap yet */
static volatile int trap_pending[NTRAP];
volatile int traps_waiting;
static int in_trap;
static const char *signames[] = {"EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
  "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
  "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"};
#define NSIGNAMES (int)(sizeof signames / sizeof signames[0])
static int signum(const char *s) {
  if (isdigit((unsigned char)*s)) {
    char *e;
    long n = strtol(s, &e, 10);
    return *e || n >= NTRAP ? -1 : (int)n;
  }
  if (!strncasecmp(s, "SIG", 3)) s += 3;
  for (int i = 0; i < NSIGNAMES; i++) if (!strcasecmp(s, signames[i])) return i;
  return -1;
}
static void signame(int n, char *buf, size_t len) {
  if (n < NSIGNAMES) snprintf(buf, len, "%s", signames[n]);
  else snprintf(buf, len, "%d", n);
}
static void on_signal(int sig) {
  if (sig > 0 && sig < NTRAP) { trap_pending[sig] = 1; traps_waiting = 1; }
}
static void apply_disposition(int sig) {
  if (sig <= 0 || sig == SIGKILL || sig == SIGSTOP) return;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  /* without a trap, SIGPIPE stays ignored in the shell itself (see child_sigs) */
  sa.sa_handler = !traps[sig] ? (sig == SIGPIPE ? SIG_IGN : SIG_DFL) : !*traps[sig] ? SIG_IGN : on_signal;
  sa.sa_flags = SA_RESTART;
  sigaction(sig, &sa, NULL);
}
/* The shell ignores SIGPIPE for its own sake: builtins write in its process, and a closed
 * pipe must not kill it (a builtin then sees EPIPE). POSIX wants what the shell ignores for
 * itself not to reach the programs it starts, so around every spawn and exec SIGPIPE goes
 * back to the default, unless the shell was started with it ignored (then it stays ignored
 * everywhere, as POSIX says) or the user set a trap: "trap '' PIPE" is inherited as ignored,
 * a trap with a command becomes the default in the child anyway (the kernel resets handlers
 * at spawn and exec). enter = 1 before starting a program, 0 after. */
int sigpipe_ign_at_start;
void child_sigs(int enter) {
  if (sigpipe_ign_at_start || traps[SIGPIPE]) return;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = enter ? SIG_DFL : SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);
}
/* a subshell starts with the parent's traps visible (so that "$(trap)" prints
 * them) but not active; the first change inside it clears them */
void traps_enter(struct trapsave *s) {
  memcpy(s->t, traps, sizeof traps);
  s->owned = traps_owned;
  traps_owned = 0;
}
static void traps_own(void) {
  if (traps_owned) return;
  for (int i = 0; i < NTRAP; i++) if (traps[i] && *traps[i]) traps[i] = NULL;
  traps_owned = 1;
}
void traps_leave(struct trapsave *s) {
  if (traps_owned && traps[0] && !in_trap) { /* the subshell's own EXIT trap */
    char *a = traps[0];
    traps[0] = NULL;
    int lb = brk, lc = cont, lr = retflag, ep = exit_pending, ec = exit_code;
    brk = cont = retflag = exit_pending = 0;
    in_trap++;
    run_text(a, NULL);
    in_trap--;
    if (!exit_pending) { exit_pending = ep; exit_code = ec; }
    brk = lb; cont = lc; retflag = lr;
  }
  int changed = traps_owned;
  memcpy(traps, s->t, sizeof traps);
  traps_owned = s->owned;
  if (changed) for (int i = 1; i < NTRAP; i++) apply_disposition(i);
}
void trap_forget_exit(void) { traps[0] = NULL; }
/* called by sh_exit: runs the EXIT trap once and returns the final status */
int run_exit_trap(int code) {
  if (!traps[0] || in_trap || !traps_owned) return code;
  char *a = traps[0];
  traps[0] = NULL;
  last_status = code;
  exit_pending = brk = cont = retflag = 0;
  in_trap++;
  run_text(a, NULL);
  in_trap--;
  return exit_pending ? exit_code : code;
}
void run_pending_traps(void) {
  if (in_trap) return;
  traps_waiting = 0;
  for (int i = 1; i < NTRAP; i++) {
    if (!trap_pending[i]) continue;
    trap_pending[i] = 0;
    if (!traps[i] || !*traps[i]) continue;
    int st = last_status;
    in_trap++;
    run_text(traps[i], NULL);
    in_trap--;
    if (!exit_pending) last_status = st;
  }
}
static void put_squoted(const char *v) {
  outs(1, "'");
  for (; *v; v++) { if (*v == '\'') outs(1, "'\\''"); else sh_write(1, v, 1); }
  outs(1, "'");
}
static int b_trap(int argc, char **argv) {
  int i = 1;
  if (i < argc && !strcmp(argv[i], "--")) i++;
  if (i >= argc || (!strcmp(argv[i], "-p") && i + 1 >= argc)) {
    char nm[16];
    for (int k = 0; k < NTRAP; k++) {
      if (!traps[k]) continue;
      signame(k, nm, sizeof nm);
      outs(1, "trap -- ");
      put_squoted(traps[k]);
      outf(1, " %s\n", nm);
    }
    return 0;
  }
  const char *action = argv[i];
  /* "trap N..." with an unsigned number first, or a single condition: reset */
  if (isdigit((unsigned char)action[0]) && signum(action) >= 0) action = "-";
  else if (i + 1 >= argc && signum(action) >= 0) action = "-";
  else i++;
  traps_own();
  int rc = 0;
  for (; i < argc; i++) {
    int sig = signum(argv[i]);
    if (sig < 0) { outf(2, "trap: %s: bad trap\n", argv[i]); rc = 1; continue; }
    if (sig == SIGPIPE && sigpipe_ign_at_start) continue; /* ignored on entry: cannot be trapped or reset (POSIX) */
    traps[sig] = !strcmp(action, "-") ? NULL : strdup(action);
    apply_disposition(sig);
  }
  return rc;
}
/* kill -l and the names kill accepts */
static int kill_list(int argc, char **argv, int i) {
  char nm[16];
  if (i >= argc) {
    for (int k = 1; k < NSIGNAMES; k++) { signame(k, nm, sizeof nm); outf(1, "%s%s", nm, k % 8 == 0 || k == NSIGNAMES - 1 ? "\n" : " "); }
    return 0;
  }
  int rc = 0;
  for (; i < argc; i++) {
    int n = isdigit((unsigned char)argv[i][0]) ? atoi(argv[i]) : signum(argv[i]);
    if (n > 128) n -= 128;
    if (n <= 0 || n >= NTRAP) { outf(2, "kill: %s: invalid signal\n", argv[i]); rc = 1; continue; }
    if (isdigit((unsigned char)argv[i][0])) { signame(n, nm, sizeof nm); outf(1, "%s\n", nm); }
    else outf(1, "%d\n", n);
  }
  return rc;
}
static int b_kill(int argc, char **argv) {
  int sig = SIGTERM, i = 1;
  if (i < argc && !strcmp(argv[i], "-l")) return kill_list(argc, argv, i + 1);
  if (i < argc && !strcmp(argv[i], "-s")) {
    if (i + 1 >= argc || (sig = signum(argv[i + 1])) < 0) { outs(2, "kill: -s: invalid signal\n"); return 2; }
    i += 2;
  } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
    if ((sig = signum(argv[i] + 1)) < 0) { outf(2, "kill: %s: invalid signal\n", argv[i]); return 2; }
    i++;
  }
  if (i < argc && !strcmp(argv[i], "--")) i++;
  if (i >= argc) { outs(2, "usage: kill [-s signal | -signal] pid...\n       kill -l [status]\n"); return 2; }
  int rc = 0;
  for (; i < argc; i++) {
    pid_t pids[16];
    int np = 0;
    if (argv[i][0] == '%') {
      int r = job_kill(argv[i], sig);
      if (r == -2) outf(2, "kill: %s: no such job\n", argv[i]);
      else if (r < 0) outf(2, "kill: %s: %s\n", argv[i], strerror(errno));
      if (r < 0) rc = 1;
      continue;
    } else {
      char *e;
      long v = strtol(argv[i], &e, 10);
      if (*e || e == argv[i]) { outf(2, "kill: %s: arguments must be process or job IDs\n", argv[i]); rc = 1; continue; }
      pids[np++] = v;
    }
    for (int k = 0; k < np; k++) {
      /* a trapped signal sent to this shell itself: run the trap here, so that it
       * also works on a kernel that does not deliver signals */
      if (pids[k] == getpid() && sig > 0 && traps[sig] && traps_owned) {
        if (*traps[sig]) { trap_pending[sig] = 1; traps_waiting = 1; }
        continue;
      }
      if (kill(pids[k], sig) < 0) { outf(2, "kill: (%d) - %s\n", (int)pids[k], strerror(errno)); rc = 1; }
    }
  }
  return rc;
}

/* variables every shell starts with */
void shell_start(void) {
  char tmp[16];
  snprintf(tmp, sizeof tmp, "%d", (int)getppid());
  setvar("PPID", tmp, 0);
  setvar("OPTIND", "1", 0);
  setvar("IFS", " \t\n", 0);
}

/* backslash escapes for echo -e and printf; returns 1 if \c (stop output) was seen */
static int unescape_to(const char *s, char *o, int *olen) {
  int n = 0;
  for (; *s; s++) {
    if (*s != '\\' || !s[1]) { o[n++] = *s; continue; }
    s++;
    switch (*s) {
    case 'n': o[n++] = '\n'; break;
    case 't': o[n++] = '\t'; break;
    case 'r': o[n++] = '\r'; break;
    case 'a': o[n++] = '\a'; break;
    case 'b': o[n++] = '\b'; break;
    case 'f': o[n++] = '\f'; break;
    case 'v': o[n++] = '\v'; break;
    case 'e': case 'E': o[n++] = 27; break;
    case '\\': o[n++] = '\\'; break;
    case 'c': *olen = n; return 1;
    case 'x': {
      int v = 0, k = 0;
      while (k < 2 && isxdigit((unsigned char)s[1])) { s++; v = v * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (tolower(*s) - 'a' + 10)); k++; }
      if (k) o[n++] = v; else { o[n++] = '\\'; o[n++] = 'x'; }
      break;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
      int v = 0, k = 0, max = *s == '0' ? 4 : 3;
      s--;
      while (k < max && s[1] >= '0' && s[1] <= '7') { s++; v = v * 8 + (*s - '0'); k++; }
      o[n++] = v;
      break;
    }
    default: o[n++] = '\\'; o[n++] = *s;
    }
  }
  *olen = n;
  return 0;
}

static int b_echo(int argc, char **argv) {
  int nl = 1, esc = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *f = argv[i] + 1;
    if (strspn(f, "neE") != strlen(f)) break;
    for (; *f; f++) { if (*f == 'n') nl = 0; else if (*f == 'e') esc = 1; else esc = 0; }
  }
  size_t total = 2;
  for (int j = i; j < argc; j++) total += strlen(argv[j]) + 1;
  char *buf = malloc(total), *o = buf;
  int stop = 0;
  for (; i < argc && !stop; i++) {
    if (esc) { int n; stop = unescape_to(argv[i], o, &n); o += n; }
    else { strcpy(o, argv[i]); o += strlen(argv[i]); }
    if (i + 1 < argc && !stop) *o++ = ' ';
  }
  if (nl && !stop) *o++ = '\n';
  sh_write(1, buf, o - buf);
  free(buf);
  return 0;
}

/* a numeric printf argument: 'c gives the character code; a bad number is
 * reported, its leading digits are used and the status becomes 1 */
static long long num_arg(const char *arg, int *rc) {
  if (!arg) return 0;
  if ((arg[0] == '\'' || arg[0] == '"') && arg[1]) return (unsigned char)arg[1];
  char *e;
  errno = 0;
  long long v = strtoll(arg, &e, 0);
  if (*e || e == arg) { outf(2, "printf: %s: invalid number\n", arg); *rc = 1; }
  return v;
}
static int b_printf(int argc, char **argv) {
  int ai = 1;
  if (ai < argc && !strcmp(argv[ai], "--")) ai++;
  if (ai >= argc) { outs(2, "printf: usage: printf format [arguments]\n"); return 2; }
  const char *fmt = argv[ai++];
  int rc = 0, stop = 0;
  size_t cap = 256, len = 0;
  char *out = malloc(cap);
#define PUT(s, n) do { size_t _n = (n); while (len + _n + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + len, (s), _n); len += _n; } while (0)
#define NEXT() (ai < argc ? argv[ai++] : NULL)
  do {
    int consumed = 0;
    for (const char *f = fmt; *f && !stop; f++) {
      if (*f == '\\') {
        char tmp[8], e[3] = {'\\', f[1], 0};
        int n;
        if (!f[1]) { PUT("\\", 1); continue; }
        if (f[1] >= '0' && f[1] <= '7') { /* octal: up to 3 digits */
          int v = 0, k = 0;
          while (k < 3 && f[1] >= '0' && f[1] <= '7') { f++; v = v * 8 + (*f - '0'); k++; }
          char c = v;
          PUT(&c, 1);
          continue;
        }
        if (f[1] == 'c') { stop = 1; break; }
        unescape_to(e, tmp, &n);
        PUT(tmp, n);
        f++;
        continue;
      }
      if (*f != '%') { PUT(f, 1); continue; }
      if (f[1] == '%') { PUT("%", 1); f++; continue; }
      char spec[48];
      int sn = 0;
      spec[sn++] = '%';
      f++;
      while (*f && strchr("-+ #0", *f) && sn < 12) spec[sn++] = *f++;
      if (*f == '*') { consumed = 1; sn += snprintf(spec + sn, 16, "%d", (int)num_arg(NEXT(), &rc)); f++; }
      else while (isdigit((unsigned char)*f) && sn < 24) spec[sn++] = *f++;
      if (*f == '.') {
        spec[sn++] = *f++;
        if (*f == '*') { consumed = 1; sn += snprintf(spec + sn, 16, "%d", (int)num_arg(NEXT(), &rc)); f++; }
        else while (isdigit((unsigned char)*f) && sn < 36) spec[sn++] = *f++;
      }
      if (!*f) break;
      char conv = *f;
      const char *arg = NEXT();
      consumed = 1;
      char buf[512];
      int n = 0;
      if (conv == 's' || conv == 'b') {
        char *tmp = NULL;
        const char *str = arg ? arg : "";
        if (conv == 'b') { int bl; tmp = malloc(strlen(str) + 1); stop = unescape_to(str, tmp, &bl); tmp[bl] = 0; str = tmp; }
        spec[sn++] = 's'; spec[sn] = 0;
        int need = snprintf(NULL, 0, spec, str);
        char *big = malloc(need + 1);
        snprintf(big, need + 1, spec, str);
        PUT(big, need);
        free(big); free(tmp);
        continue;
      }
      if (conv == 'c') {
        char one[2] = {arg ? arg[0] : 0, 0};
        spec[sn++] = 's'; spec[sn] = 0; /* an empty argument prints no character, only the padding */
        n = snprintf(buf, sizeof buf, spec, one);
        PUT(buf, n);
        continue;
      }
      if (strchr("diouxX", conv)) {
        long long v = num_arg(arg, &rc);
        spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = conv == 'i' ? 'd' : conv; spec[sn] = 0;
        n = snprintf(buf, sizeof buf, spec, v);
      } else if (strchr("fFeEgGaA", conv)) {
        double d = 0;
        if (arg) {
          char *e;
          if ((arg[0] == '\'' || arg[0] == '"') && arg[1]) d = (unsigned char)arg[1];
          else { d = strtod(arg, &e); if (*e || e == arg) { outf(2, "printf: %s: invalid number\n", arg); rc = 1; } }
        }
        spec[sn++] = strchr("aA", conv) ? (conv == 'a' ? 'e' : 'E') : conv; spec[sn] = 0;
        n = snprintf(buf, sizeof buf, spec, d);
      } else { outf(2, "printf: %%%c: invalid directive\n", conv); rc = 1; stop = 1; break; }
      if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
      PUT(buf, n);
    }
    if (!consumed || stop) break;
  } while (ai < argc);
  sh_write(1, out, len);
  free(out);
  return rc;
}
static int try_cd(const char *dir, const char *old, int print) {
  sub_saw_cd();
  if (chdir(dir) < 0) return -1;
  char cwd[512];
  if (getcwd(cwd, sizeof cwd)) setvar("PWD", cwd, 0);
  setvar("OLDPWD", old, 0);
  if (print) outf(1, "%s\n", cwd);
  return 0;
}
static int b_cd(int argc, char **argv) {
  int i = 1, print = 0;
  /* -L and -P: this system has no symbolic links, so both mean the same */
  for (; i < argc && (!strcmp(argv[i], "-L") || !strcmp(argv[i], "-P")); i++) {}
  if (i < argc && !strcmp(argv[i], "--")) i++;
  const char *dir = i < argc ? argv[i] : getvar("HOME");
  if (!dir || !*dir) { outs(2, "cd: HOME not set\n"); return 1; }
  if (!strcmp(dir, "-")) { dir = getvar("OLDPWD"); print = 1; if (!dir) { outs(2, "cd: OLDPWD not set\n"); return 1; } }
  char old[512];
  if (!getcwd(old, sizeof old)) old[0] = 0;
  const char *cdpath = getvar("CDPATH");
  int relative = dir[0] != '/' && strcmp(dir, ".") && strcmp(dir, "..") && strncmp(dir, "./", 2) && strncmp(dir, "../", 3);
  if (cdpath && *cdpath && relative) {
    for (const char *c = cdpath;;) {
      const char *e = strchr(c, ':');
      size_t n = e ? (size_t)(e - c) : strlen(c);
      char buf[512];
      if (n + strlen(dir) + 2 < sizeof buf) {
        if (n) { memcpy(buf, c, n); buf[n] = '/'; strcpy(buf + n + 1, dir); } else strcpy(buf, dir);
        if (try_cd(buf, old, n != 0 || print) == 0) return 0;
      }
      if (!e) break;
      c = e + 1;
    }
  }
  if (try_cd(dir, old, print) < 0) { outf(2, "cd: %s: %s\n", dir, strerror(errno)); return 1; }
  return 0;
}
static int b_pwd(int argc, char **argv) {
  char cwd[512];
  for (int i = 1; i < argc; i++) if (strcmp(argv[i], "-L") && strcmp(argv[i], "-P")) { outf(2, "pwd: %s: invalid option\n", argv[i]); return 2; }
  if (!getcwd(cwd, sizeof cwd)) { outf(2, "pwd: %s\n", strerror(errno)); return 1; }
  outf(1, "%s\n", cwd);
  return 0;
}
static int b_exit(int argc, char **argv) {
  exit_code = argc > 1 ? atoi(argv[1]) & 255 : last_status;
  exit_pending = 1;
  return exit_code;
}
static int b_return(int argc, char **argv) {
  if (!in_func && !dot_depth) { outs(2, "return: not in a function or a sourced file\n"); return sperr(2); }
  int v = argc > 1 ? atoi(argv[1]) & 255 : last_status;
  retflag = 1;
  last_status = v;
  return v;
}
static int b_break(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1;
  if (!loop_depth) { outf(2, "%s: only meaningful in a loop\n", argv[0]); return 0; }
  if (n < 1) n = 1;
  if (n > loop_depth) n = loop_depth;
  if (argv[0][0] == 'b') brk = n; else cont = n;
  return 0;
}
static int b_shift(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1;
  if (n < 0 || n > frame->argc) { outs(2, "shift: shift count out of range\n"); return sperr(1); }
  frame->argc -= n;
  frame->argv += n;
  return 0;
}
/* export and readonly: name[=value]... or -p */
static int b_export(int argc, char **argv) {
  int ro = argv[0][0] == 'r', i = 1;
  if (i < argc && !strcmp(argv[i], "-p")) i++;
  if (i < argc && !strcmp(argv[i], "--")) i++;
  if (i >= argc) { if (ro) list_readonly(); else list_vars(1); return 0; }
  int rc = 0;
  for (; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
    if (!valid_name(argv[i], n)) { outf(2, "%s: `%s': not a valid identifier\n", argv[0], argv[i]); rc = sperr(1); continue; }
    char *name = strndup(argv[i], n);
    if (eq) setvar(name, eq + 1, !ro);
    if (assign_error) { assign_error = 0; rc = sperr(1); }
    else if (ro) set_readonly(name);
    else export_var(name);
    free(name);
  }
  return rc;
}
static int b_unset(int argc, char **argv) {
  int funcs = 0, vars = 0, i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "-f")) funcs = 1; else if (!strcmp(argv[i], "-v")) vars = 1;
    else { outf(2, "unset: %s: invalid option\n", argv[i]); return sperr(2); }
  }
  for (; i < argc; i++) {
    if (funcs && !vars) { unsetfunc(argv[i]); continue; }
    if (!vars && !getvar(argv[i]) && !is_readonly(argv[i]) && getfunc(argv[i])) { unsetfunc(argv[i]); continue; }
    unsetvar(argv[i]);
    if (assign_error) { assign_error = 0; rc = sperr(1); }
  }
  return rc;
}
static int b_local(int argc, char **argv) {
  if (!in_func) { outs(2, "local: can only be used in a function\n"); return 1; }
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    char *name = eq ? strndup(argv[i], eq - argv[i]) : strdup(argv[i]);
    if (!valid_name(name, strlen(name))) { outf(2, "local: `%s': not a valid identifier\n", argv[i]); free(name); return 1; }
    local_var(name);
    if (eq) setvar(name, eq + 1, 0);
    free(name);
  }
  return 0;
}
static int b_alias(int argc, char **argv) {
  if (argc == 1) { list_aliases(); return 0; }
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) { char *name = strndup(argv[i], eq - argv[i]); setalias(name, eq + 1); free(name); }
    else {
      char *v = getalias(argv[i]);
      if (v) { outf(1, "%s=", argv[i]); put_squoted(v); outs(1, "\n"); }
      else { outf(2, "alias: %s: not found\n", argv[i]); rc = 1; }
    }
  }
  return rc;
}
static int b_unalias(int argc, char **argv) {
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) { unalias_all(); continue; }
    if (unalias(argv[i]) < 0) { outf(2, "unalias: %s: not found\n", argv[i]); rc = 1; }
  }
  return rc;
}
static int b_source(int argc, char **argv) {
  if (argc < 2) { outf(2, "%s: filename argument required\n", argv[0]); return sperr(2); }
  /* POSIX: a name without a slash is searched for in PATH only (it need not be
   * executable). Not found or not readable: a special builtin error, which
   * ends a non-interactive shell; an interactive one prints a message. */
  char *path = strchr(argv[1], '/') ? strdup(argv[1]) : find_in_path(argv[1]);
  if (!path) { outf(2, "%s: %s: not found\n", argv[0], argv[1]); return sperr(1); }
  size_t len;
  char *text = read_file(path, &len);
  if (!text) { outf(2, "%s: %s: %s\n", argv[0], argv[1], strerror(errno)); free(path); return sperr(1); }
  struct frame f, *saved = frame;
  if (argc > 2) { f.argc = argc - 2; f.argv = argv + 2; f.locals = NULL; f.up = frame->up; frame = &f; }
  dot_depth++;
  int st = run_text(text, argv[1]);
  dot_depth--;
  if (argc > 2) { saved->locals = f.locals ? f.locals : saved->locals; frame = saved; }
  if (retflag && !in_func) { retflag = 0; st = last_status; }
  free(text);
  free(path);
  return st;
}
static int b_eval(int argc, char **argv) {
  size_t n = 1;
  for (int i = 1; i < argc; i++) n += strlen(argv[i]) + 1;
  char *s = malloc(n);
  s[0] = 0;
  for (int i = 1; i < argc; i++) { strcat(s, argv[i]); if (i + 1 < argc) strcat(s, " "); }
  if (!s[strspn(s, " \t\n")]) { free(s); return 0; }
  int st = run_text(s, "eval");
  free(s);
  return st;
}

static int b_read(int argc, char **argv) {
  int raw = 0, i = 1;
  const char *prompt = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-r")) raw = 1;
    else if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else { outf(2, "read: %s: invalid option\n", argv[i]); return 2; }
  }
  if (prompt) outs(2, prompt);
  size_t cap = 128, len = 0;
  char *line = malloc(cap);
  unsigned char *lit = malloc(cap); /* 1 = escaped by a backslash: never a separator */
  int got_eof = 1;
  char c;
  while (read(0, &c, 1) == 1) {
    if (c == '\n') { got_eof = 0; break; }
    int esc = 0;
    if (c == '\\' && !raw) {
      if (read(0, &c, 1) != 1) break;
      if (c == '\n') continue;
      esc = 1;
    }
    if (len + 2 > cap) { cap *= 2; line = realloc(line, cap); lit = realloc(lit, cap); }
    lit[len] = esc;
    line[len++] = c;
  }
  line[len] = 0;
  const char *ifs = getvar("IFS");
  if (!ifs) ifs = " \t\n";
  int nvars = argc - i;
  char *def[] = {"REPLY"};
  char **vars = nvars ? argv + i : def;
  if (!nvars) nvars = 1;
#define ISSEP(k) (!lit[k] && strchr(ifs, line[k]) && line[k])
#define ISWS(k) (ISSEP(k) && isspace((unsigned char)line[k]))
  size_t k = 0;
  if (nvars == 1 && !argv[i]) { setvar("REPLY", line, 0); } /* REPLY keeps the line as is */
  else {
    for (int v = 0; v < nvars; v++) {
      while (k < len && ISWS(k)) k++;
      size_t start = k, end;
      if (v == nvars - 1) {
        end = len;
        while (end > start && ISWS(end - 1)) end--;
      } else {
        while (k < len && !ISSEP(k)) k++;
        end = k;
        if (k < len) { k++; while (k < len && ISWS(k)) k++; }
      }
      char *val = strndup(line + start, end - start);
      setvar(vars[v], val, 0);
      free(val);
    }
  }
  free(line);
  free(lit);
  return got_eof;
}

static const char *keywords[] = {"!", "{", "}", "case", "do", "done", "elif", "else", "esac", "fi", "for", "if", "in", "then", "until", "while", 0};
static int is_keyword(const char *n) { for (int i = 0; keywords[i]; i++) if (!strcmp(keywords[i], n)) return 1; return 0; }
/* type NAME and command -V NAME (brief = 0); command -v NAME (brief = 1) */
static int describe(const char *n, int brief) {
  char *a, *p;
  if ((a = getalias(n))) {
    if (brief) { outf(1, "alias %s=", n); put_squoted(a); outs(1, "\n"); }
    else outf(1, "%s is an alias for %s\n", n, a);
  } else if (is_keyword(n)) outf(1, brief ? "%s\n" : "%s is a shell keyword\n", n);
  else if (is_special_builtin(n)) outf(1, brief ? "%s\n" : "%s is a special shell builtin\n", n);
  else if (getfunc(n)) outf(1, brief ? "%s\n" : "%s is a function\n", n);
  else if (find_builtin(n)) outf(1, brief ? "%s\n" : "%s is a shell builtin\n", n);
  else if ((p = find_in_path(n))) { if (brief) outf(1, "%s\n", p); else outf(1, "%s is %s\n", n, p); free(p); }
  else { if (!brief) outf(2, "%s: not found\n", n); return 127; }
  return 0;
}
static int b_type(int argc, char **argv) {
  int rc = 0;
  for (int i = 1; i < argc; i++) if (describe(argv[i], 0)) rc = 127;
  return rc;
}
/* command -v / -V (running a command through "command" is done in exec.c) */
static int b_command(int argc, char **argv) {
  int i = 1, mode = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'v') mode = 1; else if (*f == 'V') mode = 2;
      else if (*f != 'p') { outf(2, "command: -%c: invalid option\n", *f); return 2; }
    }
  }
  int rc = 0;
  for (; i < argc; i++) if (describe(argv[i], mode == 1)) rc = 127;
  return rc;
}
static int b_history(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "-c")) { for (int i = 0; i < nhist; i++) free(hist[i]); nhist = 0; return 0; }
  int start = 0;
  if (argc > 1) { int n = atoi(argv[1]); if (n > 0 && n < nhist) start = nhist - n; }
  for (int i = start; i < nhist; i++) outf(1, "%5d  %s\n", i + 1, hist[i]);
  return 0;
}
static int b_exec(int argc, char **argv) {
  int i = 1;
  if (i < argc && !strcmp(argv[i], "--")) i++;
  if (i >= argc) { exec_keep = 1; return 0; } /* exec >file: the redirections stay */
  return exec_cmd(argc - i, argv + i);
}
static int b_wait(int argc, char **argv) {
  if (argc == 1) { wait_jobs(); return 0; }
  int st = 0;
  for (int i = 1; i < argc; i++) {
    pid_t pids[16];
    int np = 0, found;
    if (argv[i][0] == '%') np = job_pids(argv[i], pids, 16);
    else { char *e; long v = strtol(argv[i], &e, 10); if (!*e && e != argv[i]) pids[np++] = v; }
    if (np <= 0) { outf(2, "wait: %s: no such job\n", argv[i]); st = 127; continue; }
    for (int k = 0; k < np; k++) {
      st = wait_for(pids[k], &found);
      if (!found) { outf(2, "wait: pid %d is not a child of this shell\n", (int)pids[k]); st = 127; }
    }
  }
  return st;
}
static int b_jobs(int argc, char **argv) { return job_list(argc, argv); }
static int b_fg(int argc, char **argv) { return argv[0][0] == 'f' ? job_fg(argc > 1 ? argv[1] : NULL) : job_bg(argc > 1 ? argv[1] : NULL); }
static int b_disown(int argc, char **argv) { return job_disown(argc, argv); }

/* umask. The shell keeps the mask itself and hands it to the kernel (Linux's
 * umask, 166) when the kernel has the call. Without it (ENOSYS) the shell still
 * sets, reports and passes on its own value: nothing stores file modes then,
 * so nothing observable is lost, and umask succeeds as POSIX wants. */
#ifndef SYS_umask
#define SYS_umask 166
#endif
static int cur_mask = -1; /* -1: not read yet */
static int get_mask(void) {
  if (cur_mask < 0) {
    long r = __syscall1(SYS_umask, 022); /* read the kernel's value by setting one */
    if (r >= 0) { __syscall1(SYS_umask, r); cur_mask = r & 0777; }
    else cur_mask = 022;                  /* the usual default */
  }
  return cur_mask;
}
/* set the mask; also used to put a subshell's change back */
void set_mask(int m) {
  cur_mask = m & 0777;
  __syscall1(SYS_umask, cur_mask);       /* ENOSYS on a kernel without the call: fine */
}
static int b_umask(int argc, char **argv) {
  int i = 1, sym = 0;
  if (i < argc && !strcmp(argv[i], "-S")) { sym = 1; i++; }
  if (i < argc && !strcmp(argv[i], "--")) i++;
  int old = get_mask();
  if (i >= argc) {
    if (!sym) { outf(1, "%04o\n", (unsigned)old); return 0; }
    int perm = ~old & 0777;
    const char *who = "ugo";
    for (int k = 0; k < 3; k++) {
      int b = perm >> (6 - 3 * k);
      outf(1, "%c=%s%s%s%s", who[k], b & 4 ? "r" : "", b & 2 ? "w" : "", b & 1 ? "x" : "", k < 2 ? "," : "\n");
    }
    return 0;
  }
  const char *m = argv[i];
  int mask;
  if (isdigit((unsigned char)*m)) {
    char *e;
    long v = strtol(m, &e, 8);
    if (*e || v > 0777) { outf(2, "umask: %s: invalid mode\n", m); return 1; }
    mask = v;
  } else { /* symbolic, as for chmod, applied to the permissions the mask allows */
    int perm = ~old & 0777;
    const char *c = m;
    for (;;) {
      int who = 0;
      for (; *c && strchr("ugoa", *c); c++) who |= *c == 'u' ? 0700 : *c == 'g' ? 070 : *c == 'o' ? 07 : 0777;
      if (!who) who = 0777;
      if (!*c || !strchr("+-=", *c)) { outf(2, "umask: %s: invalid mode\n", m); return 1; }
      while (*c && strchr("+-=", *c)) {
        char op = *c++;
        int bits = 0;
        for (; *c && strchr("rwxXst", *c); c++) bits |= *c == 'r' ? 0444 : *c == 'w' ? 0222 : *c == 'x' || *c == 'X' ? 0111 : 0;
        bits &= who;
        if (op == '=') perm = (perm & ~who) | bits;
        else if (op == '+') perm |= bits;
        else perm &= ~bits;
      }
      if (!*c) break;
      if (*c++ != ',') { outf(2, "umask: %s: invalid mode\n", m); return 1; }
    }
    mask = ~perm & 0777;
  }
  sub_saw_umask(old);
  set_mask(mask);
  return 0;
}

/* getopts optstring name [arg...] */
static int g_ind = 1, g_pos;
static int b_getopts(int argc, char **argv) {
  if (argc < 3) { outs(2, "getopts: usage: getopts optstring name [arg...]\n"); return 2; }
  const char *opts = argv[1], *name = argv[2];
  char **args = argc > 3 ? argv + 3 : frame->argv;
  int nargs = argc > 3 ? argc - 3 : frame->argc;
  const char *ov = getvar("OPTIND");
  int ind = ov ? atoi(ov) : 1;
  if (ind < 1) ind = 1;
  if (ind != g_ind) g_pos = 0; /* OPTIND was set by the script: start afresh */
  int silent = opts[0] == ':';
  char cbuf[2] = {0, 0}, nbuf[16];
  if (!g_pos) {
    const char *a = ind <= nargs ? args[ind - 1] : NULL;
    if (!a || a[0] != '-' || !a[1]) goto end;
    if (!strcmp(a, "--")) { ind++; goto end; }
    g_pos = 1;
  }
  const char *a = args[ind - 1];
  char c = a[g_pos++];
  if (!a[g_pos]) { ind++; g_pos = 0; }
  const char *p = c != ':' ? strchr(opts + silent, c) : NULL;
  cbuf[0] = c;
  if (!p) {
    if (silent) setvar("OPTARG", cbuf, 0);
    else { outf(2, "%s: illegal option -- %c\n", shell_name, c); unsetvar("OPTARG"); }
    setvar(name, "?", 0);
  } else if (p[1] == ':') {
    if (g_pos) { setvar("OPTARG", a + g_pos, 0); ind++; g_pos = 0; setvar(name, cbuf, 0); }
    else if (ind <= nargs) { setvar("OPTARG", args[ind - 1], 0); ind++; setvar(name, cbuf, 0); }
    else if (silent) { setvar("OPTARG", cbuf, 0); setvar(name, ":", 0); }
    else { outf(2, "%s: option requires an argument -- %c\n", shell_name, c); unsetvar("OPTARG"); setvar(name, "?", 0); }
  } else { unsetvar("OPTARG"); setvar(name, cbuf, 0); }
  snprintf(nbuf, sizeof nbuf, "%d", ind);
  setvar("OPTIND", nbuf, 0);
  g_ind = ind;
  return 0;
end:
  g_pos = 0;
  setvar(name, "?", 0);
  snprintf(nbuf, sizeof nbuf, "%d", ind);
  setvar("OPTIND", nbuf, 0);
  g_ind = ind;
  return 1;
}

/* times: this shell's own CPU time from the kernel; children are not counted */
static int b_times(int argc, char **argv) {
  struct procinfo pi;
  unsigned long long us = 0;
  for (int k = 0; __syscall2(SYS_procinfo, k, &pi) == 0; k++) if (pi.pid == getpid()) { us = pi.cpu_us; break; }
  outf(1, "%dm%d.%02ds 0m0.00s\n0m0.00s 0m0.00s\n", (int)(us / 60000000), (int)(us / 1000000 % 60), (int)(us / 10000 % 100));
  return 0;
}
static int b_help(int argc, char **argv);

/* special: the POSIX special builtins (found before functions; their errors end
 * a non-interactive shell; assignments in front of them stay) */
static const struct { const char *name; builtin_fn fn; char special; } builtins[] = {
  {":", b_true, 1}, {".", b_source, 1}, {"[", sh_test, 0}, {"alias", b_alias, 0}, {"bg", b_fg, 0}, {"break", b_break, 1}, {"cd", b_cd, 0},
  {"command", b_command, 0}, {"continue", b_break, 1}, {"disown", b_disown, 0}, {"echo", b_echo, 0}, {"eval", b_eval, 1}, {"exec", b_exec, 1},
  {"exit", b_exit, 1}, {"export", b_export, 1}, {"false", b_false, 0}, {"fg", b_fg, 0}, {"getopts", b_getopts, 0}, {"help", b_help, 0},
  {"history", b_history, 0}, {"jobs", b_jobs, 0}, {"kill", b_kill, 0}, {"local", b_local, 0}, {"printf", b_printf, 0},
  {"pwd", b_pwd, 0}, {"read", b_read, 0}, {"readonly", b_export, 1}, {"return", b_return, 1}, {"set", b_set, 1},
  {"shift", b_shift, 1}, {"source", b_source, 0}, {"test", sh_test, 0}, {"times", b_times, 1}, {"trap", b_trap, 1},
  {"true", b_true, 0}, {"type", b_type, 0}, {"umask", b_umask, 0}, {"unalias", b_unalias, 0}, {"unset", b_unset, 1},
  {"wait", b_wait, 0},
};
#define NBUILTINS (int)(sizeof builtins / sizeof builtins[0])
static int b_help(int argc, char **argv) {
  outs(1, "sh, the UC.OS shell. Builtin commands:\n");
  for (int i = 0; i < NBUILTINS; i++) outf(1, "  %-10s%s", builtins[i].name, i % 6 == 5 ? "\n" : "");
  outs(1, "\nSee man sh for the full description.\n");
  return 0;
}
builtin_fn find_builtin(const char *name) {
  for (int i = 0; i < NBUILTINS; i++) if (!strcmp(builtins[i].name, name)) return builtins[i].fn;
  return NULL;
}
const char *builtin_name(int i) { return i < NBUILTINS ? builtins[i].name : NULL; }
int is_special_builtin(const char *name) {
  for (int i = 0; i < NBUILTINS; i++) if (!strcmp(builtins[i].name, name)) return builtins[i].special;
  return 0;
}
