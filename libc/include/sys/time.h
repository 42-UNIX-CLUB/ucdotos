// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <stddef_extra.h>
#include <sys/select.h>
struct timezone { int tz_minuteswest, tz_dsttime; };
int gettimeofday(struct timeval *, void *);
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, op) ((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec op (b)->tv_usec : (a)->tv_sec op (b)->tv_sec)
#define timeradd(a, b, r) do { (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
  if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } } while (0)
#define timersub(a, b, r) do { (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
  if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } } while (0)
/* interval timers: only ITIMER_REAL (SIGALRM) */
struct itimerval { struct timeval it_interval, it_value; };
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
int getitimer(int, struct itimerval *);
int setitimer(int, const struct itimerval *, struct itimerval *);
#endif
