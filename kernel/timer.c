// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel timers. The kernel keeps a sorted list and asks the host for one timeout,
 * at the earliest deadline; the host calls timer_fire() when it is reached. Nothing
 * here runs inside a system call, so a timer callback can wake any process. */
#include "net.h"

HOST(timer) void host_timer(double ms); /* call timer_fire() in ms (0: next turn); <0 cancels */

static struct ktimer *timers;
static double armed_for = -1; /* the deadline the host currently holds, -1 none */
static int firing;

static void rearm(void) {
  if (firing) return;
  double due = timers ? timers->due : -1;
  if (due == armed_for) return;
  armed_for = due;
  if (due < 0) { host_timer(-1); return; }
  double ms = due - host_now();
  host_timer(ms > 0 ? ms : 0);
}
void timer_cancel(struct ktimer *t) {
  if (!t->armed) return;
  for (struct ktimer **pp = &timers; *pp; pp = &(*pp)->next)
    if (*pp == t) { *pp = t->next; break; }
  t->armed = 0;
  rearm();
}
void timer_set(struct ktimer *t, double ms, void (*fn)(void *), void *arg) {
  if (t->armed) {
    for (struct ktimer **pp = &timers; *pp; pp = &(*pp)->next)
      if (*pp == t) { *pp = t->next; break; }
  }
  t->due = host_now() + (ms > 0 ? ms : 0);
  t->fn = fn; t->arg = arg; t->armed = 1;
  struct ktimer **pp = &timers;
  while (*pp && (*pp)->due <= t->due) pp = &(*pp)->next;
  t->next = *pp;
  *pp = t;
  rearm();
}
EXPORT(timer_fire) void timer_fire(void) {
  armed_for = -1;
  firing = 1;
  /* Run what is due, including timers that callbacks set for "now". Bounded, so a
   * callback that keeps re-arming itself at 0 ms cannot hold the page. */
  for (int n = 0; timers && n < 4096; n++) {
    double now = host_now();
    struct ktimer *t = timers;
    if (t->due > now) break;
    timers = t->next;
    t->armed = 0;
    t->fn(t->arg);
  }
  firing = 0;
  rearm();
}
