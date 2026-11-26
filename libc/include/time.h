// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _TIME_H
#define _TIME_H
#include <stddef_extra.h>
#include <sys/syscall.h>
/* The clock is UTC; there are no time zones, so localtime == gmtime. */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000
typedef int clockid_t;
time_t time(time_t *);
clock_t clock(void);
int clock_gettime(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
struct tm *localtime_r(const time_t *, struct tm *);
time_t mktime(struct tm *);
time_t timegm(struct tm *);
double difftime(time_t, time_t);
char *asctime(const struct tm *);
char *asctime_r(const struct tm *, char *);
char *ctime(const time_t *);
size_t strftime(char *, size_t, const char *, const struct tm *);
#endif
