// SPDX-License-Identifier: GPL-2.0-or-later
/* journalctl: read the manager's journal (journalctl(1), the subset in
 * docs/SERVICES.md). The journal lives in the manager's memory and holds only the
 * current boot: nothing persists across a reload of the page. */
#include "../../libsvc/os.h"
#include "../../libsvc/proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char units[8][160];
static int nunits, lines = -1, follow, quiet, maxprio = 7, out_cat, out_iso, out_mono, reverse, nshown;
static char ident[64], host[64] = "42lisboa";
static int64_t since_ms = -1;

static void norm(const char *in, char *out, size_t ol) {
  static const char *const sfx[] = {".service", ".socket", ".target", ".timer", ".path", ".slice", ".mount", ".device", ".scope", 0};
  for (int i = 0; sfx[i]; i++) { size_t l = strlen(in), s = strlen(sfx[i]); if (l > s && !strcmp(in + l - s, sfx[i])) { snprintf(out, ol, "%s", in); return; } }
  snprintf(out, ol, "%s.service", in);
}

static char **keep; /* the last `lines` matches when filtering here */
static int nkeep, kpos;
static void out(char *line);
static void show(char *line) {
  char *f[7];
  int n = 0;
  char *q = line;
  while (n < 6) { f[n++] = q; q = strchr(q, '\t'); if (!q) return; *q++ = 0; }
  f[6] = q;
  int prio = atoi(f[2]);
  if (prio > maxprio) return;
  if (ident[0] && strcmp(ident, f[5])) return;
  int64_t t = atoll(f[1]);
  if (since_ms >= 0 && t < since_ms) return;
  /* filters applied here: restore the tabs and keep only the last `lines` */
  for (int i = 0; i < 6; i++) f[i][strlen(f[i])] = '\t';
  if (keep && !follow) { free(keep[kpos % lines]); keep[kpos % lines] = strdup(line); kpos++; return; }
  out(line);
}
static void out(char *line) {
  char *f[7];
  int n = 0;
  char *q = line;
  while (n < 6) { f[n++] = q; q = strchr(q, '\t'); if (!q) return; *q++ = 0; }
  f[6] = q;
  int64_t t = atoll(f[1]);
  nshown++;
  if (out_cat) { printf("%s\n", f[6]); return; }
  time_t s = (time_t)(t / 1000);
  struct tm tm;
  gmtime_r(&s, &tm);
  char ts[40];
  if (out_iso) strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
  else strftime(ts, sizeof ts, "%b %d %H:%M:%S", &tm);
  printf("%s %s %s[%s]: %s\n", ts, host, f[5], f[3], f[6]);
  fflush(stdout);
}

int main(int argc, char **argv) {
  gethostname(host, sizeof host);
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : 0;
    if ((!strcmp(a, "-u") || !strcmp(a, "--unit")) && v) { if (nunits < 8) norm(v, units[nunits++], sizeof units[0]); i++; }
    else if (!strncmp(a, "--unit=", 7)) { if (nunits < 8) norm(a + 7, units[nunits++], sizeof units[0]); }
    else if (!strncmp(a, "-u", 2) && a[2]) { if (nunits < 8) norm(a + 2, units[nunits++], sizeof units[0]); }
    else if ((!strcmp(a, "-n") || !strcmp(a, "--lines")) && v && (v[0] >= '0' && v[0] <= '9')) { lines = atoi(v); i++; }
    else if (!strcmp(a, "-n") || !strcmp(a, "--lines")) lines = 10;
    else if (!strncmp(a, "--lines=", 8)) lines = atoi(a + 8);
    else if (!strncmp(a, "-n", 2) && a[2]) lines = atoi(a + 2);
    else if (!strcmp(a, "-f") || !strcmp(a, "--follow")) follow = 1;
    else if (!strcmp(a, "-e") || !strcmp(a, "--pager-end")) { if (lines < 0) lines = 1000; }
    else if (!strcmp(a, "-r") || !strcmp(a, "--reverse")) reverse = 1;
    else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "-x") || !strcmp(a, "--catalog") || !strcmp(a, "--no-pager") || !strcmp(a, "--system") || !strcmp(a, "-l") ||
             !strcmp(a, "--full") || !strcmp(a, "--no-hostname") || !strcmp(a, "-a") || !strcmp(a, "--all")) {}
    else if (!strcmp(a, "-b") || !strcmp(a, "--boot")) {
      /* only the current boot exists */
      if (v && (v[0] == '-' || (v[0] >= '0' && v[0] <= '9'))) {
        i++;
        if (strcmp(v, "0")) { fprintf(stderr, "Specifying boot ID or boot offset has no effect, no persistent journal was found.\n"); return 1; }
      }
    } else if (!strncmp(a, "--boot=", 7)) { if (strcmp(a + 7, "0")) { fprintf(stderr, "Specifying boot ID or boot offset has no effect, no persistent journal was found.\n"); return 1; } }
    else if (!strcmp(a, "--list-boots")) { printf("IDX BOOT ID                          FIRST ENTRY LAST ENTRY\n  0 current                          -           -\n"); return 0; }
    else if ((!strcmp(a, "-p") || !strcmp(a, "--priority")) && v) {
      static const char *const pn[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
      maxprio = (v[0] >= '0' && v[0] <= '7') ? v[0] - '0' : 7;
      for (int k = 0; k < 8; k++) if (!strncmp(v, pn[k], strlen(pn[k]))) maxprio = k;
      i++;
    } else if ((!strcmp(a, "-t") || !strcmp(a, "--identifier")) && v) { snprintf(ident, sizeof ident, "%s", v); i++; }
    else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && v) {
      out_cat = !strcmp(v, "cat"); out_iso = !strcmp(v, "short-iso"); out_mono = !strcmp(v, "short-monotonic");
      i++;
    } else if (!strncmp(a, "--output=", 9)) { out_cat = !strcmp(a + 9, "cat"); out_iso = !strcmp(a + 9, "short-iso"); }
    else if (!strcmp(a, "-k") || !strcmp(a, "--dmesg")) { printf("-- No entries --\n"); return 0; }
    else if (!strcmp(a, "--since") && v) { if (!strcmp(v, "today")) { int64_t n = svc_epoch_ms(); since_ms = n - n % 86400000; } i++; }
    else if (!strcmp(a, "--disk-usage")) {
      int fd = svc_connect_unix(getenv("SVCD_SOCKET") ? getenv("SVCD_SOCKET") : SVC_CONTROL);
      if (fd < 0) { fprintf(stderr, "No journal: the service manager is not running.\n"); return 1; }
      if (write(fd, "status-manager\n", 15) != 15) return 1;
      char b[2048];
      int n = 0;
      ssize_t r;
      while ((r = read(fd, b + n, sizeof b - 1 - n)) > 0) n += r;
      b[n] = 0;
      char *u = strstr(b, "JournalUsed="), *t = strstr(b, "JournalBytes=");
      printf("The journal is in memory and uses %d of its %d bytes.\n", u ? atoi(u + 12) : 0, t ? atoi(t + 13) : 0);
      return 0;
    }
    else if (!strncmp(a, "--vacuum", 8) || !strcmp(a, "--rotate") || !strcmp(a, "--flush") || !strcmp(a, "--sync")) return 0;
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
      printf("journalctl [-u UNIT] [-n N] [-f] [-b] [-e] [-p PRIO] [-t IDENT] [-o short|short-iso|cat] [--disk-usage]\n"
             "Shows the journal kept in the manager's memory (this boot only).\n");
      return 0;
    }
    else if (a[0] == '-') { fprintf(stderr, "journalctl: unrecognized option '%s'\n", a); return 1; }
    else if (!strncmp(a, "_SYSTEMD_UNIT=", 14)) { if (nunits < 8) norm(a + 14, units[nunits++], sizeof units[0]); }
  }
  if (follow && lines < 0) lines = 10;
  int fd = svc_connect_unix(getenv("SVCD_SOCKET") ? getenv("SVCD_SOCKET") : SVC_CONTROL);
  if (fd < 0) { fprintf(stderr, "No journal: the service manager is not running.\n"); return 1; }
  char req[1600];
  int l = snprintf(req, sizeof req, "journal");
  for (int i = 0; i < nunits; i++) l += snprintf(req + l, sizeof req - l, " -u %s", units[i]);
  int local = ident[0] || maxprio < 7 || since_ms >= 0; /* filters the manager does not know */
  if (lines >= 0 && !reverse && !local) l += snprintf(req + l, sizeof req - l, " -n %d", lines);
  if (local && lines > 0 && !follow) keep = calloc(lines, sizeof *keep);
  if (follow) l += snprintf(req + l, sizeof req - l, " -f");
  l += snprintf(req + l, sizeof req - l, "\n");
  if (write(fd, req, l) != l) return 1;
  /* read lines as they come; "=rc" ends a non-follow answer */
  char buf[8192];
  int n = 0;
  char **all = 0;
  int nall = 0;
  for (;;) {
    ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
    if (r <= 0) break;
    n += r;
    buf[n] = 0;
    char *s = buf, *e;
    while ((e = strchr(s, '\n'))) {
      *e = 0;
      if (!strncmp(s, "=rc ", 4)) { n = 0; goto done; }
      if (reverse) { all = realloc(all, (nall + 1) * sizeof *all); all[nall++] = strdup(s); }
      else show(s);
      s = e + 1;
    }
    n = buf + n - s;
    memmove(buf, s, n);
  }
done:
  close(fd);
  if (keep) {
    int start = kpos > lines ? kpos - lines : 0;
    for (int i = start; i < kpos; i++) out(keep[i % lines]);
  }
  if (reverse) for (int i = nall - 1, k = 0; i >= 0 && (lines < 0 || k < lines); i--, k++) show(all[i]);
  if (!nshown && !quiet && !follow) printf("-- No entries --\n");
  return 0;
}
