// SPDX-License-Identifier: GPL-2.0-or-later
/* string.h / strings.h functions that only user programs need (the kernel
 * shares lib/string.c). */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

int strcoll(const char *a, const char *b) { return strcmp(a, b); } /* "C" locale only */
size_t strxfrm(char *d, const char *s, size_t n) { return strlcpy(d, s, n); }
void *memccpy(void *d, const void *s, int c, size_t n) {
  unsigned char *dp = d;
  const unsigned char *sp = s;
  while (n--) if ((*dp++ = *sp++) == (unsigned char)c) return dp;
  return 0;
}
void *mempcpy(void *d, const void *s, size_t n) { return (char *)memcpy(d, s, n) + n; }
void *memrchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  while (n--) if (*--p == (unsigned char)c) return (void *)p;
  return 0;
}
void *memmem(const void *h, size_t hn, const void *n, size_t nn) {
  if (!nn) return (void *)h;
  for (const char *p = h; hn >= nn; p++, hn--) if (*p == *(const char *)n && !memcmp(p, n, nn)) return (void *)p;
  return 0;
}
char *stpcpy(char *d, const char *s) { while ((*d = *s++)) d++; return d; }
char *stpncpy(char *d, const char *s, size_t n) {
  size_t l = strnlen(s, n);
  memcpy(d, s, l);
  memset(d + l, 0, n - l);
  return d + l;
}
size_t strlcat(char *d, const char *s, size_t n) {
  size_t l = strnlen(d, n);
  return l == n ? n + strlen(s) : l + strlcpy(d + l, s, n - l);
}
char *strsep(char **sp, const char *delim) {
  char *s = *sp;
  if (!s) return 0;
  char *e = s + strcspn(s, delim);
  if (*e) *e++ = 0, *sp = e; else *sp = 0;
  return s;
}
char *strcasestr(const char *h, const char *n) {
  size_t l = strlen(n);
  for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
  return l ? 0 : (char *)h;
}
char *strchrnul(const char *s, int c) { while (*s && *s != (char)c) s++; return (char *)s; }
char *strsignal(int sig) {
  static const char *names[] = {0, "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap",
    "Aborted", "Bus error", "Floating point exception", "Killed", "User defined signal 1", "Segmentation fault",
    "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated", "Stack fault", "Child exited",
    "Continued", "Stopped (signal)", "Stopped", "Stopped (tty input)", "Stopped (tty output)",
    "Urgent I/O condition", "CPU time limit exceeded", "File size limit exceeded", "Virtual timer expired",
    "Profiling timer expired", "Window changed", "I/O possible", "Power failure", "Bad system call"};
  static char buf[32];
  if (sig > 0 && sig < (int)(sizeof names / sizeof *names)) return (char *)names[sig];
  snprintf(buf, sizeof buf, "Unknown signal %d", sig);
  return buf;
}
void bzero(void *p, size_t n) { memset(p, 0, n); }
void bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }
int ffs(int x) { return x ? __builtin_ctz(x) + 1 : 0; }
