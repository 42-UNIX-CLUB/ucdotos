// SPDX-License-Identifier: GPL-2.0-or-later
/* date [-u] [-d @SECONDS] [+FORMAT]: times are UTC (there is no time zone database) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct civil { int year, mon, mday, hour, min, sec, wday, yday; };
static void to_civil(long long t, struct civil *c) {
  long long days = t / 86400, s = t % 86400;
  if (s < 0) { s += 86400; days--; }
  c->hour = s / 3600; c->min = s / 60 % 60; c->sec = s % 60;
  c->wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */
  long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  c->mday = doy - (153 * mp + 2) / 5 + 1;
  c->mon = mp < 10 ? mp + 3 : mp - 9;
  c->year = yoe + era * 400 + (c->mon <= 2);
  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int leap = (c->year % 4 == 0 && c->year % 100 != 0) || c->year % 400 == 0;
  c->yday = cum[c->mon - 1] + c->mday - 1 + (leap && c->mon > 2);
}
static const char *wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *mn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static void fmt(const char *f, long long t, const struct civil *c) {
  for (; *f; f++) {
    if (*f != '%' || !f[1]) { putchar(*f); continue; }
    switch (*++f) {
    case 'a': printf("%.3s", wd[c->wday]); break;
    case 'A': printf("%s", wd[c->wday]); break;
    case 'b': case 'h': printf("%.3s", mn[c->mon - 1]); break;
    case 'B': printf("%s", mn[c->mon - 1]); break;
    case 'c': fmt("%a %b %e %H:%M:%S %Y", t, c); break;
    case 'C': printf("%02d", c->year / 100); break;
    case 'd': printf("%02d", c->mday); break;
    case 'D': fmt("%m/%d/%y", t, c); break;
    case 'e': printf("%2d", c->mday); break;
    case 'F': fmt("%Y-%m-%d", t, c); break;
    case 'H': printf("%02d", c->hour); break;
    case 'I': printf("%02d", c->hour % 12 ? c->hour % 12 : 12); break;
    case 'j': printf("%03d", c->yday + 1); break;
    case 'k': printf("%2d", c->hour); break;
    case 'm': printf("%02d", c->mon); break;
    case 'M': printf("%02d", c->min); break;
    case 'n': putchar('\n'); break;
    case 'p': printf("%s", c->hour < 12 ? "AM" : "PM"); break;
    case 'R': fmt("%H:%M", t, c); break;
    case 's': printf("%lld", t); break;
    case 'S': printf("%02d", c->sec); break;
    case 't': putchar('\t'); break;
    case 'T': fmt("%H:%M:%S", t, c); break;
    case 'u': printf("%d", c->wday ? c->wday : 7); break;
    case 'w': printf("%d", c->wday); break;
    case 'y': printf("%02d", c->year % 100); break;
    case 'Y': printf("%d", c->year); break;
    case 'z': printf("+0000"); break;
    case 'Z': printf("UTC"); break;
    case '%': putchar('%'); break;
    default: putchar('%'); putchar(*f);
    }
  }
}
int main(int argc, char **argv) {
  long long t = time(0);
  const char *f = "%a %b %e %H:%M:%S %Z %Y";
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '+') f = argv[i] + 1;
    else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) {}
    else if (!strcmp(argv[i], "-R")) f = "%a, %d %b %Y %H:%M:%S %z";
    else if (!strcmp(argv[i], "-I")) f = "%Y-%m-%d";
    else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
      const char *d = argv[++i];
      if (*d != '@') { fprintf(stderr, "date: only -d @SECONDS is supported\n"); return 1; }
      t = strtoll(d + 1, 0, 10);
    } else { fprintf(stderr, "usage: date [-u] [-R] [-I] [-d @SECONDS] [+FORMAT]\n"); return 1; }
  }
  struct civil c;
  to_civil(t, &c);
  fmt(f, t, &c);
  putchar('\n');
  return 0;
}
