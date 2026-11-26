// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <strings.h>

/* Integer parsing core: magnitude in *v, returns the end pointer. *ovf is set
 * when the magnitude does not fit in 64 bits. */
static const char *parse_uint(const char *s0, int base, int *neg, int *ovf, unsigned long long *v) {
  const char *s = s0;
  while (isspace((unsigned char)*s)) s++;
  *neg = 0; *ovf = 0; *v = 0;
  if (*s == '+' || *s == '-') *neg = *s++ == '-';
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) { s += 2; base = 16; }
  else if (base == 0) base = s[0] == '0' ? 8 : 10;
  const char *digits = s;
  for (;; s++) {
    int d = isdigit((unsigned char)*s) ? *s - '0' : isalpha((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10 : 99;
    if (d >= base) break;
    if (*v > (~0ull - d) / base) *ovf = 1;
    *v = *v * base + d;
  }
  return s == digits ? s0 : s;
}
static long long to_signed(const char *s, char **end, int base, long long lo, long long hi) {
  int neg, ovf;
  unsigned long long v;
  const char *e = parse_uint(s, base, &neg, &ovf, &v);
  if (end) *end = (char *)e;
  if (!neg && (ovf || v > (unsigned long long)hi)) { errno = ERANGE; return hi; }
  if (neg && (ovf || v > (unsigned long long)hi + 1)) { errno = ERANGE; return lo; }
  return neg ? (long long)(0 - v) : (long long)v;
}
static unsigned long long to_unsigned(const char *s, char **end, int base, unsigned long long hi) {
  int neg, ovf;
  unsigned long long v;
  const char *e = parse_uint(s, base, &neg, &ovf, &v);
  if (end) *end = (char *)e;
  if (ovf || v > hi) { errno = ERANGE; return hi; }
  return neg ? (0 - v) & hi : v;
}
long long strtoll(const char *s, char **e, int b) { return to_signed(s, e, b, LLONG_MIN, LLONG_MAX); }
unsigned long long strtoull(const char *s, char **e, int b) { return to_unsigned(s, e, b, ULLONG_MAX); }
long strtol(const char *s, char **e, int b) { return (long)to_signed(s, e, b, LONG_MIN, LONG_MAX); }
unsigned long strtoul(const char *s, char **e, int b) { return (unsigned long)to_unsigned(s, e, b, ULONG_MAX); }
intmax_t strtoimax(const char *s, char **e, int b) { return strtoll(s, e, b); }
uintmax_t strtoumax(const char *s, char **e, int b) { return strtoull(s, e, b); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }

/* strtod: up to 19 significant digits are kept exactly in a u64; the power of ten
 * is applied in double-double, so the result is within an ulp (exact for the
 * common case of <= 15 digits and |exponent| <= 22). */
static void two_prod(double a, double b, double *p, double *e) {
  double c = 134217729.0 * a, ah = c - (c - a), al = a - ah;
  double d = 134217729.0 * b, bh = d - (d - b), bl = b - bh;
  *p = a * b;
  *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}
static void dd_mul(double *h, double *l, double b) { /* (h,l) *= b, b exact */
  double p, e;
  two_prod(*h, b, &p, &e);
  e += *l * b;
  *h = p + e; *l = e - (*h - p);
}
static void dd_div(double *h, double *l, double b) { /* (h,l) /= b, b exact */
  double q = *h / b, p, e;
  two_prod(q, b, &p, &e);
  double q2 = (((*h - p) - e) + *l) / b;
  *h = q + q2; *l = q2 - (*h - q);
}
static double pow2(double v, int e) { while (e > 0) v *= 2, e--; while (e < 0) v *= 0.5, e++; return v; }
double strtod(const char *s0, char **end) {
  const char *s = s0;
  while (isspace((unsigned char)*s)) s++;
  int neg = 0;
  if (*s == '+' || *s == '-') neg = *s++ == '-';
  double v = 0;
  if (!strncasecmp(s, "inf", 3)) { s += strncasecmp(s, "infinity", 8) ? 3 : 8; v = __builtin_inf(); goto done; }
  if (!strncasecmp(s, "nan", 3)) {
    s += 3;
    if (*s == '(') { const char *p = s + 1; while (isalnum((unsigned char)*p) || *p == '_') p++; if (*p == ')') s = p + 1; }
    v = __builtin_nan(""); goto done;
  }
  unsigned long long m = 0;
  int ex = 0, nd = 0, any = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && (isxdigit((unsigned char)s[2]) || (s[2] == '.' && isxdigit((unsigned char)s[3])))) {
    s += 2;
    for (int dot = 0;; s++) { /* hex float: mantissa bits, then a binary exponent */
      if (*s == '.' && !dot) { dot = 1; continue; }
      if (!isxdigit((unsigned char)*s)) break;
      int d = isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10;
      if (m >> 59) { ex += dot ? 0 : 4; m |= d != 0; } else { m = m * 16 + d; ex -= dot ? 4 : 0; }
    }
    if ((*s == 'p' || *s == 'P') && (isdigit((unsigned char)s[1]) || ((s[1] == '+' || s[1] == '-') && isdigit((unsigned char)s[2])))) {
      int en = 0, eneg = 0;
      s++;
      if (*s == '+' || *s == '-') eneg = *s++ == '-';
      while (isdigit((unsigned char)*s)) { if (en < 100000) en = en * 10 + (*s - '0'); s++; }
      ex += eneg ? -en : en;
    }
    v = pow2((double)m, ex);
    goto done;
  }
  for (; isdigit((unsigned char)*s); s++, any = 1) {
    if (nd < 19) { m = m * 10 + (*s - '0'); if (m) nd++; } else ex++;
  }
  if (*s == '.') {
    s++;
    for (; isdigit((unsigned char)*s); s++, any = 1)
      if (nd < 19) { m = m * 10 + (*s - '0'); ex--; if (m) nd++; }
  }
  if (!any) { s = s0; goto done; }
  if ((*s == 'e' || *s == 'E') && (isdigit((unsigned char)s[1]) || ((s[1] == '+' || s[1] == '-') && isdigit((unsigned char)s[2])))) {
    int en = 0, eneg = 0;
    s++;
    if (*s == '+' || *s == '-') eneg = *s++ == '-';
    while (isdigit((unsigned char)*s)) { if (en < 100000) en = en * 10 + (*s - '0'); s++; }
    ex += eneg ? -en : en;
  }
  if (!m) goto done;
  if (ex > 310) { v = __builtin_inf(); errno = ERANGE; goto done; }
  if (ex < -360) { errno = ERANGE; goto done; }
  if (m < (1ull << 53) && ex >= -22 && ex <= 22) { /* one correctly rounded operation */
    double p = 1;
    for (int i = 0; i < (ex < 0 ? -ex : ex); i++) p *= 10; /* exact up to 1e22 */
    v = ex < 0 ? (double)m / p : (double)m * p;
    goto done;
  }
  static const double P10[] = {1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double h = (double)m, l = (double)(long long)(m - (unsigned long long)h); /* m exactly, as h + l */
  double sc = ex > 0 ? 0x1p-128 : 0x1p128; /* keep Dekker's splitting away from overflow and underflow */
  h *= sc; l *= sc;
  for (; ex > 0; ex -= ex > 22 ? 22 : ex) dd_mul(&h, &l, P10[ex > 22 ? 22 : ex]);
  for (; ex < 0; ex += ex < -22 ? 22 : -ex) dd_div(&h, &l, P10[ex < -22 ? 22 : -ex]);
  v = (h + l) / sc;
  if (sc > 1 && h < 0x1p-894) { /* subnormal result: round h + l once, in units of 2^-1074 */
    double xh = h * 0x1p946, xl = l * 0x1p946, n = __builtin_floor(xh), d = xh - n;
    if (d == 0 && xl < 0) n -= 1, d = 1;
    double t = (d - 0.5) + xl; /* sign of (fraction - 1/2) */
    if (t > 0 || (t == 0 && ((long long)n & 1))) n += 1;
    v = n * 0x1p-1074;
  }
  if (v - v != 0 || v == 0 || v < 0x1p-1022) errno = ERANGE;
done:
  if (end) *end = (char *)s;
  return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, 0); }
float strtof(const char *s, char **e) { return (float)strtod(s, e); }
/* long double is IEEE binary128 on wasm32. Build it from the double's bits, so
 * no compiler-rt conversion helper is needed. */
long double __double_to_ld(double x) {
  union { double d; unsigned long long u; } b = {x};
  unsigned long long sign = b.u >> 63, m = b.u & 0xfffffffffffffull, hi = 0, lo = 0;
  int e = (int)((b.u >> 52) & 0x7ff);
  if (e == 0x7ff) e = 0x7fff;
  else if (e == 0) {
    if (m) { e = 1; while (!(m >> 52)) m <<= 1, e--; m &= 0xfffffffffffffull; e += 16383 - 1023; }
  } else e += 16383 - 1023;
  hi = sign << 63 | (unsigned long long)e << 48 | m >> 4;
  lo = m << 60;
  union { long double ld; unsigned long long w[2]; } r;
  r.w[0] = lo; r.w[1] = hi;
  return r.ld;
}
long double strtold(const char *s, char **e) { return __double_to_ld(strtod(s, e)); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
intmax_t imaxabs(intmax_t x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { return (div_t){a / b, a % b}; }
ldiv_t ldiv(long a, long b) { return (ldiv_t){a / b, a % b}; }
lldiv_t lldiv(long long a, long long b) { return (lldiv_t){a / b, a % b}; }
imaxdiv_t imaxdiv(intmax_t a, intmax_t b) { return (imaxdiv_t){a / b, a % b}; }
void _Exit(int code) { _exit(code); }
static unsigned long long rng = 0x2545F4914F6CDD1Dull;
void srand(unsigned s) { rng = s ? s : 1; }
int rand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (int)(rng >> 33); }

static void swap(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  char *b = base;
  while (n > 1) { /* quicksort, recursing into the smaller half */
    size_t i = 0, j = n - 1;
    swap(b, b + (n / 2) * sz, sz);
    for (;;) {
      do i++; while (i < n && cmp(b + i * sz, b) < 0);
      while (cmp(b + j * sz, b) > 0) j--;
      if (i >= j) break;
      swap(b + i * sz, b + j * sz, sz);
      j--;
    }
    swap(b, b + j * sz, sz);
    if (j < n - j - 1) { qsort(b, j, sz, cmp); b += (j + 1) * sz; n -= j + 1; }
    else { qsort(b + (j + 1) * sz, n - j - 1, sz, cmp); n = j; }
  }
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = cmp(key, (const char *)base + mid * sz);
    if (!c) return (char *)base + mid * sz;
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  return 0;
}
char *getenv(const char *name) {
  size_t n = strlen(name);
  for (char **e = environ; e && *e; e++) if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
  return 0;
}
int setenv(const char *name, const char *val, int overwrite) {
  static char **own;
  size_t n = strlen(name), count = 0;
  for (char **e = environ; e && *e; e++, count++)
    if (!strncmp(*e, name, n) && (*e)[n] == '=') {
      if (!overwrite) return 0;
      char *s = malloc(n + strlen(val) + 2);
      sprintf(s, "%s=%s", name, val);
      *e = s;
      return 0;
    }
  char **nv = malloc((count + 2) * sizeof *nv);
  memcpy(nv, environ, count * sizeof *nv);
  nv[count] = malloc(n + strlen(val) + 2);
  sprintf(nv[count], "%s=%s", name, val);
  nv[count + 1] = 0;
  if (own == environ) free(own);
  environ = own = nv;
  return 0;
}
int unsetenv(const char *name) {
  size_t n = strlen(name);
  for (char **e = environ; e && *e; e++)
    if (!strncmp(*e, name, n) && (*e)[n] == '=') { for (; *e; e++) e[0] = e[1]; break; }
  return 0;
}
/* system: no fork, so spawn /bin/sh -c cmd and wait for it */
int system(const char *cmd) {
  if (!cmd) return access("/bin/sh", X_OK) == 0;
  char *argv[] = {"sh", "-c", (char *)cmd, 0};
  pid_t pid = spawn("/bin/sh", argv, 0, 0);
  if (pid < 0) return -1;
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
  return st;
}
/* realpath: resolve . and .. textually against the cwd (this system has no
 * symlinks), then require that the result exists. */
char *realpath(const char *path, char *out) {
  char buf[PATH_MAX];
  size_t n = 0;
  if (!path) { errno = EINVAL; return 0; }
  if (!*path) { errno = ENOENT; return 0; }
  if (path[0] != '/') { if (!getcwd(buf, sizeof buf)) return 0; n = strlen(buf); if (n == 1) n = 0; }
  for (const char *p = path; *p;) {
    while (*p == '/') p++;
    const char *e = p;
    while (*e && *e != '/') e++;
    size_t len = e - p;
    if (!len || (len == 1 && p[0] == '.')) {}
    else if (len == 2 && p[0] == '.' && p[1] == '.') { while (n && buf[n - 1] != '/') n--; if (n) n--; }
    else {
      if (n + 1 + len >= sizeof buf) { errno = ENAMETOOLONG; return 0; }
      buf[n++] = '/'; memcpy(buf + n, p, len); n += len;
    }
    p = e;
  }
  if (!n) buf[n++] = '/';
  buf[n] = 0;
  struct stat st;
  if (stat(buf, &st) < 0) return 0;
  if (!out && !(out = malloc(n + 1))) return 0;
  return memcpy(out, buf, n + 1);
}

char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strndup(const char *s, size_t n) { size_t l = strnlen(s, n); char *p = malloc(l + 1); if (p) { memcpy(p, s, l); p[l] = 0; } return p; }
char *strncat(char *d, const char *s, size_t n) { char *p = d + strlen(d); while (n-- && *s) *p++ = *s++; *p = 0; return d; }
int strcasecmp(const char *a, const char *b) { while (*a && tolower(*a) == tolower(*b)) a++, b++; return tolower(*a) - tolower(*b); }
int strncasecmp(const char *a, const char *b, size_t n) { while (n && *a && tolower(*a) == tolower(*b)) a++, b++, n--; return n ? tolower(*a) - tolower(*b) : 0; }
size_t strspn(const char *s, const char *set) { size_t n = 0; while (s[n] && strchr(set, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *set) { size_t n = 0; while (s[n] && !strchr(set, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *set) { s += strcspn(s, set); return *s ? (char *)s : 0; }
char *strtok_r(char *s, const char *delim, char **save) {
  if (!s) s = *save;
  s += strspn(s, delim);
  if (!*s) { *save = s; return 0; }
  char *e = s + strcspn(s, delim);
  if (*e) *e++ = 0;
  *save = e;
  return s;
}
char *strtok(char *s, const char *delim) { static char *save; return strtok_r(s, delim, &save); }
char *strerror(int e) {
  switch (e) {
  case 0: return "Success";
  case EPERM: return "Operation not permitted";
  case ENOENT: return "No such file or directory";
  case ESRCH: return "No such process";
  case EINTR: return "Interrupted system call";
  case EIO: return "Input/output error";
  case ENOEXEC: return "Exec format error";
  case EBADF: return "Bad file descriptor";
  case ECHILD: return "No child processes";
  case EAGAIN: return "Resource temporarily unavailable";
  case ENOMEM: return "Cannot allocate memory";
  case EACCES: return "Permission denied";
  case EFAULT: return "Bad address";
  case EBUSY: return "Device or resource busy";
  case EEXIST: return "File exists";
  case ENOTDIR: return "Not a directory";
  case EISDIR: return "Is a directory";
  case EINVAL: return "Invalid argument";
  case ENFILE: return "Too many open files in system";
  case EMFILE: return "Too many open files";
  case ENOTTY: return "Inappropriate ioctl for device";
  case ENOSPC: return "No space left on device";
  case ESPIPE: return "Illegal seek";
  case EPIPE: return "Broken pipe";
  case ERANGE: return "Numerical result out of range";
  case ENAMETOOLONG: return "File name too long";
  case ENOSYS: return "Function not implemented";
  case ENOTEMPTY: return "Directory not empty";
  case ENOTSOCK: return "Socket operation on non-socket";
  case EADDRINUSE: return "Address already in use";
  case ECONNREFUSED: return "Connection refused";
  }
  static char buf[32];
  snprintf(buf, sizeof buf, "Unknown error %d", e);
  return buf;
}
