// SPDX-License-Identifier: GPL-2.0-or-later
/* Calendar events for OnCalendar= (systemd.time(7)), in UTC: UC.OS has no time zones.
 * Supported: the shorthands (minutely hourly daily weekly monthly yearly annually
 * quarterly semiannually), "[WEEKDAYS] [DATE] [TIME]" where each date and time field
 * is "*", a number, a list "a,b", a range "a..b" and a repetition "/step". Not
 * supported: "~" (days from the end of the month), fractional seconds (truncated),
 * time zone names other than UTC. */
#include "svc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void svc_civil(int64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd) {
  int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  int64_t rem = t - days * 86400;
  if (h) *h = rem / 3600;
  if (mi) *mi = rem / 60 % 60;
  if (s) *s = rem % 60;
  if (wd) *wd = (int)(((days % 7) + 7 + 3) % 7); /* 1970-01-01 was a Thursday (3 when Monday is 0) */
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int dd = (int)(doy - (153 * mp + 2) / 5 + 1);
  int mm = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (y) *y = (int)(yy + (mm <= 2));
  if (mo) *mo = mm;
  if (d) *d = dd;
}

/* Parse "*", "5", "1,3", "1..7", "*\/10", "0..30/5" into a bitmask over [lo, hi]. */
static int field(const char *s, size_t n, int lo, int hi, uint64_t *mask) {
  *mask = 0;
  const char *e = s + n;
  while (s < e) {
    const char *c = s;
    while (c < e && *c != ',') c++;
    int a = lo, b = hi, step = 1;
    const char *p = s;
    if (*p == '*') { p++; }
    else {
      if (!isdigit((unsigned char)*p)) return -1;
      a = 0;
      while (p < c && isdigit((unsigned char)*p)) a = a * 10 + (*p++ - '0');
      b = a;
      if (p + 1 < c && p[0] == '.' && p[1] == '.') {
        p += 2; b = 0;
        if (p >= c || !isdigit((unsigned char)*p)) return -1;
        while (p < c && isdigit((unsigned char)*p)) b = b * 10 + (*p++ - '0');
      }
    }
    if (p < c && *p == '.') { while (p < c && *p != '/') p++; } /* fractional seconds: ignored */
    if (p < c && *p == '/') {
      p++; step = 0;
      while (p < c && isdigit((unsigned char)*p)) step = step * 10 + (*p++ - '0');
      if (step <= 0) return -1;
      if (s[0] != '*' && b == a) b = hi; /* "5/10" means 5, 15, 25... */
    }
    if (p != c || a < lo || b > hi || a > b) return -1;
    for (int v = a; v <= b; v += step) *mask |= 1ULL << v;
    s = c < e ? c + 1 : e;
  }
  return *mask ? 0 : -1;
}

static int wdays(const char *s, size_t n, uint8_t *m) {
  static const char *const names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  *m = 0;
  const char *e = s + n;
  while (s < e) {
    const char *c = s;
    while (c < e && *c != ',') c++;
    int a = -1, b = -1;
    const char *dots = 0;
    for (const char *q = s; q + 1 < c; q++) if (q[0] == '.' && q[1] == '.') { dots = q; break; }
    const char *ae = dots ? dots : c;
    for (int i = 0; i < 7; i++) if (ae - s >= 3 && !strncasecmp(s, names[i], 3)) a = i;
    b = a;
    if (dots) { const char *bs = dots + 2; b = -1; for (int i = 0; i < 7; i++) if (c - bs >= 3 && !strncasecmp(bs, names[i], 3)) b = i; }
    if (a < 0 || b < 0) return -1;
    for (int v = a;; v = (v + 1) % 7) { *m |= 1 << v; if (v == b) break; }
    s = c < e ? c + 1 : e;
  }
  return 0;
}

int svc_cal_parse(const char *in, struct svc_cal *c) {
  static const struct { const char *n, *v; } sh[] = {
    {"minutely", "*-*-* *:*:00"}, {"hourly", "*-*-* *:00:00"}, {"daily", "*-*-* 00:00:00"},
    {"weekly", "Mon *-*-* 00:00:00"}, {"monthly", "*-*-01 00:00:00"}, {"yearly", "*-01-01 00:00:00"},
    {"annually", "*-01-01 00:00:00"}, {"quarterly", "*-01,04,07,10-01 00:00:00"},
    {"semiannually", "*-01,07-01 00:00:00"}, {0, 0}};
  char buf[256];
  while (isspace((unsigned char)*in)) in++;
  strncpy(buf, in, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  size_t bl = strlen(buf);
  while (bl && isspace((unsigned char)buf[bl - 1])) buf[--bl] = 0;
  if (bl > 4 && !strcasecmp(buf + bl - 4, " UTC")) buf[bl -= 4] = 0;
  for (int i = 0; sh[i].n; i++) if (!strcasecmp(buf, sh[i].n)) { strcpy(buf, sh[i].v); break; }
  memset(c, 0, sizeof *c);
  c->wday = 0x7f;
  uint64_t m;
  const char *tok[3];
  size_t tl[3];
  int nt = 0;
  for (char *p = buf; *p && nt < 3;) {
    while (*p == ' ') p++;
    if (!*p) break;
    tok[nt] = p;
    while (*p && *p != ' ') p++;
    tl[nt] = p - tok[nt];
    nt++;
  }
  int i = 0, have_date = 0, have_time = 0;
  if (i < nt && isalpha((unsigned char)tok[i][0])) { if (wdays(tok[i], tl[i], &c->wday)) return -1; i++; }
  c->mon = 0x1ffe; c->mday = 0xfffffffe; c->hour = 1; c->min = 1; c->sec = 1;
  for (; i < nt; i++) {
    const char *t = tok[i];
    size_t n = tl[i];
    if (memchr(t, ':', n)) {
      if (have_time) return -1;
      have_time = 1;
      const char *a = t, *e = t + n, *c1 = memchr(t, ':', n);
      const char *c2 = memchr(c1 + 1, ':', e - c1 - 1);
      if (field(a, c1 - a, 0, 23, &m)) return -1;
      c->hour = (uint32_t)m;
      if (field(c1 + 1, (c2 ? c2 : e) - c1 - 1, 0, 59, &m)) return -1;
      c->min = m;
      if (c2) { if (field(c2 + 1, e - c2 - 1, 0, 59, &m)) return -1; c->sec = m; }
      else c->sec = 1;
    } else if (memchr(t, '-', n)) {
      if (have_date) return -1;
      have_date = 1;
      const char *e = t + n, *d1 = memchr(t, '-', n);
      const char *d2 = memchr(d1 + 1, '-', e - d1 - 1);
      const char *ys = 0, *ms, *ds;
      size_t yl = 0, ml, dl;
      if (d2) { ys = t; yl = d1 - t; ms = d1 + 1; ml = d2 - d1 - 1; ds = d2 + 1; dl = e - d2 - 1; }
      else { ms = t; ml = d1 - t; ds = d1 + 1; dl = e - d1 - 1; }
      if (ys && !(yl == 1 && ys[0] == '*')) {
        /* years: one list of values or ranges */
        const char *p = ys, *pe = ys + yl;
        while (p < pe && c->nyear < 4) {
          const char *q = p;
          while (q < pe && *q != ',') q++;
          int a = atoi(p), b = a, st = 1;
          const char *dd = strstr(p, "..");
          if (dd && dd < q) b = atoi(dd + 2);
          const char *sl = memchr(p, '/', q - p);
          if (sl) st = atoi(sl + 1);
          if (a < 1970 || b < a || st < 1) return -1;
          c->year[c->nyear].a = a; c->year[c->nyear].b = b; c->year[c->nyear].step = st; c->nyear++;
          p = q < pe ? q + 1 : pe;
        }
      }
      if (field(ms, ml, 1, 12, &m)) return -1;
      c->mon = (uint16_t)m;
      if (field(ds, dl, 1, 31, &m)) return -1;
      c->mday = (uint32_t)m;
    } else return -1;
  }
  return 0;
}

static int year_ok(const struct svc_cal *c, int y) {
  if (!c->nyear) return 1;
  for (int i = 0; i < c->nyear; i++)
    if (y >= c->year[i].a && y <= c->year[i].b && (y - c->year[i].a) % c->year[i].step == 0) return 1;
  return 0;
}

int64_t svc_cal_next(const struct svc_cal *c, int64_t after) {
  int64_t t = after + 1;
  int64_t day = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  int64_t secofday = t - day * 86400;
  for (int k = 0; k < 366 * 30; k++, day++, secofday = 0) {
    int y, mo, d, wd;
    svc_civil(day * 86400, &y, &mo, &d, 0, 0, 0, &wd);
    if (!year_ok(c, y) || !(c->mon >> mo & 1) || !(c->mday >> d & 1) || !(c->wday >> wd & 1)) continue;
    for (int h = (int)(secofday / 3600); h < 24; h++) {
      if (!(c->hour >> h & 1)) continue;
      int m0 = h == secofday / 3600 ? (int)(secofday / 60 % 60) : 0;
      for (int mi = m0; mi < 60; mi++) {
        if (!(c->min >> mi & 1)) continue;
        int s0 = (h == secofday / 3600 && mi == secofday / 60 % 60) ? (int)(secofday % 60) : 0;
        for (int s = s0; s < 60; s++)
          if (c->sec >> s & 1) return day * 86400 + h * 3600 + mi * 60 + s;
      }
    }
  }
  return -1;
}
