// SPDX-License-Identifier: GPL-2.0-or-later
/* systemctl: control the service manager (bin/svcd). The commands, options, output
 * and exit codes follow systemctl(1) for the subset listed in docs/SERVICES.md, so
 * scripts and documentation written for systemd apply. Runtime commands go to the
 * manager over its socket; enable, disable, mask, is-enabled, cat and list-unit-files
 * work on the unit files directly and so also work when the manager is not running. */
#include "../../libsvc/svc.h"
#include "../../libsvc/os.h"
#include "../../libsvc/proto.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int quiet, all, no_block, now_opt, value_only, no_legend, full, force, lines_opt = 10;
static char props[1024], type_filter[64], state_filter[64], job_mode[32] = "", signal_opt[16] = "15", whom[16] = "all";
static char root_opt[200];
static struct svc_roots roots;
static int color;

static const char *sock_path(void) { const char *e = getenv("SVCD_SOCKET"); return e && *e ? e : SVC_CONTROL; }

/* Send one request; collect the reply lines in *out (malloc'ed), return the "=rc". */
static int request(const char *req, char **out, int show_errors) {
  int fd = svc_connect_unix(sock_path());
  if (fd < 0) {
    if (show_errors) fprintf(stderr, "System has not been booted with the service manager as init system, or it is not running.\nFailed to connect to %s: %s\n", sock_path(), strerror(-fd));
    if (out) *out = 0;
    return -1;
  }
  size_t l = strlen(req);
  if (write(fd, req, l) != (ssize_t)l || write(fd, "\n", 1) != 1) { close(fd); return -1; }
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  for (;;) {
    if (n + 1024 > cap) { cap *= 2; b = realloc(b, cap); }
    ssize_t r = read(fd, b + n, cap - n - 1);
    if (r <= 0) break;
    n += r;
  }
  close(fd);
  b[n] = 0;
  int rc = 1;
  char *p = strstr(b, "=rc ");
  if (p && (p == b || p[-1] == '\n')) { rc = atoi(p + 4); *p = 0; }
  /* "!" lines are errors for stderr */
  if (show_errors)
    for (char *s = b; *s;) {
      char *e = strchr(s, '\n');
      if (!e) e = s + strlen(s);
      if (*s == '!') fprintf(stderr, "%.*s\n", (int)(e - s - 1), s + 1);
      s = *e ? e + 1 : e;
    }
  if (out) *out = b; else free(b);
  return rc;
}

static void norm(const char *in, char *out, size_t ol) {
  if (svc_name_type(in) != SVC_T_UNKNOWN) snprintf(out, ol, "%s", in);
  else snprintf(out, ol, "%s.service", in);
}

/* properties of one unit as returned by "show" */
struct props { char *text; };
static const char *prop(const char *text, const char *key) {
  static char ring[8][1024];
  static int next;
  char *v = ring[next++ & 7]; /* several values may be in use at once */
  size_t kl = strlen(key);
  for (const char *s = text; s && *s;) {
    const char *e = strchr(s, '\n');
    if (!e) e = s + strlen(s);
    if ((size_t)(e - s) > kl && !strncmp(s, key, kl) && s[kl] == '=') {
      snprintf(v, 1024, "%.*s", (int)(e - s - kl - 1), s + kl + 1);
      return v;
    }
    s = *e ? e + 1 : e;
  }
  return 0;
}
static char *show(const char *unit) {
  char req[300], *out;
  snprintf(req, sizeof req, "show %s", unit);
  if (request(req, &out, 1) < 0) return 0;
  return out;
}

static const char *ago(int64_t t_ms, char *b, size_t bl) {
  int64_t d = (svc_epoch_ms() - t_ms) / 1000;
  if (d < 0) d = 0;
  if (d < 60) snprintf(b, bl, "%llds ago", (long long)d);
  else if (d < 3600) snprintf(b, bl, "%lldmin %llds ago", (long long)d / 60, (long long)d % 60);
  else if (d < 86400) snprintf(b, bl, "%lldh %lldmin ago", (long long)d / 3600, (long long)d / 60 % 60);
  else snprintf(b, bl, "%lld days ago", (long long)d / 86400);
  return b;
}
static const char *stamp(int64_t t_ms, char *b, size_t bl) {
  time_t t = (time_t)(t_ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(b, bl, "%a %Y-%m-%d %H:%M:%S UTC", &tm);
  return b;
}
static const char *bytes_h(unsigned long long v, char *b, size_t bl) {
  if (v >= 1024ULL * 1024 * 1024) snprintf(b, bl, "%.1fG", v / (1024.0 * 1024 * 1024));
  else if (v >= 1024 * 1024) snprintf(b, bl, "%.1fM", v / (1024.0 * 1024));
  else if (v >= 1024) snprintf(b, bl, "%.1fK", v / 1024.0);
  else snprintf(b, bl, "%lluB", v);
  return b;
}

static void print_journal_lines(const char *text, int short_fmt);

static int cmd_status(int n, char **units) {
  int rc = 0;
  if (n == 0) {
    char *out;
    if (request("status-manager", &out, 1) < 0) return 1;
    char *failed;
    request("list-units", &failed, 0);
    int nf = 0, nj = 0;
    for (char *s = failed; s && *s;) { char *e = strchr(s, '\n'); if (!e) break; if (strstr(s, "\tfailed\t")) nf++; s = e + 1; }
    const char *j = prop(out, "Jobs");
    nj = j ? atoi(j) : 0;
    char host[64] = "42lisboa";
    gethostname(host, sizeof host);
    printf("%s %s\n    State: %s\n    Units: %s loaded\n     Jobs: %d queued\n   Failed: %d units\n  Journal: %s records, %s of %s bytes used\n",
           color ? (nf ? "\033[31m●\033[0m" : "\033[32m●\033[0m") : "●", host, nf ? "degraded" : nj ? "starting" : "running", prop(out, "Units"), nj, nf,
           prop(out, "JournalRecords"), prop(out, "JournalUsed"), prop(out, "JournalBytes"));
    free(out); free(failed);
    return 0;
  }
  for (int i = 0; i < n; i++) {
    char name[256];
    norm(units[i], name, sizeof name);
    char *p = show(name);
    if (!p) return 1;
    const char *load = prop(p, "LoadState");
    if (load && !strcmp(load, "not-found") && !(prop(p, "ActiveState") && strcmp(prop(p, "ActiveState"), "inactive"))) {
      fprintf(stderr, "Unit %s could not be found.\n", name);
      free(p);
      rc = 4;
      continue;
    }
    char active[32], sub[32], desc[200], frag[300], ufs[32], result[32];
    snprintf(active, sizeof active, "%s", prop(p, "ActiveState") ? prop(p, "ActiveState") : "?");
    snprintf(sub, sizeof sub, "%s", prop(p, "SubState") ? prop(p, "SubState") : "?");
    snprintf(desc, sizeof desc, "%s", prop(p, "Description") ? prop(p, "Description") : name);
    snprintf(frag, sizeof frag, "%s", prop(p, "FragmentPath") ? prop(p, "FragmentPath") : "");
    snprintf(ufs, sizeof ufs, "%s", prop(p, "UnitFileState") ? prop(p, "UnitFileState") : "");
    snprintf(result, sizeof result, "%s", prop(p, "Result") ? prop(p, "Result") : "");
    const char *dot = "●";
    const char *c1 = "", *c0 = "";
    if (color) { c0 = "\033[0m"; c1 = !strcmp(active, "active") ? "\033[32m" : !strcmp(active, "failed") ? "\033[31m" : !strcmp(active, "inactive") ? "" : "\033[33m"; }
    if (i) printf("\n");
    printf("%s%s%s %s - %s\n", c1, !strcmp(active, "active") || !strcmp(active, "inactive") ? dot : !strcmp(active, "failed") ? "×" : dot, c0, name, desc);
    printf("     Loaded: %s (%s%s%s)\n", load, frag, ufs[0] ? "; " : "", ufs);
    const char *dr = prop(p, "DropInPaths");
    if (dr && *dr) {
      char d[1024];
      snprintf(d, sizeof d, "%s", dr);
      int first = 1;
      for (char *t = strtok(d, " "); t; t = strtok(0, " ")) { printf("%s%s\n", first ? "    Drop-In: " : "             ", t); first = 0; }
    }
    char b1[64], b2[64];
    const char *since = !strcmp(active, "active") ? prop(p, "ActiveEnterTimestamp") : prop(p, "StateChangeTimestamp");
    if (since) {
      int64_t t = atoll(since);
      printf("     Active: %s%s (%s)%s%s%s since %s; %s\n", c1, active, sub, c0, !strcmp(active, "failed") ? " (Result: " : "", !strcmp(active, "failed") ? result : "",
             stamp(t, b1, sizeof b1), ago(t, b2, sizeof b2));
      if (!strcmp(active, "failed")) {}
    } else printf("     Active: %s%s (%s)%s\n", c1, active, sub, c0);
    const char *trig = prop(p, "TriggeredBy");
    if (trig && *trig) printf("TriggeredBy: %s\n", trig);
    const char *tr = prop(p, "Triggers");
    if (tr && *tr && strstr(name, ".timer")) printf("   Triggers: ● %s\n", tr);
    const char *ne = prop(p, "NextElapse");
    if (ne && strstr(name, ".timer")) { int64_t t = atoll(ne); printf("    Trigger: %s; in %llds\n", stamp(t, b1, sizeof b1), (long long)((t - svc_epoch_ms()) / 1000)); }
    const char *mp = prop(p, "MainPID");
    if (mp && atoi(mp) > 0) printf("   Main PID: %s\n", mp);
    else if (strstr(name, ".service") && prop(p, "ExecMainCode") && atoi(prop(p, "ExecMainCode")) && strcmp(active, "active")) {
      int code = atoi(prop(p, "ExecMainCode")), st = atoi(prop(p, "ExecMainStatus"));
      printf("    Process: (code=%s, status=%d%s)\n", code == 1 ? "exited" : "killed", st, code == 1 ? (st == 0 ? "/SUCCESS" : "/FAILURE") : "");
    }
    const char *stt = prop(p, "StatusText");
    if (stt && *stt) printf("     Status: \"%s\"\n", stt);
    const char *mem = prop(p, "MemoryCurrent");
    if (mem && atoll(mem) > 0 && !strcmp(active, "active")) {
      char m1[32], m2[32];
      unsigned long long cur = strtoull(mem, 0, 10), peak = strtoull(prop(p, "MemoryPeak") ? prop(p, "MemoryPeak") : "0", 0, 10);
      printf("     Memory: %s (peak: %s)\n", bytes_h(cur, m1, sizeof m1), bytes_h(peak, m2, sizeof m2));
    }
    const char *procs = prop(p, "Processes");
    if (procs && *procs) {
      char pl[600];
      snprintf(pl, sizeof pl, "%s", procs);
      printf("     CGroup: %s (process group)\n", prop(p, "ControlGroup") ? prop(p, "ControlGroup") + 5 : "");
      int count = 0;
      for (char *t = strtok(pl, " "); t; t = strtok(0, " ")) count++;
      snprintf(pl, sizeof pl, "%s", procs);
      int k = 0;
      for (char *t = strtok(pl, " "); t; t = strtok(0, " ")) printf("             %s%s\n", ++k == count ? "└─" : "├─", t);
    }
    if (!strcmp(active, "active") || !strcmp(active, "reloading") || !strcmp(active, "activating") || !strcmp(active, "deactivating")) {}
    else if (rc == 0) rc = 3;
    free(p);
    if (lines_opt > 0) {
      char req[300], *out;
      snprintf(req, sizeof req, "journal -u %s -n %d", name, lines_opt);
      if (request(req, &out, 0) >= 0 && out && *out) { printf("\n"); print_journal_lines(out, 1); }
      free(out);
    }
  }
  return rc;
}

static void print_journal_lines(const char *text, int short_fmt) {
  char host[64] = "42lisboa";
  gethostname(host, sizeof host);
  for (const char *s = text; *s;) {
    const char *e = strchr(s, '\n');
    if (!e) e = s + strlen(s);
    /* seq t prio pid unit ident text */
    char line[4400];
    snprintf(line, sizeof line, "%.*s", (int)(e - s), s);
    char *f[7];
    int nf = 0;
    char *q = line;
    while (nf < 6) { f[nf++] = q; q = strchr(q, '\t'); if (!q) break; *q++ = 0; }
    if (nf == 6 && q) {
      f[6] = q;
      time_t t = (time_t)(atoll(f[1]) / 1000);
      struct tm tm;
      gmtime_r(&t, &tm);
      char ts[32];
      strftime(ts, sizeof ts, "%b %d %H:%M:%S", &tm);
      (void)short_fmt;
      printf("%s %s %s[%s]: %s\n", ts, host, f[5], f[3], f[6]);
    }
    s = *e ? e + 1 : e;
  }
}

static int cmd_show(int n, char **units) {
  if (n == 0) { char *out; int r = request("status-manager", &out, 1); if (r >= 0) { printf("%s", out); free(out); } return r < 0 ? 1 : 0; }
  for (int i = 0; i < n; i++) {
    char name[256];
    norm(units[i], name, sizeof name);
    char *p = show(name);
    if (!p) return 1;
    if (i) printf("\n");
    for (char *s = p; *s;) {
      char *e = strchr(s, '\n');
      if (!e) break;
      *e = 0;
      if (*s) {
        char *eq = strchr(s, '=');
        int want = !props[0];
        if (!want && eq) {
          char pl[1100];
          snprintf(pl, sizeof pl, ",%s,", props);
          char key[200];
          snprintf(key, sizeof key, ",%.*s,", (int)(eq - s), s);
          want = strstr(pl, key) != 0;
        }
        if (want && eq && (all || eq[1] || props[0])) printf("%s\n", value_only ? eq + 1 : s);
      }
      s = e + 1;
    }
    free(p);
  }
  return 0;
}

static int cmd_is(const char *what, int n, char **units) {
  int any = 0;
  for (int i = 0; i < n; i++) {
    char name[256];
    norm(units[i], name, sizeof name);
    char *p = show(name);
    if (!p) { if (!quiet) printf("inactive\n"); continue; }
    const char *a = prop(p, "ActiveState");
    const char *l = prop(p, "LoadState");
    char st[32];
    snprintf(st, sizeof st, "%s", a ? a : "unknown");
    if (l && !strcmp(l, "not-found") && !strcmp(st, "inactive")) snprintf(st, sizeof st, "inactive");
    if (!quiet) printf("%s\n", st);
    if (!strcmp(what, "is-active") && (!strcmp(st, "active") || !strcmp(st, "reloading"))) any = 1;
    if (!strcmp(what, "is-failed") && !strcmp(st, "failed")) any = 1;
    free(p);
  }
  if (!strcmp(what, "is-active")) return any ? 0 : 3;
  return any ? 0 : 1;
}

static int is_enabled_rc(const char *s) {
  return s && (!strcmp(s, "enabled") || !strcmp(s, "static") || !strcmp(s, "indirect") || !strcmp(s, "alias") || !strcmp(s, "generated"));
}
static int cmd_is_enabled(int n, char **units) {
  int any = 0, missing = 0;
  for (int i = 0; i < n; i++) {
    char name[256];
    norm(units[i], name, sizeof name);
    const char *s = svc_is_enabled(&roots, name);
    if (!s) { fprintf(stderr, "Failed to get unit file state for %s: No such file or directory\n", name); missing = 1; continue; }
    if (!quiet) printf("%s\n", s);
    if (is_enabled_rc(s)) any = 1;
  }
  return any ? 0 : missing ? 1 : 1;
}

static void reload_if_running(void) {
  char *out;
  if (request("daemon-reload", &out, 0) >= 0) free(out);
}
static int jobs(const char *verb, int n, char **units);

static void say_change(struct svc_change *c) {
  if (quiet) return;
  if (c->removed) printf("Removed \"%s\".\n", c->path);
  else printf("Created %s %s → %s.\n", svc_have_symlinks() ? "symlink" : "link file", c->path, c->target);
}
static int cmd_enable(const char *verb, int n, char **units) {
  int rc = 0;
  char names[64][256];
  int nn = 0;
  for (int i = 0; i < n && nn < 64; i++) norm(units[i], names[nn++], sizeof names[0]);
  for (int i = 0; i < nn; i++) {
    struct svc_change ch[64];
    char err[300] = "";
    if (!strcmp(verb, "disable") || !strcmp(verb, "reenable")) {
      int k = svc_disable(&roots, names[i], ch, 64);
      for (int j = 0; j < k && j < 64; j++) say_change(&ch[j]);
      if (!strcmp(verb, "disable")) {
        if (svc_find(&roots, names[i], err, sizeof err, 0, 0, 0) < 0) { fprintf(stderr, "Failed to disable unit: Unit %s does not exist\n", names[i]); rc = 1; }
        continue;
      }
    }
    int k = svc_enable(&roots, names[i], 0, ch, 64, err, sizeof err);
    if (k < 0) { fprintf(stderr, "Failed to enable unit: %s\n", err); rc = 1; continue; }
    for (int j = 0; j < k && j < 64; j++) say_change(&ch[j]);
    if (k == 0 && !quiet) {
      const char *s = svc_is_enabled(&roots, names[i]);
      if (s && !strcmp(s, "static"))
        printf("The unit files have no installation config (WantedBy=, RequiredBy=, UpheldBy=, Also=,\nor Alias= settings in the [Install] section, and DefaultInstance= for template\nunits). This means they are not meant to be enabled or disabled using systemctl.\n");
    }
  }
  reload_if_running();
  if (now_opt && !rc) {
    char *argv2[64];
    for (int i = 0; i < nn; i++) argv2[i] = names[i];
    return jobs(!strcmp(verb, "disable") ? "stop" : !strcmp(verb, "reenable") ? "restart" : "start", nn, argv2);
  }
  return rc;
}

static int cmd_mask(int unmask, int n, char **units) {
  int rc = 0;
  for (int i = 0; i < n; i++) {
    char name[256], p[400];
    norm(units[i], name, sizeof name);
    int r = svc_mask(&roots, name, unmask);
    snprintf(p, sizeof p, "%s/etc/systemd/system/%s", roots.root[0], name);
    if (unmask) { if (r > 0 && !quiet) printf("Removed \"%s\".\n", p); continue; }
    if (r == -EEXIST) { fprintf(stderr, "Failed to mask unit: File %s already exists.\n", p); rc = 1; continue; }
    if (r < 0) { fprintf(stderr, "Failed to mask unit: %s\n", strerror(-r)); rc = 1; continue; }
    if (!quiet) printf("Created %s %s → /dev/null.\n", svc_have_symlinks() ? "symlink" : "link file", p);
  }
  reload_if_running();
  if (!unmask && now_opt) return jobs("stop", n, units);
  return rc;
}

static int jobs(const char *verb, int n, char **units) {
  if (!n) { fprintf(stderr, "Too few arguments.\n"); return 1; }
  char req[4096];
  int l = snprintf(req, sizeof req, "%s%s", verb, no_block ? " --no-block" : "");
  if (job_mode[0]) l += snprintf(req + l, sizeof req - l, " --job-mode=%s", job_mode);
  for (int i = 0; i < n && l < (int)sizeof req - 300; i++) {
    char name[256];
    norm(units[i], name, sizeof name);
    l += snprintf(req + l, sizeof req - l, " %s", name);
  }
  int rc = request(req, 0, 1);
  return rc < 0 ? 1 : rc;
}

static int cmd_list_units(void) {
  char *out;
  if (request(all ? "list-units --all" : "list-units", &out, 1) < 0) return 1;
  struct row { char *f[5]; } rows[800];
  int nr = 0;
  int w0 = 4, w1 = 4, w2 = 6, w3 = 3;
  for (char *s = out; *s && nr < 800;) {
    char *e = strchr(s, '\n');
    if (!e) break;
    *e = 0;
    char *f[5];
    int k = 0;
    for (char *t = s; k < 5;) { f[k++] = t; t = strchr(t, '\t'); if (!t) break; *t++ = 0; }
    if (k == 5) {
      int keep = 1;
      if (type_filter[0]) { const char *d = strrchr(f[0], '.'); keep = d && !strcmp(d + 1, type_filter); }
      if (state_filter[0] && strcmp(f[2], state_filter) && strcmp(f[3], state_filter) && strcmp(f[1], state_filter)) keep = 0;
      if (keep) {
        memcpy(rows[nr].f, f, sizeof f);
        if ((int)strlen(f[0]) > w0) w0 = strlen(f[0]);
        if ((int)strlen(f[1]) > w1) w1 = strlen(f[1]);
        if ((int)strlen(f[2]) > w2) w2 = strlen(f[2]);
        if ((int)strlen(f[3]) > w3) w3 = strlen(f[3]);
        nr++;
      }
    }
    s = e + 1;
  }
  if (!no_legend) printf("  %-*s %-*s %-*s %-*s %s\n", w0, "UNIT", w1, "LOAD", w2, "ACTIVE", w3, "SUB", "DESCRIPTION");
  for (int i = 0; i < nr; i++) {
    const char *mark = !strcmp(rows[i].f[2], "failed") ? (color ? "\033[31m●\033[0m" : "●") : " ";
    printf("%s %-*s %-*s %-*s %-*s %s\n", mark, w0, rows[i].f[0], w1, rows[i].f[1], w2, rows[i].f[2], w3, rows[i].f[3], rows[i].f[4]);
  }
  if (!no_legend) {
    printf("\nLegend: LOAD   → Reflects whether the unit definition was properly loaded.\n        ACTIVE → The high-level unit activation state, i.e. generalization of SUB.\n        SUB    → The low-level unit activation state, values depend on unit type.\n\n");
    printf("%d loaded units listed.%s\n", nr, all ? "" : " Pass --all to see loaded but inactive units, too.");
  }
  free(out);
  return 0;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmd_list_unit_files(void) {
  char *names[2048];
  int n = 0;
  for (int ri = 0; ri < roots.n; ri++)
    for (int d = 0; ; d++) {
      char dir[400];
      if (!svc_unit_dir(&roots, ri, d, dir, sizeof dir)) break;
      DIR *dd = opendir(dir);
      if (!dd) continue;
      struct dirent *de;
      while ((de = readdir(dd)) && n < 2048) {
        if (!svc_name_valid(de->d_name)) continue;
        if (type_filter[0]) { const char *x = strrchr(de->d_name, '.'); if (!x || strcmp(x + 1, type_filter)) continue; }
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(names[i], de->d_name)) { dup = 1; break; }
        if (!dup) names[n++] = strdup(de->d_name);
      }
      closedir(dd);
    }
  qsort(names, n, sizeof *names, cmp);
  int w = 9;
  for (int i = 0; i < n; i++) if ((int)strlen(names[i]) > w) w = strlen(names[i]);
  if (!no_legend) printf("%-*s STATE           PRESET\n", w, "UNIT FILE");
  int shown = 0;
  for (int i = 0; i < n; i++) {
    const char *s = svc_is_enabled(&roots, names[i]);
    if (!s) s = "bad";
    if (svc_name_split(names[i], 0, 0, 0, 0) == 1) { free(names[i]); continue; } /* instances are not unit files */
    if (state_filter[0] && strcmp(s, state_filter)) { free(names[i]); continue; }
    printf("%-*s %-15s -\n", w, names[i], s);
    shown++;
    free(names[i]);
  }
  if (!no_legend) printf("\n%d unit files listed.\n", shown);
  return 0;
}

static int cmd_cat(int n, char **units) {
  int rc = 0;
  for (int i = 0; i < n; i++) {
    char name[256], path[512];
    norm(units[i], name, sizeof name);
    struct svc_file f = {0};
    int ri;
    int e = svc_load(&roots, name, &f, path, sizeof path, &ri, 0, 0);
    if (e == 1) { fprintf(stderr, "Unit %s is masked.\n", name); rc = 1; continue; }
    if (e) { fprintf(stderr, "No files found for %s.\n", name); rc = 1; svc_file_free(&f); continue; }
    for (int k = 0; k < f.nsrc; k++) {
      if (i || k) printf("\n");
      printf("# %s\n", f.srcs[k]);
      FILE *fp = fopen(f.srcs[k], "r");
      if (!fp) continue;
      char b[1024];
      size_t r;
      while ((r = fread(b, 1, sizeof b, fp)) > 0) fwrite(b, 1, r, stdout);
      fclose(fp);
    }
    svc_file_free(&f);
  }
  return rc;
}

static int cmd_list_timers(void) {
  char *out;
  if (request("list-units --all", &out, 1) < 0) return 1;
  printf("%-30s %-12s %-30s %-12s %-24s %s\n", "NEXT", "LEFT", "LAST", "PASSED", "UNIT", "ACTIVATES");
  int n = 0;
  for (char *s = out; *s;) {
    char *e = strchr(s, '\n');
    if (!e) break;
    *e = 0;
    char *tab = strchr(s, '\t');
    if (tab) *tab = 0;
    if (strstr(s, ".timer") && tab && strstr(tab + 1, "\tactive\t")) {
      char *p = show(s);
      if (p) {
        char b1[64] = "-", b2[32] = "-", b3[64] = "-", b4[32] = "-", act[200] = "";
        const char *ne = prop(p, "NextElapse");
        if (ne) { int64_t t = atoll(ne); stamp(t, b1, sizeof b1); snprintf(b2, sizeof b2, "%llds", (long long)((t - svc_epoch_ms()) / 1000)); }
        const char *lt = prop(p, "LastTrigger");
        if (lt) { int64_t t = atoll(lt); stamp(t, b3, sizeof b3); ago(t, b4, sizeof b4); }
        snprintf(act, sizeof act, "%s", prop(p, "Triggers") ? prop(p, "Triggers") : "");
        printf("%-30s %-12s %-30s %-12s %-24s %s\n", b1, b2, b3, b4, s, act);
        n++;
        free(p);
      }
    }
    s = e + 1;
  }
  printf("\n%d timers listed.\n", n);
  free(out);
  return 0;
}

static int cmd_default(int set, const char *t) {
  char p[400], v[300];
  snprintf(p, sizeof p, "%s/etc/systemd/system/default.target", roots.root[0]);
  if (set) {
    char name[256], path[512];
    norm(t, name, sizeof name);
    int ri;
    if (svc_find(&roots, name, path, sizeof path, &ri, 0, 0) != 0) { fprintf(stderr, "Failed to set default target: Unit %s does not exist.\n", name); return 1; }
    svc_makelink(path + strlen(roots.root[ri]), p);
    if (!quiet) printf("Created %s %s → %s.\n", svc_have_symlinks() ? "symlink" : "link file", p, path + strlen(roots.root[ri]));
    return 0;
  }
  char real[256];
  int ri;
  if (svc_find(&roots, "default.target", v, sizeof v, &ri, real, sizeof real) != 0) { fprintf(stderr, "Failed to get default target: No such file or directory\n"); return 1; }
  printf("%s\n", real);
  return 0;
}

static void usage(void) {
  printf("systemctl [OPTIONS...] COMMAND ...\n\nQuery or send control commands to the service manager.\n\n"
         "Unit commands:\n  list-units  list-timers  list-jobs  status [UNIT...]  show [UNIT...]  cat UNIT...\n"
         "  is-active UNIT...  is-failed UNIT...  start  stop  restart  try-restart  reload\n"
         "  reload-or-restart  try-reload-or-restart  isolate  kill  reset-failed\n"
         "Unit file commands:\n  list-unit-files  enable  disable  reenable  mask  unmask  is-enabled\n  get-default  set-default TARGET\n"
         "Manager:\n  daemon-reload  daemon-reexec  is-system-running  poweroff  reboot  halt  soft-reboot\n\n"
         "Options: -a --all  -q --quiet  --no-block  --now  -p --property=NAME  --value  -t --type=TYPE\n"
         "  --state=STATE  --no-legend  --plain  -l --full  -n --lines=N  -s --signal=SIG  --kill-whom=main|all\n"
         "  --job-mode=MODE  --root=DIR  --no-pager  --system  --version\n\nSee man systemctl and docs/SERVICES.md.\n");
}

int main(int argc, char **argv) {
  char *args[256];
  int na = 0;
  color = isatty(1) && getenv("TERM") && strcmp(getenv("TERM"), "dumb");
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { for (i++; i < argc && na < 256; i++) args[na++] = argv[i]; break; }
    if (a[0] != '-' || !a[1]) { if (na < 256) args[na++] = a; continue; }
    if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "-a") || !strcmp(a, "--all")) all = 1;
    else if (!strcmp(a, "--no-block")) no_block = 1;
    else if (!strcmp(a, "--now")) now_opt = 1;
    else if (!strcmp(a, "--value")) value_only = 1;
    else if (!strcmp(a, "--no-legend")) no_legend = 1;
    else if (!strcmp(a, "-l") || !strcmp(a, "--full") || !strcmp(a, "--plain") || !strcmp(a, "--no-pager") || !strcmp(a, "--system") ||
             !strcmp(a, "--no-ask-password") || !strcmp(a, "--runtime") || !strcmp(a, "--no-reload") || !strcmp(a, "--global")) full = 1;
    else if (!strcmp(a, "-f") || !strcmp(a, "--force")) force = 1;
    else if (!strcmp(a, "--user")) { fprintf(stderr, "UC.OS has one user and one manager: --user is not supported; use the system manager.\n"); return 1; }
    else if (!strcmp(a, "--version")) { printf("systemctl for UC.OS (subset of systemd's interface; see docs/SERVICES.md)\n"); return 0; }
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
    else if ((!strcmp(a, "-p") || !strcmp(a, "-t") || !strcmp(a, "-n") || !strcmp(a, "-s") || !strcmp(a, "-o")) && i + 1 < argc) {
      char *v = argv[++i];
      if (a[1] == 'p') { if (props[0]) strncat(props, ",", sizeof props - strlen(props) - 1); strncat(props, v, sizeof props - strlen(props) - 1); }
      else if (a[1] == 't') snprintf(type_filter, sizeof type_filter, "%s", v);
      else if (a[1] == 'n') lines_opt = atoi(v);
      else if (a[1] == 's') snprintf(signal_opt, sizeof signal_opt, "%s", v);
    } else if (!strncmp(a, "--property=", 11)) { if (props[0]) strncat(props, ",", sizeof props - strlen(props) - 1); strncat(props, a + 11, sizeof props - strlen(props) - 1); }
    else if (!strncmp(a, "--type=", 7)) snprintf(type_filter, sizeof type_filter, "%s", a + 7);
    else if (!strncmp(a, "--state=", 8)) snprintf(state_filter, sizeof state_filter, "%s", a + 8);
    else if (!strncmp(a, "--lines=", 8)) lines_opt = atoi(a + 8);
    else if (!strncmp(a, "--job-mode=", 11)) snprintf(job_mode, sizeof job_mode, "%s", a + 11);
    else if (!strncmp(a, "--signal=", 9)) snprintf(signal_opt, sizeof signal_opt, "%s", a + 9);
    else if (!strncmp(a, "--kill-whom=", 12) || !strncmp(a, "--kill-who=", 11)) snprintf(whom, sizeof whom, "%s", strchr(a, '=') + 1);
    else if (!strncmp(a, "--root=", 7)) snprintf(root_opt, sizeof root_opt, "%s", a + 7);
    else if (!strncmp(a, "--output=", 9) || !strncmp(a, "--no-wall", 9) || !strncmp(a, "--message=", 10)) {}
    else { fprintf(stderr, "systemctl: unrecognized option '%s'\n", a); return 1; }
  }
  svc_roots_default(&roots, getenv("SVC_GUEST_ROOT") ? getenv("SVC_GUEST_ROOT") : "/debian");
  if (root_opt[0]) { snprintf(roots.root[0], sizeof roots.root[0], "%s", strcmp(root_opt, "/") ? root_opt : ""); roots.n = 1; }
  if (getenv("SVC_ROOT")) snprintf(roots.root[0], sizeof roots.root[0], "%s", getenv("SVC_ROOT")); /* tests on a host */
  (void)force; (void)full;
  if (!na) return cmd_list_units();
  const char *v = args[0];
  char **u = args + 1;
  int nu = na - 1;
  if (!strcmp(v, "list-units")) return cmd_list_units();
  if (!strcmp(v, "list-unit-files")) return cmd_list_unit_files();
  if (!strcmp(v, "list-timers")) return cmd_list_timers();
  if (!strcmp(v, "list-jobs")) {
    char *out;
    if (request("list-jobs", &out, 1) < 0) return 1;
    int n = 0;
    for (char *s = out; *s; s++) if (*s == '\n') n++;
    if (!n) printf("No jobs running.\n");
    else { printf("JOB UNIT TYPE STATE\n"); for (char *s = out; *s; s++) putchar(*s == '\t' ? ' ' : *s); printf("\n%d jobs listed.\n", n); }
    free(out);
    return 0;
  }
  if (!strcmp(v, "status")) return cmd_status(nu, u);
  if (!strcmp(v, "show")) return cmd_show(nu, u);
  if (!strcmp(v, "cat")) return cmd_cat(nu, u);
  if (!strcmp(v, "is-active") || !strcmp(v, "is-failed")) return cmd_is(v, nu, u);
  if (!strcmp(v, "is-enabled")) return cmd_is_enabled(nu, u);
  if (!strcmp(v, "enable") || !strcmp(v, "disable") || !strcmp(v, "reenable")) return cmd_enable(v, nu, u);
  if (!strcmp(v, "mask") || !strcmp(v, "unmask")) return cmd_mask(!strcmp(v, "unmask"), nu, u);
  if (!strcmp(v, "get-default")) return cmd_default(0, 0);
  if (!strcmp(v, "set-default")) { if (!nu) { fprintf(stderr, "Too few arguments.\n"); return 1; } return cmd_default(1, u[0]); }
  if (!strcmp(v, "start") || !strcmp(v, "stop") || !strcmp(v, "restart") || !strcmp(v, "reload") || !strcmp(v, "try-restart") ||
      !strcmp(v, "condrestart") || !strcmp(v, "reload-or-restart") || !strcmp(v, "try-reload-or-restart") || !strcmp(v, "force-reload") ||
      !strcmp(v, "isolate"))
    return jobs(v, nu, u);
  if (!strcmp(v, "load-all")) { /* measurement hook (docs/SERVICES.md): load every unit file, print the times */
    char *out;
    int r = request("load-all", &out, 1);
    if (r >= 0) { printf("%s", out); free(out); }
    return r < 0 ? 1 : r;
  }
  if (!strcmp(v, "daemon-reload") || !strcmp(v, "daemon-reexec") || !strcmp(v, "poweroff") || !strcmp(v, "reboot") || !strcmp(v, "halt") ||
      !strcmp(v, "soft-reboot")) {
    int r = request(v, 0, 1);
    return r < 0 ? 1 : r;
  }
  if (!strcmp(v, "reset-failed")) {
    char req[300];
    char name[256] = "";
    if (nu) norm(u[0], name, sizeof name);
    snprintf(req, sizeof req, "reset-failed%s%s", nu ? " " : "", name);
    int r = request(req, 0, 1);
    return r < 0 ? 1 : r;
  }
  if (!strcmp(v, "kill")) {
    int rc = 0;
    for (int i = 0; i < nu; i++) {
      char req[400], name[256];
      norm(u[i], name, sizeof name);
      int sig = atoi(signal_opt);
      if (!sig) {
        static const struct { const char *n; int s; } t[] = {{"HUP", 1}, {"INT", 2}, {"KILL", 9}, {"USR1", 10}, {"USR2", 12}, {"TERM", 15}, {0, 0}};
        const char *s = signal_opt + (strncmp(signal_opt, "SIG", 3) ? 0 : 3);
        for (int k = 0; t[k].n; k++) if (!strcmp(s, t[k].n)) sig = t[k].s;
      }
      snprintf(req, sizeof req, "kill %s --signal=%d --kill-whom=%s", name, sig ? sig : 15, whom);
      int r = request(req, 0, 1);
      if (r) rc = 1;
    }
    return rc;
  }
  if (!strcmp(v, "is-system-running")) {
    char *out;
    if (request("list-units", &out, 0) < 0) { if (!quiet) printf("offline\n"); return 1; }
    int failed = strstr(out, "\tfailed\t") != 0;
    free(out);
    char *st;
    request("status-manager", &st, 0);
    int jobsn = st && prop(st, "Jobs") ? atoi(prop(st, "Jobs")) : 0;
    free(st);
    const char *s = jobsn ? "starting" : failed ? "degraded" : "running";
    if (!quiet) printf("%s\n", s);
    return !strcmp(s, "running") ? 0 : 1;
  }
  fprintf(stderr, "Unknown command verb '%s'.\n", v);
  return 1;
}
