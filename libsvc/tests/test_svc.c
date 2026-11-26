// SPDX-License-Identifier: GPL-2.0-or-later
/* Native tests of libsvc. Build and run: sh libsvc/tests/run.sh
 *   test_svc                 the unit checks
 *   test_svc parse FILES...  parse every file, print problems, exit 1 if any
 *   test_svc show FILE KEY   print how we read KEY (Exec lines as argv), for comparing
 *                            with `systemctl show` on a host that runs systemd */
#include "../svc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks, failed;
#define CHECK(what, cond) do { checks++; if (!(cond)) { failed++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static int nwarn;
static void warn(void *ctx, const char *src, int line, const char *msg) { (void)ctx; nwarn++; printf("  %s:%d: %s\n", src, line, msg); }

static const char *env_get(void *ctx, const char *n) {
  (void)ctx;
  if (!strcmp(n, "MAINPID")) return "42";
  if (!strcmp(n, "OPTS")) return "-a  -b 'c d'";
  if (!strcmp(n, "EMPTY")) return "";
  return 0;
}

static void t_parse(void) {
  const char *t =
    "# comment\n; another\n[Unit]\nDescription = A test  \nAfter=a.service\nAfter=b.service c.service\n"
    "[Service]\nExecStart=/bin/x \\\n  --long \\\n# comment inside\n  --more\nEnvironment=A=1\nEnvironment=\nEnvironment=B=2\n"
    "Type=oneshot\r\n[X-Vendor]\nFoo=bar\n";
  struct svc_file f = {0};
  nwarn = 0;
  int bad = svc_parse(&f, t, strlen(t), "t1", warn, 0);
  CHECK("parse ok", bad == 0 && nwarn == 0);
  CHECK("description trimmed", svc_get(&f, "Unit", "Description") && !strcmp(svc_get(&f, "Unit", "Description"), "A test"));
  const char *v[8];
  CHECK("list keys", svc_getall(&f, "Unit", "After", v, 8) == 2 && !strcmp(v[1], "b.service c.service"));
  CHECK("continuation", !strcmp(svc_get(&f, "Service", "ExecStart"), "/bin/x  --long  --more"));
  CHECK("reset by empty", svc_getall(&f, "Service", "Environment", v, 8) == 1 && !strcmp(v[0], "B=2"));
  CHECK("CRLF", !strcmp(svc_get(&f, "Service", "Type"), "oneshot"));
  CHECK("vendor section", !strcmp(svc_get(&f, "X-Vendor", "Foo"), "bar"));
  CHECK("missing", svc_get(&f, "Service", "Nope") == 0);
  svc_file_free(&f);
  const char *b = "Key=outside\n[Unit\nnoequals\n[Unit]\n=novalue\n";
  nwarn = 0;
  bad = svc_parse(&f, b, strlen(b), "t2", 0, 0);
  CHECK("bad lines counted", bad == 4);
  svc_file_free(&f);
  /* a drop-in appended later overrides */
  { const char *a = "[Service]\nExecStart=/a\nRestart=no\n"; svc_parse(&f, a, strlen(a), "frag", 0, 0); }
  { const char *a = "[Service]\nExecStart=\nExecStart=/b\nRestart=always\n"; svc_parse(&f, a, strlen(a), "drop", 0, 0); }
  CHECK("drop-in reset", svc_getall(&f, "Service", "ExecStart", v, 8) == 1 && !strcmp(v[0], "/b"));
  CHECK("drop-in override", !strcmp(svc_get(&f, "Service", "Restart"), "always"));
  svc_file_free(&f);
}

static void t_values(void) {
  int64_t ms;
  CHECK("5s", svc_timespan("5s", &ms, 1000) == 0 && ms == 5000);
  CHECK("bare", svc_timespan("90", &ms, 1000) == 0 && ms == 90000);
  CHECK("1min 30s", svc_timespan("1min 30s", &ms, 1000) == 0 && ms == 90000);
  CHECK("1h30min", svc_timespan("1h30min", &ms, 1000) == 0 && ms == 5400000);
  CHECK("100ms", svc_timespan("100ms", &ms, 1000) == 0 && ms == 100);
  CHECK("100min", svc_timespan("100min", &ms, 1000) == 0 && ms == 6000000);
  CHECK("12h", svc_timespan("12h", &ms, 1000) == 0 && ms == 43200000);
  CHECK("0.5s", svc_timespan("0.5s", &ms, 1000) == 0 && ms == 500);
  CHECK("infinity", svc_timespan("infinity", &ms, 1000) == 0 && ms == SVC_INFINITY);
  CHECK("bad unit", svc_timespan("5 parsecs", &ms, 1000) < 0);
  CHECK("bool yes", svc_bool("yes", -1) == 1 && svc_bool("On", -1) == 1 && svc_bool("false", -1) == 0 && svc_bool("maybe", -1) == -1);
  char **w;
  int n = svc_words("a \"b c\" 'd\\'e' f\\x41 \\\"g", &w);
  CHECK("words", n == 5 && !strcmp(w[1], "b c") && !strcmp(w[2], "d'e") && !strcmp(w[3], "fA") && !strcmp(w[4], "\"g"));
  svc_free_words(w, n);
  CHECK("unbalanced", svc_words("a \"b", &w) < 0);
}

static void t_cmd(void) {
  struct svc_cmd *c;
  char err[100];
  int n = svc_cmdline("-/bin/kill -HUP $MAINPID", env_get, 0, &c, err, sizeof err);
  CHECK("kill line", n == 1 && (c[0].flags & SVC_EX_IGNORE) && c[0].argc == 3 && !strcmp(c[0].argv[2], "42") && !strcmp(c[0].path, "/bin/kill"));
  svc_cmds_free(c, n);
  n = svc_cmdline("/bin/x $OPTS ${OPTS} x${MAINPID}y $EMPTY ${EMPTY} '$OPTS' $$", env_get, 0, &c, err, sizeof err);
  CHECK("expansion count", n == 1 && c[0].argc == 10);
  if (n == 1 && c[0].argc == 10) {
    CHECK("$VAR split", !strcmp(c[0].argv[1], "-a") && !strcmp(c[0].argv[2], "-b") && !strcmp(c[0].argv[3], "'c") && !strcmp(c[0].argv[4], "d'"));
    CHECK("${VAR} one word", !strcmp(c[0].argv[5], "-a  -b 'c d'"));
    CHECK("${VAR} in word", !strcmp(c[0].argv[6], "x42y"));
    CHECK("${EMPTY} keeps a word", !strcmp(c[0].argv[7], ""));
    CHECK("single quotes", !strcmp(c[0].argv[8], "$OPTS") && !strcmp(c[0].argv[9], "$"));
  }
  svc_cmds_free(c, n);
  n = svc_cmdline("@/bin/sh myname -c \"echo hi\" ; /bin/true", 0, 0, &c, err, sizeof err);
  CHECK("argv0 and two commands", n == 2 && !strcmp(c[0].path, "/bin/sh") && !strcmp(c[0].argv[0], "myname") && !strcmp(c[0].argv[2], "echo hi") && !strcmp(c[1].path, "/bin/true"));
  svc_cmds_free(c, n);
  n = svc_cmdline(":/bin/echo $MAINPID", env_get, 0, &c, err, sizeof err);
  CHECK("no expansion", n == 1 && !strcmp(c[0].argv[1], "$MAINPID"));
  svc_cmds_free(c, n);
  CHECK("bad quotes", svc_cmdline("/bin/x \"open", 0, 0, &c, err, sizeof err) < 0);
  CHECK("prefix only", svc_cmdline("-", 0, 0, &c, err, sizeof err) < 0);
}

static int64_t mk(int y, int mo, int d, int h, int mi, int s) {
  struct tm tm = {0};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
  return (int64_t)timegm(&tm);
}
static void t_cal(void) {
  struct svc_cal c;
  int64_t base = mk(2026, 11, 26, 15, 47, 10); /* a Thursday */
  struct { const char *spec; int64_t want; } t[] = {
    {"daily", mk(2026, 11, 27, 0, 0, 0)},
    {"weekly", mk(2026, 11, 30, 0, 0, 0)},
    {"hourly", mk(2026, 11, 26, 16, 0, 0)},
    {"monthly", mk(2026, 12, 1, 0, 0, 0)},
    {"*-*-* 07..23:30", mk(2026, 11, 26, 16, 30, 0)},
    {"*-*-* 6:00", mk(2026, 11, 27, 6, 0, 0)},
    {"*-*-* 6,18:00", mk(2026, 11, 26, 18, 0, 0)},
    {"*-*-* 00,12:00:00", mk(2026, 11, 27, 0, 0, 0)},
    {"Sun *-*-* 03:10:00", mk(2026, 11, 29, 3, 10, 0)},
    {"Sun *-*-1..7 1:00:00", mk(2026, 12, 6, 1, 0, 0)},
    {"*-*-* 06:25:00", mk(2026, 11, 27, 6, 25, 0)},
    {"*:00/10", mk(2026, 11, 26, 15, 50, 0)},
    {"00:00", mk(2026, 11, 27, 0, 0, 0)},
    {"00:07:00", mk(2026, 11, 27, 0, 7, 0)},
    {"Mon..Fri 09:00", mk(2026, 11, 27, 9, 0, 0)},
    {"Sat,Sun 10:00", mk(2026, 11, 28, 10, 0, 0)},
    {"2027-02-29", -1},
    {"2028-02-29 12:00", mk(2028, 2, 29, 12, 0, 0)},
    {"*-12-25", mk(2026, 12, 25, 0, 0, 0)},
    {"minutely", mk(2026, 11, 26, 15, 48, 0)},
    {"quarterly", mk(2027, 1, 1, 0, 0, 0)},
    {"*-*-* *:*:30", mk(2026, 11, 26, 15, 47, 30)},
    {"daily UTC", mk(2026, 11, 27, 0, 0, 0)},
  };
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
    char what[80];
    snprintf(what, sizeof what, "calendar %s", t[i].spec);
    int ok = svc_cal_parse(t[i].spec, &c) == 0;
    int64_t got = ok ? svc_cal_next(&c, base) : -2;
    CHECK(what, ok && got == t[i].want);
    if (ok && got != t[i].want) printf("  got %lld want %lld\n", (long long)got, (long long)t[i].want);
  }
  CHECK("calendar bad", svc_cal_parse("Funday 10:00", &c) < 0 && svc_cal_parse("*-13-01", &c) < 0 && svc_cal_parse("25:00", &c) < 0);
  int y, mo, d, h, mi, s, wd;
  svc_civil(base, &y, &mo, &d, &h, &mi, &s, &wd);
  CHECK("civil", y == 2026 && mo == 11 && d == 26 && h == 15 && mi == 47 && s == 10 && wd == 3);
}

static void t_names(void) {
  char tm[64], in[64];
  CHECK("type", svc_name_type("a.service") == SVC_T_SERVICE && svc_name_type("b.timer") == SVC_T_TIMER && svc_name_type("c") == SVC_T_UNKNOWN);
  CHECK("instance", svc_name_split("getty@tty1.service", tm, sizeof tm, in, sizeof in) == 1 && !strcmp(tm, "getty@.service") && !strcmp(in, "tty1"));
  CHECK("template", svc_name_split("getty@.service", tm, sizeof tm, in, sizeof in) == 2);
  CHECK("plain", svc_name_split("ssh.service", tm, sizeof tm, in, sizeof in) == 0);
  CHECK("valid", svc_name_valid("a-b_c.d@x\\x2d.service") && !svc_name_valid("a b.service") && !svc_name_valid("x.foo"));
  struct svc_spec sp = {"postgresql@17-main.service", "/home/user", "user", "42lisboa", "0.1.0", 1000};
  int bad = 0;
  char *s = svc_specifiers("%n %N %p %i %I %f %t/%i.pid %h %u %U %H %% %Z", &sp, &bad);
  CHECK("specifiers", !strcmp(s, "postgresql@17-main.service postgresql@17-main postgresql 17-main 17/main /17/main /run/17-main.pid /home/user user 1000 42lisboa % %Z") && bad == 1);
  if (strcmp(s, "postgresql@17-main.service postgresql@17-main postgresql 17-main 17/main /17/main /run/17-main.pid /home/user user 1000 42lisboa % %Z")) printf("  got '%s'\n", s);
  free(s);
  char u[64];
  svc_unescape("dev-disk-by\\x2dlabel", u, sizeof u);
  CHECK("unescape", !strcmp(u, "dev/disk/by-label"));
}

static int parse_files(int n, char **files) {
  int problems = 0;
  for (int i = 0; i < n; i++) {
    struct svc_file f = {0};
    nwarn = 0;
    int bad = svc_parse_path(&f, files[i], warn, 0);
    if (bad < 0) { printf("cannot read %s\n", files[i]); problems++; continue; }
    /* every Exec line must split */
    for (int k = 0; k < f.n; k++) {
      if (strncmp(f.kv[k].key, "Exec", 4) || !f.kv[k].val[0]) continue;
      struct svc_cmd *c;
      char err[100];
      char *x = svc_specifiers(f.kv[k].val, &(struct svc_spec){"x@inst.service", 0, 0, 0, 0, 0}, 0);
      int m = svc_cmdline(x, 0, 0, &c, err, sizeof err);
      free(x);
      if (m < 0) { printf("  %s:%d: %s: %s\n", files[i], f.kv[k].line, f.kv[k].key, err); bad++; }
      else svc_cmds_free(c, m);
    }
    const char *oc = svc_get(&f, "Timer", "OnCalendar");
    if (oc) {
      const char *v[8];
      int m = svc_getall(&f, "Timer", "OnCalendar", v, 8);
      for (int j = 0; j < m; j++) { struct svc_cal c; if (svc_cal_parse(v[j], &c)) { printf("  %s: OnCalendar=%s not understood\n", files[i], v[j]); bad++; } }
    }
    static const char *const spans[] = {"RestartSec", "TimeoutStopSec", "TimeoutStartSec", "TimeoutSec", "RandomizedDelaySec",
                                        "OnBootSec", "OnUnitActiveSec", "OnActiveSec", "AccuracySec", "OnUnitInactiveSec", "WatchdogSec", 0};
    for (int k = 0; k < f.n; k++)
      for (int j = 0; spans[j]; j++)
        if (!strcmp(f.kv[k].key, spans[j]) && f.kv[k].val[0]) {
          int64_t ms;
          if (svc_timespan(f.kv[k].val, &ms, 1000)) { printf("  %s:%d: %s=%s not understood\n", files[i], f.kv[k].line, spans[j], f.kv[k].val); bad++; }
        }
    if (bad) problems++;
    svc_file_free(&f);
  }
  printf("parsed %d files, %d with problems\n", n, problems);
  return problems != 0;
}

/* Print our reading of an Exec key in the shape of systemctl show's "argv[]=" field. */
static int show(const char *file, const char *key) {
  struct svc_file f = {0};
  if (svc_parse_path(&f, file, 0, 0) < 0) return 2;
  const char *v[32];
  int n = svc_getall(&f, "Service", key, v, 32);
  const char *nm = strrchr(file, '/');
  struct svc_spec sp = {nm ? nm + 1 : file, 0, 0, 0, 0, 0};
  for (int i = 0; i < n; i++) {
    struct svc_cmd *c;
    char err[100];
    char *x = svc_specifiers(v[i], &sp, 0);
    /* systemctl show prints command lines before variable expansion: read with ":" */
    char *y = malloc(strlen(x) + 2);
    y[0] = ':'; strcpy(y + 1, x);
    int m = svc_cmdline(y, 0, 0, &c, err, sizeof err);
    free(x); free(y);
    for (int j = 0; j < m; j++) {
      printf("path=%s ; argv[]=", c[j].path);
      for (int k = 0; k < c[j].argc; k++) printf("%s%s", k ? " " : "", c[j].argv[k]);
      printf(" ; ignore_errors=%s\n", (c[j].flags & SVC_EX_IGNORE) ? "yes" : "no");
    }
    if (m > 0) svc_cmds_free(c, m);
  }
  svc_file_free(&f);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "parse")) return parse_files(argc - 2, argv + 2);
  if (argc > 3 && !strcmp(argv[1], "show")) return show(argv[2], argv[3]);
  t_parse(); t_values(); t_cmd(); t_cal(); t_names();
  printf("libsvc: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
