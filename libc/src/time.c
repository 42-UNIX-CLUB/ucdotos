// SPDX-License-Identifier: GPL-2.0-or-later
/* Calendar time. The system clock is UTC and there are no time zones, so
 * localtime is gmtime. */
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static int leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const short ydays[2][13] = {{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
                                   {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

/* days since 1970-01-01 of a civil date (proleptic Gregorian), month 1..12 */
static long long days_from_civil(long long y, int m, int d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
struct tm *gmtime_r(const time_t *t, struct tm *tm) {
  long long s = *t, days = s / 86400, rem = s % 86400;
  if (rem < 0) rem += 86400, days--;
  tm->tm_hour = rem / 3600; tm->tm_min = rem / 60 % 60; tm->tm_sec = rem % 60;
  tm->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
  long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  long long doe = z - era * 146097;
  long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long long mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y += m <= 2;
  tm->tm_year = (int)(y - 1900); tm->tm_mon = m - 1; tm->tm_mday = d;
  tm->tm_yday = ydays[leap(y)][m - 1] + d - 1;
  tm->tm_isdst = 0;
  return tm;
}
struct tm *localtime_r(const time_t *t, struct tm *tm) { return gmtime_r(t, tm); }
struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
struct tm *localtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
time_t timegm(struct tm *tm) {
  long long mon = tm->tm_mon, y = tm->tm_year + 1900LL + mon / 12;
  mon %= 12;
  if (mon < 0) mon += 12, y--;
  time_t t = days_from_civil(y, (int)mon + 1, 1) * 86400 + (tm->tm_mday - 1) * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
  gmtime_r(&t, tm); /* normalizes the fields, as mktime must */
  return t;
}
time_t mktime(struct tm *tm) { return timegm(tm); }
double difftime(time_t a, time_t b) { return (double)(a - b); }

static const char *const wday_name[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const mon_name[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
char *asctime_r(const struct tm *tm, char *buf) {
  snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", wday_name[tm->tm_wday % 7], mon_name[tm->tm_mon % 12],
           tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, 1900 + tm->tm_year);
  return buf;
}
char *asctime(const struct tm *tm) { static char buf[32]; return asctime_r(tm, buf); }
char *ctime(const time_t *t) { struct tm tm; return asctime(gmtime_r(t, &tm)); }

/* ISO 8601 week number and week-based year */
static int iso_week(const struct tm *tm, int *year) {
  int y = tm->tm_year + 1900, wd = (tm->tm_wday + 6) % 7; /* Monday = 0 */
  int w = (tm->tm_yday - wd + 10) / 7;
  if (w < 1) {
    y--;
    int pyd = tm->tm_yday + 365 + leap(y); /* yday counted from the previous year */
    w = (pyd - wd + 10) / 7;
  } else if (w == 53) {
    int jan1 = (wd - tm->tm_yday % 7 + 7 * 60) % 7; /* weekday of Jan 1, Monday = 0 */
    if (!(jan1 == 3 || (jan1 == 2 && leap(y)))) { w = 1; y++; }
  }
  *year = y;
  return w;
}
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
  size_t n = 0;
  char tmp[64];
  for (; *fmt; fmt++) {
    const char *add = tmp;
    if (*fmt != '%') { tmp[0] = *fmt; tmp[1] = 0; }
    else {
      fmt++;
      while (*fmt == 'E' || *fmt == 'O') fmt++;
      int hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, iy;
      switch (*fmt) {
      case 'a': snprintf(tmp, sizeof tmp, "%.3s", wday_name[tm->tm_wday % 7]); break;
      case 'A': add = wday_name[tm->tm_wday % 7]; break;
      case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", mon_name[tm->tm_mon % 12]); break;
      case 'B': add = mon_name[tm->tm_mon % 12]; break;
      case 'c': strftime(tmp, sizeof tmp, "%a %b %e %H:%M:%S %Y", tm); break;
      case 'C': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) / 100); break;
      case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
      case 'D': strftime(tmp, sizeof tmp, "%m/%d/%y", tm); break;
      case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
      case 'F': strftime(tmp, sizeof tmp, "%Y-%m-%d", tm); break;
      case 'g': iso_week(tm, &iy); snprintf(tmp, sizeof tmp, "%02d", iy % 100); break;
      case 'G': iso_week(tm, &iy); snprintf(tmp, sizeof tmp, "%d", iy); break;
      case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
      case 'I': snprintf(tmp, sizeof tmp, "%02d", hour12); break;
      case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
      case 'k': snprintf(tmp, sizeof tmp, "%2d", tm->tm_hour); break;
      case 'l': snprintf(tmp, sizeof tmp, "%2d", hour12); break;
      case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
      case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
      case 'n': add = "\n"; break;
      case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
      case 'P': add = tm->tm_hour < 12 ? "am" : "pm"; break;
      case 'r': strftime(tmp, sizeof tmp, "%I:%M:%S %p", tm); break;
      case 'R': strftime(tmp, sizeof tmp, "%H:%M", tm); break;
      case 's': { struct tm c = *tm; snprintf(tmp, sizeof tmp, "%lld", (long long)timegm(&c)); break; }
      case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
      case 't': add = "\t"; break;
      case 'T': strftime(tmp, sizeof tmp, "%H:%M:%S", tm); break;
      case 'u': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday ? tm->tm_wday : 7); break;
      case 'U': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
      case 'V': snprintf(tmp, sizeof tmp, "%02d", iso_week(tm, &iy)); break;
      case 'w': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday); break;
      case 'W': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
      case 'x': strftime(tmp, sizeof tmp, "%m/%d/%y", tm); break;
      case 'X': strftime(tmp, sizeof tmp, "%H:%M:%S", tm); break;
      case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); break;
      case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
      case 'z': add = "+0000"; break;
      case 'Z': add = "UTC"; break;
      case '%': add = "%"; break;
      case 0: fmt--; add = ""; break;
      default: snprintf(tmp, sizeof tmp, "%%%c", *fmt);
      }
    }
    size_t l = strlen(add);
    if (n + l >= max) { if (max) s[0] = 0; return 0; }
    memcpy(s + n, add, l);
    n += l;
  }
  if (max) s[n] = 0;
  return n;
}

int gettimeofday(struct timeval *tv, void *tz) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;
  if (tv) { tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; }
  if (tz) memset(tz, 0, 2 * sizeof(int));
  return 0;
}
