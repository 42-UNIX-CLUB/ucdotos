// SPDX-License-Identifier: GPL-2.0-or-later
/* libc self-test: checks the C library against known values. Prints ok/FAIL per
 * check and a final count; exit status 1 if anything failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <libgen.h>
#include <locale.h>
#include <alloca.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fails, checks;
static void check(const char *name, int ok) {
  checks++;
  if (ok) printf("  ok    %s\n", name);
  else { fails++; printf("  FAIL  %s\n", name); }
}
#define CHECK(name, cond) check(name, (cond) != 0)
/* relative (or absolute near zero) closeness */
static int near(double a, double b, double tol) {
  if (isnan(b)) return isnan(a);
  if (isinf(b)) return a == b;
  double d = fabs(a - b), m = fabs(b);
  return d <= tol * (m > 1 ? m : 1);
}
#define NEAR(expr, want) do { double _v = (expr); char _n[160]; snprintf(_n, sizeof _n, "%s = %.17g", #expr, _v); check(_n, near(_v, want, 1e-12)); } while (0)
#define NEARF(expr, want) do { double _v = (expr); char _n[160]; snprintf(_n, sizeof _n, "%s = %.9g", #expr, _v); check(_n, near(_v, want, 1e-6)); } while (0)
static char sbuf[512];
#define FMT(want, ...) do { snprintf(sbuf, sizeof sbuf, __VA_ARGS__); char _n[640]; snprintf(_n, sizeof _n, "printf(%s) -> \"%s\"", #__VA_ARGS__, sbuf); check(_n, !strcmp(sbuf, want)); } while (0)

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
struct rec { int key; char pad[13]; };
static int cmp_rec(const void *a, const void *b) { return cmp_int(&((const struct rec *)a)->key, &((const struct rec *)b)->key); }

static void test_math(void) {
  printf("math\n");
  NEAR(sin(0.5), 0.479425538604203);
  NEAR(sin(M_PI / 6), 0.5);
  NEAR(sin(-3.0), -0.1411200080598672);
  NEAR(sin(1e6), -0.34999350217129294);
  NEAR(sin(1e22), -0.8522008497671888);
  NEAR(cos(0.5), 0.8775825618903728);
  NEAR(cos(M_PI), -1.0);
  NEAR(cos(10.0), -0.8390715290764524);
  NEAR(tan(1.0), 1.5574077246549023);
  NEAR(tan(-0.3), -0.30933624960962325);
  NEAR(asin(0.5), 0.5235987755982989);
  NEAR(asin(-0.9), -1.1197695149986342);
  NEAR(acos(0.5), 1.0471975511965979);
  NEAR(acos(-0.9), 2.6905658417935308);
  NEAR(acos(1.0), 0.0);
  NEAR(atan(1.0), M_PI_4);
  NEAR(atan(-7.5), -1.4382447944982226);
  NEAR(atan2(1.0, -1.0), 3 * M_PI_4);
  NEAR(atan2(-2.0, -3.0), -2.5535900500422257);
  NEAR(atan2(0.0, -1.0), M_PI);
  NEAR(sinh(1.0), 1.1752011936438014);
  NEAR(cosh(-2.0), 3.7621956910836314);
  NEAR(tanh(0.5), 0.46211715726000974);
  NEAR(tanh(30.0), 1.0);
  NEAR(asinh(2.0), 1.4436354751788103);
  NEAR(acosh(2.0), 1.3169578969248166);
  NEAR(atanh(0.5), 0.5493061443340549);
  NEAR(exp(1.0), M_E);
  NEAR(exp(-10.0), 4.5399929762484854e-05);
  NEAR(exp(100.0), 2.6881171418161356e+43);
  NEAR(exp2(10.5), 1448.1546878700492);
  NEAR(expm1(1e-10), 1.00000000005e-10);
  NEAR(log(M_E), 1.0);
  NEAR(log(10.0), M_LN10);
  NEAR(log(1e-300), -690.7755278982137);
  NEAR(log2(1024.0), 10.0);
  NEAR(log2(3.0), 1.584962500721156);
  NEAR(log10(1000.0), 3.0);
  NEAR(log10(2.0), 0.3010299956639812);
  NEAR(log1p(1e-10), 9.9999999995e-11);
  NEAR(pow(2.0, 10.0), 1024.0);
  NEAR(pow(2.0, 0.5), M_SQRT2);
  NEAR(pow(10.0, -2.0), 0.01);
  NEAR(pow(-2.0, 3.0), -8.0);
  NEAR(pow(1.5, 100.0), 4.065611775352152e+17);
  NEAR(pow(0.0, 0.0), 1.0);
  NEAR(sqrt(2.0), M_SQRT2);
  NEAR(cbrt(27.0), 3.0);
  NEAR(cbrt(-2.0), -1.2599210498948732);
  NEAR(hypot(3.0, 4.0), 5.0);
  NEAR(hypot(1e300, 1e300), 1.4142135623730951e300);
  NEAR(fmod(10.0, 3.0), 1.0);
  NEAR(fmod(-7.5, 2.0), -1.5);
  NEAR(fmod(1e20, 3.0), 1.0);
  NEAR(ldexp(0.75, 4), 12.0);
  NEAR(floor(-2.5), -3.0);
  NEAR(ceil(-2.5), -2.0);
  NEAR(round(2.5), 3.0);
  NEAR(round(-2.5), -3.0);
  NEAR(round(0.49999999999999994), 0.0);
  NEAR(trunc(-2.7), -2.0);
  NEAR(fabs(-3.25), 3.25);
  NEAR(copysign(2.0, -0.0), -2.0);
  NEAR(fmin(1.0, NAN), 1.0);
  NEAR(fmax(-1.0, 2.0), 2.0);
  int e;
  double m = frexp(48.0, &e);
  CHECK("frexp(48) = 0.75 * 2^6", m == 0.75 && e == 6);
  m = frexp(5e-320, &e);
  CHECK("frexp(subnormal)", m >= 0.5 && m < 1 && ldexp(m, e) == 5e-320);
  double ip;
  m = modf(-3.75, &ip);
  CHECK("modf(-3.75) = -3 + -0.75", ip == -3.0 && m == -0.75);
  CHECK("pow(3,20) exact", pow(3.0, 20.0) == 3486784401.0);
  CHECK("pow(10,15) exact", pow(10.0, 15.0) == 1e15);
  CHECK("pow(-8, 1/3) is nan", isnan(pow(-8.0, 1.0 / 3)));
  CHECK("pow(0, -1) = inf", isinf(pow(0.0, -1.0)) && pow(0.0, -1.0) > 0);
  CHECK("log(0) = -inf", isinf(log(0.0)) && log(0.0) < 0);
  CHECK("log(-1) is nan", isnan(log(-1.0)));
  CHECK("sqrt(-1) is nan", isnan(sqrt(-1.0)));
  CHECK("exp(1000) = inf", isinf(exp(1000.0)));
  CHECK("exp(-1000) = 0", exp(-1000.0) == 0);
  CHECK("isnan/isinf/isfinite", isnan(NAN) && isinf(INFINITY) && !isfinite(HUGE_VAL) && isfinite(1.0));
  CHECK("signbit(-0.0)", signbit(-0.0) && !signbit(0.0));
  CHECK("fpclassify", fpclassify(0.0) == FP_ZERO && fpclassify(1.0) == FP_NORMAL && fpclassify(5e-320) == FP_SUBNORMAL);
  CHECK("sin/cos(inf) is nan", isnan(sin(INFINITY)) && isnan(cos(-INFINITY)));
  double worst = 0;
  for (double x = -20; x < 20; x += 0.01) { double s = sin(x), c = cos(x), d = fabs(s * s + c * c - 1); if (d > worst) worst = d; }
  CHECK("sin^2 + cos^2 = 1 over [-20, 20]", worst < 4e-16);
  worst = 0;
  for (double x = 0.01; x < 100; x *= 1.01) { double d = fabs(exp(log(x)) / x - 1); if (d > worst) worst = d; }
  CHECK("exp(log(x)) = x over [0.01, 100]", worst < 1e-15);
  NEARF(sinf(0.5f), 0.47942554f);
  NEARF(cosf(2.0f), -0.41614684f);
  NEARF(tanf(0.7f), 0.84228838f);
  NEARF(atan2f(1.0f, 2.0f), 0.46364761f);
  NEARF(expf(2.0f), 7.3890561f);
  NEARF(logf(2.0f), 0.69314718f);
  NEARF(powf(2.0f, 0.25f), 1.1892071f);
  NEARF(sqrtf(9.0f), 3.0f);
  NEARF(floorf(-1.5f), -2.0f);
  NEARF(fmodf(5.5f, 2.0f), 1.5f);
  NEARF(hypotf(5.0f, 12.0f), 13.0f);
  NEARF(roundf(1.5f), 2.0f);
  NEARF(fabsf(-4.0f), 4.0f);
  NEARF(cbrtf(8.0f), 2.0f);
}

static void test_printf(void) {
  printf("printf\n");
  FMT("42|  42|42  |0042|+42| 42", "%d|%4d|%-4d|%04d|%+d|% d", 42, 42, 42, 42, 42, 42);
  FMT("-0042|  -42", "%05d|%5d", -42, -42);
  FMT("-9223372036854775808 18446744073709551615", "%lld %llu", LLONG_MIN, ULLONG_MAX);
  FMT("4294967295 -2147483648", "%lu %ld", ULONG_MAX, LONG_MIN);
  FMT("123456 4294967295", "%zu %zu", (size_t)123456, (size_t)-1);
  FMT("1234567890123 -7", "%jd %jd", (intmax_t)1234567890123LL, (intmax_t)-7);
  FMT("ff FF 0xff 377 0377", "%x %X %#x %o %#o", 255, 255, 255, 255, 255);
  FMT("-56 65535", "%hhd %hu", 200, 65535);
  FMT("00042|   42", "%.5d|%5.2d", 42, 42);
  FMT("[hello][   hi][hi   ][he]", "[%s][%5s][%-5s][%.2s]", "hello", "hi", "hi", "hello");
  FMT("x|    y|%", "%c|%5c|%%", 'x', 'y');
  FMT("(null)", "%s", (char *)0);
  FMT("3.141593 3.14 3 3.1416", "%f %.2f %.0f %.4f", M_PI, M_PI, M_PI, M_PI);
  FMT("  3.14|3.14  |003.14|+3.14", "%6.2f|%-6.2f|%06.2f|%+.2f", 3.14159, 3.14159, 3.14159, 3.14159);
  FMT("-0.500000 0.000001 1000000.000000", "%f %f %f", -0.5, 1e-6, 1e6);
  FMT("0.125 0.12 2 0", "%.3f %.2f %.0f %.0f", 0.125, 0.125, 2.5, 0.5);
  FMT("1e+10 1e-05", "%g %g", 1e10, 1e-5);
  FMT("1.234568e+05 1.23E-07 1e+100", "%e %.2E %.0e", 123456.789, 1.23456e-7, 1e100);
  FMT("-1.500000e+00 0.000000e+00", "%e %e", -1.5, 0.0);
  FMT("100000 1e+06 0.0001 1.5 3.14159", "%g %g %g %g %g", 100000.0, 1000000.0, 0.0001, 1.5, M_PI);
  FMT("0.1 0.30000000000000004 1e+22", "%g %.17g %g", 0.1, 0.1 + 0.2, 1e22);
  FMT("123456789 1.2346e+08", "%.9g %.5g", 123456789.0, 123456789.0);
  FMT("2.00000 1.e+06", "%#g %#.0e", 2.0, 1e6);
  FMT("inf -inf nan INF", "%f %f %f %F", INFINITY, -INFINITY, NAN, INFINITY);
  FMT("  inf|-inf ", "%5.1f|%-5g", INFINITY, -INFINITY);
  FMT("1.7976931348623157e+308 4.9406564584124654e-324", "%.17g %.17g", 1.7976931348623157e308, 5e-324);
  FMT("100000000000000000000.000000", "%f", 1e20);
  FMT("9007199254740994.000000 0.333333333333333314829616256247", "%f %.30f", 9007199254740992.0 + 2, 1.0 / 3);
  FMT("      1.50", "%*.*f", 10, 2, 1.5);
  FMT("0x1234", "%p", (void *)0x1234);
  FMT("-12.3%", "%.1f%%", -12.34);
  char small[8];
  int n = snprintf(small, sizeof small, "%s", "truncated string");
  CHECK("snprintf truncates and returns full length", n == 16 && !strcmp(small, "truncat"));
  char *as = 0;
  CHECK("asprintf", asprintf(&as, "%d-%s", 7, "x") == 3 && !strcmp(as, "7-x"));
  free(as);
}

static void test_scanf(void) {
  printf("scanf\n");
  int a = 0, b = 0, c = 0; unsigned u = 0; char s[32] = "", t[32] = ""; char ch = 0; float f = 0; double d = 0;
  CHECK("sscanf %d %d", sscanf("12 -34", "%d %d", &a, &b) == 2 && a == 12 && b == -34);
  CHECK("sscanf %u %x %o", sscanf("4000000000 ff 17", "%u %x %o", &u, &a, &b) == 3 && u == 4000000000u && a == 255 && b == 15);
  CHECK("sscanf %x with 0x", sscanf("0x1F", "%x", &a) == 1 && a == 31);
  CHECK("sscanf %i bases", sscanf("0x10 010 10", "%i %i %i", &a, &b, &c) == 3 && a == 16 && b == 8 && c == 10);
  CHECK("sscanf %s %c", sscanf("  hello world", "%s %c", s, &ch) == 2 && !strcmp(s, "hello") && ch == 'w');
  CHECK("sscanf %c keeps spaces", sscanf(" x", "%c", &ch) == 1 && ch == ' ');
  CHECK("sscanf %f %lf", sscanf("3.5 -2e3", "%f %lf", &f, &d) == 2 && f == 3.5f && d == -2000.0);
  CHECK("sscanf %lf nan/inf", sscanf("inf -nan", "%lf %f", &d, &f) == 2 && isinf(d) && isnan(f));
  CHECK("sscanf %[a-z]", sscanf("abc123", "%[a-z]%d", s, &a) == 2 && !strcmp(s, "abc") && a == 123);
  CHECK("sscanf %[^,]", sscanf("key,value", "%[^,],%s", s, t) == 2 && !strcmp(s, "key") && !strcmp(t, "value"));
  CHECK("sscanf %[]x]", sscanf("]]x-", "%[]x]", s) == 1 && !strcmp(s, "]]x"));
  CHECK("sscanf widths", sscanf("123456abcdef", "%3d%2d%3s%2c", &a, &b, s, t) == 4 && a == 123 && b == 45 && !strcmp(s, "6ab") && t[0] == 'c' && t[1] == 'd');
  CHECK("sscanf %*d suppress", sscanf("1 2 3", "%*d %d %*d", &a) == 1 && a == 2);
  CHECK("sscanf literal match", sscanf("10:20:30", "%d:%d:%d", &a, &b, &c) == 3 && c == 30);
  CHECK("sscanf literal mismatch", sscanf("10-20", "%d:%d", &a, &b) == 1);
  CHECK("sscanf %n", sscanf("abc def", "%s%n", s, &a) == 1 && a == 3);
  CHECK("sscanf %%", sscanf("50%", "%d%%", &a) == 1 && a == 50);
  CHECK("sscanf empty -> EOF", sscanf("", "%d", &a) == EOF && sscanf("   ", "%s", s) == EOF);
  CHECK("sscanf no match -> 0", sscanf("abc", "%d", &a) == 0);
  short hs; signed char hhs; long long ll; long l;
  CHECK("sscanf %hd %hhd %lld %ld", sscanf("-300 -5 -9000000000 77", "%hd %hhd %lld %ld", &hs, &hhs, &ll, &l) == 4 && hs == -300 && hhs == -5 && ll == -9000000000LL && l == 77);
  uint64_t v64;
  CHECK("sscanf SCNu64", sscanf("18446744073709551615", "%" SCNu64, &v64) == 1 && v64 == UINT64_MAX);
  CHECK("sscanf %f width", sscanf("1.2345", "%4f", &f) == 1 && f == 1.23f);
  void *p;
  CHECK("sscanf %p", sscanf("0x1234", "%p", &p) == 1 && p == (void *)0x1234);
  /* scanf family on a FILE */
  FILE *fp = fopen("/tmp/libctest.scan", "w+");
  CHECK("fopen w+", fp != 0);
  if (fp) {
    fprintf(fp, "7 words 2.5\nline two\n");
    rewind(fp);
    CHECK("fscanf", fscanf(fp, "%d %s %lf", &a, s, &d) == 3 && a == 7 && !strcmp(s, "words") && d == 2.5);
    char *line = 0; size_t cap = 0;
    CHECK("getline after fscanf", getline(&line, &cap, fp) == 1 && !strcmp(line, "\n"));
    CHECK("getline", getline(&line, &cap, fp) == 9 && !strcmp(line, "line two\n"));
    CHECK("getline at EOF", getline(&line, &cap, fp) == -1);
    rewind(fp);
    CHECK("getdelim", getdelim(&line, &cap, ' ', fp) == 2 && !strcmp(line, "7 "));
    free(line);
    fclose(fp);
    unlink("/tmp/libctest.scan");
  }
}

static void test_strtod(void) {
  printf("strtod / strtol\n");
  char *end;
  CHECK("strtod 1e10", strtod("1e10", &end) == 1e10 && !*end);
  CHECK("strtod -0.5", strtod("-0.5", 0) == -0.5);
  CHECK("strtod .25", strtod(".25", &end) == 0.25 && !*end);
  CHECK("strtod 0.1 exact", strtod("0.1", 0) == 0.1);
  CHECK("strtod 123.456e-2", strtod("123.456e-2", 0) == 1.23456);
  CHECK("strtod 1.7976931348623157e308", strtod("1.7976931348623157e308", 0) == 1.7976931348623157e308);
  CHECK("strtod 2.2250738585072014e-308", strtod("2.2250738585072014e-308", 0) == 2.2250738585072014e-308);
  CHECK("strtod 3.141592653589793238462643", strtod("3.141592653589793238462643", 0) == M_PI);
  CHECK("strtod 1e400 = inf, ERANGE", (errno = 0, isinf(strtod("1e400", 0))) && errno == ERANGE);
  CHECK("strtod inf / -Infinity / nan", isinf(strtod("inf", 0)) && strtod("-Infinity", 0) == -INFINITY && isnan(strtod("nan", 0)));
  CHECK("strtod 0x1.8p3", strtod("0x1.8p3", 0) == 12.0);
  CHECK("strtod stops at junk", strtod("12.5xyz", &end) == 12.5 && !strcmp(end, "xyz"));
  CHECK("strtod 1e (no exponent digits)", strtod("1e", &end) == 1 && !strcmp(end, "e"));
  const char *junk = "abc";
  CHECK("strtod no digits -> endptr = start", strtod(junk, &end) == 0 && end == junk);
  CHECK("strtof", strtof("0.1", 0) == 0.1f);
  long double ld = strtold("2.5", 0);
  union { long double l; uint64_t w[2]; } lu = {ld};
  CHECK("strtold builds binary128 2.5", lu.w[1] == 0x4000400000000000ull && lu.w[0] == 0);
  CHECK("atof", atof("  -1.5e2") == -150.0);
  CHECK("strtol base 16/8/0", strtol("ff", 0, 16) == 255 && strtol("017", 0, 0) == 15 && strtol("0x1A", 0, 0) == 26);
  CHECK("strtol overflow", (errno = 0, strtol("99999999999", 0, 10) == LONG_MAX) && errno == ERANGE);
  CHECK("strtol underflow", strtol("-99999999999", 0, 10) == LONG_MIN);
  CHECK("strtoul -1", strtoul("-1", 0, 10) == ULONG_MAX);
  CHECK("strtoll / strtoull limits", strtoll("-9223372036854775808", 0, 10) == LLONG_MIN && strtoull("18446744073709551615", 0, 10) == ULLONG_MAX);
  CHECK("strtol endptr", strtol("  42abc", &end, 10) == 42 && !strcmp(end, "abc"));
  CHECK("strtol no digits", strtol("xyz", &end, 10) == 0 && !strcmp(end, "xyz"));
  CHECK("atoll", atoll("-123456789012") == -123456789012LL);
  CHECK("strtoimax / strtoumax", strtoimax("-77", 0, 10) == -77 && strtoumax("77", 0, 8) == 63);
  CHECK("llabs / imaxabs", llabs(-5000000000LL) == 5000000000LL && imaxabs(-3) == 3);
  div_t q = div(-7, 2);
  ldiv_t lq = ldiv(17, 5);
  lldiv_t llq = lldiv(-17, 5);
  CHECK("div / ldiv / lldiv", q.quot == -3 && q.rem == -1 && lq.quot == 3 && lq.rem == 2 && llq.quot == -3 && llq.rem == -2);
}

static void test_string(void) {
  printf("string\n");
  char buf[64];
  CHECK("stpcpy", stpcpy(buf, "abc") == buf + 3 && !strcmp(buf, "abc"));
  CHECK("mempcpy", (char *)mempcpy(buf, "xyz", 3) == buf + 3 && !memcmp(buf, "xyz", 3));
  memset(buf, 0, sizeof buf);
  CHECK("memccpy stops after c", (char *)memccpy(buf, "hello world", ' ', 64) == buf + 6 && !strcmp(buf, "hello "));
  CHECK("memccpy not found", memccpy(buf, "abc", 'z', 3) == 0);
  strcpy(buf, "abc");
  CHECK("strlcat", strlcat(buf, "defgh", 6) == 8 && !strcmp(buf, "abcde"));
  CHECK("strlcat full", strlcat(buf, "x", 3) == 4);
  CHECK("strcoll", strcoll("a", "b") < 0 && strcoll("b", "b") == 0);
  CHECK("strsignal", !strcmp(strsignal(SIGINT), "Interrupt") && !strcmp(strsignal(SIGKILL), "Killed"));
  char *s = strdup("a,b,,c"), *sp = s;
  CHECK("strsep", !strcmp(strsep(&sp, ","), "a") && !strcmp(strsep(&sp, ","), "b") && !strcmp(strsep(&sp, ","), "") && !strcmp(strsep(&sp, ","), "c") && !sp);
  free(s);
  CHECK("strcasestr", !strcmp(strcasestr("Hello World", "WORLD"), "World"));
  const char *ab = "abcabc";
  CHECK("memrchr", (const char *)memrchr(ab, 'b', 6) == ab + 4 && !memrchr(ab, 'z', 6));
  CHECK("memmem", memmem("foobar", 6, "bar", 3) != 0 && !memmem("foobar", 6, "baz", 3));
  CHECK("stpncpy", stpncpy(buf, "ab", 4) == buf + 2 && buf[2] == 0 && buf[3] == 0);
  bzero(buf, 4);
  CHECK("bzero", !buf[0] && !buf[3]);
  bcopy("12345", buf, 6);
  CHECK("bcopy", !strcmp(buf, "12345"));
  CHECK("bcmp", bcmp("abc", "abd", 3) != 0 && bcmp("abc", "abc", 3) == 0);
  CHECK("index / rindex", !strcmp(index("a/b/c", '/'), "/b/c") && !strcmp(rindex("a/b/c", '/'), "/c"));
  CHECK("ffs", ffs(0) == 0 && ffs(8) == 4);
  CHECK("strcasecmp via strings.h", strcasecmp("HeLLo", "hello") == 0 && strncasecmp("abX", "ABy", 2) == 0);
}

static void test_qsort(void) {
  printf("qsort\n");
  int a[1000];
  srand(42);
  for (int i = 0; i < 1000; i++) a[i] = rand() % 500 - 250;
  qsort(a, 1000, sizeof a[0], cmp_int);
  int sorted = 1;
  for (int i = 1; i < 1000; i++) if (a[i - 1] > a[i]) sorted = 0;
  CHECK("qsort 1000 random ints with duplicates", sorted);
  for (int i = 0; i < 1000; i++) a[i] = 1000 - i;
  qsort(a, 1000, sizeof a[0], cmp_int);
  sorted = 1;
  for (int i = 0; i < 1000; i++) if (a[i] != i + 1) sorted = 0;
  CHECK("qsort 1000 reversed", sorted);
  for (int i = 0; i < 1000; i++) a[i] = 7;
  qsort(a, 1000, sizeof a[0], cmp_int);
  CHECK("qsort 1000 equal", a[0] == 7 && a[999] == 7);
  static struct rec r[1000];
  for (int i = 0; i < 1000; i++) r[i].key = (i * 7919) % 1000;
  qsort(r, 1000, sizeof r[0], cmp_rec);
  sorted = 1;
  for (int i = 0; i < 1000; i++) if (r[i].key != i) sorted = 0;
  CHECK("qsort 1000 odd-sized records", sorted);
  int key = 500;
  int *hit = bsearch(&key, a, 1, sizeof a[0], cmp_int);
  CHECK("bsearch miss", !hit);
}

/* Sizes that do not fit must be refused, and the heap must still work afterwards. */
static void test_malloc_limits(void) {
  printf("malloc limits\n");
  volatile size_t huge = (size_t)-16, a = 65536, b = 65537, half = 0x80000000u;
  errno = 0;
  void *p = malloc(huge);
  CHECK("malloc of a size that wraps when rounded returns NULL", !p);
  CHECK("  errno is ENOMEM", errno == ENOMEM);
  errno = 0;
  void *q = calloc(a, b);
  CHECK("calloc whose product does not fit returns NULL", !q);
  CHECK("  errno is ENOMEM", errno == ENOMEM);
  CHECK("malloc above PTRDIFF_MAX returns NULL", !malloc(half));
  char *s = malloc(100);
  CHECK("the heap still works afterwards", s != NULL);
  if (s) { memset(s, 'x', 100); CHECK("  and the block is usable", s[0] == 'x' && s[99] == 'x'); free(s); }
  char *z = calloc(16, 16);
  int zero = z != NULL;
  for (int i = 0; z && i < 256; i++) if (z[i]) zero = 0;
  CHECK("calloc of a size that fits returns zeroed memory", zero);
  free(z);
}

static void test_time(void) {
  printf("time\n");
  char buf[128];
  time_t t = 0;
  struct tm *tm = gmtime(&t);
  CHECK("gmtime(0) = 1970-01-01 Thu", tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 4 && tm->tm_yday == 0);
  t = 951782400; /* 2000-02-29 00:00:00 */
  tm = gmtime(&t);
  CHECK("gmtime leap day 2000-02-29", tm->tm_year == 100 && tm->tm_mon == 1 && tm->tm_mday == 29 && tm->tm_wday == 2 && tm->tm_yday == 59);
  t = 1700000000; /* 2023-11-14 22:13:20 Tue */
  tm = localtime(&t);
  CHECK("localtime 1700000000", tm->tm_year == 123 && tm->tm_mon == 10 && tm->tm_mday == 14 && tm->tm_hour == 22 && tm->tm_min == 13 && tm->tm_sec == 20 && tm->tm_wday == 2);
  t = -86400;
  tm = gmtime(&t);
  CHECK("gmtime(-86400) = 1969-12-31", tm->tm_year == 69 && tm->tm_mon == 11 && tm->tm_mday == 31 && tm->tm_wday == 3);
  t = 4102444800LL; /* 2100-01-01 */
  tm = gmtime(&t);
  CHECK("gmtime 2100-01-01 (64-bit time_t)", tm->tm_year == 200 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 5);
  t = 1700000000;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
  CHECK("strftime %Y-%m-%d %H:%M:%S", !strcmp(buf, "2023-11-14 22:13:20"));
  strftime(buf, sizeof buf, "%a %A %b %B %e %j %y %C", &tmv);
  CHECK("strftime %a %A %b %B %e %j %y %C", !strcmp(buf, "Tue Tuesday Nov November 14 318 23 20"));
  strftime(buf, sizeof buf, "%I:%M %p|%l|%k|%u %w|%D|%F|%T|%R", &tmv);
  CHECK("strftime %I %p %l %k %u %w %D %F %T %R", !strcmp(buf, "10:13 PM|10|22|2 2|11/14/23|2023-11-14|22:13:20|22:13"));
  strftime(buf, sizeof buf, "%U %W %V %G %s %z %Z %% %n%t", &tmv);
  CHECK("strftime %U %W %V %G %s %z %Z %%", !strcmp(buf, "46 46 46 2023 1700000000 +0000 UTC % \n\t"));
  strftime(buf, sizeof buf, "%c", &tmv);
  CHECK("strftime %c", !strcmp(buf, "Tue Nov 14 22:13:20 2023"));
  t = 1104537600; /* 2005-01-01 (Sat): ISO week 53 of 2004 */
  gmtime_r(&t, &tmv);
  strftime(buf, sizeof buf, "%V %G %g", &tmv);
  CHECK("strftime ISO week across years", !strcmp(buf, "53 2004 04"));
  CHECK("strftime too small returns 0", strftime(buf, 5, "%Y-%m-%d", &tmv) == 0);
  struct tm mk = {0};
  mk.tm_year = 123; mk.tm_mon = 12; mk.tm_mday = 1; /* month 12 normalizes to Jan 2024 */
  CHECK("mktime normalizes", mktime(&mk) == 1704067200 && mk.tm_year == 124 && mk.tm_mon == 0);
  t = 0;
  CHECK("ctime", !strcmp(ctime(&t), "Thu Jan  1 00:00:00 1970\n"));
  struct timeval tv;
  CHECK("gettimeofday", gettimeofday(&tv, 0) == 0 && tv.tv_sec > 1600000000 && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
  CHECK("time() agrees with gettimeofday", llabs(time(0) - tv.tv_sec) <= 1);
}

static void test_getopt(void) {
  printf("getopt\n");
  char *argv1[] = {"prog", "-a", "-bvalue", "-c", "carg", "-xy", "--", "-notopt", "file", 0};
  int argc1 = 9, c, a = 0, x = 0, y = 0;
  char *b = 0, *cc = 0;
  optind = 1;
  while ((c = getopt(argc1, argv1, "ab:c:xy")) != -1) {
    if (c == 'a') a = 1; else if (c == 'b') b = optarg; else if (c == 'c') cc = optarg;
    else if (c == 'x') x = 1; else if (c == 'y') y = 1;
  }
  CHECK("getopt flags, attached and separate args, bundles, --", a && x && y && b && !strcmp(b, "value") && cc && !strcmp(cc, "carg") && optind == 7 && !strcmp(argv1[optind], "-notopt"));
  char *argv2[] = {"prog", "-q", "-b", 0};
  optind = 1; opterr = 0;
  c = getopt(3, argv2, "b:");
  CHECK("getopt unknown option -> '?' and optopt", c == '?' && optopt == 'q');
  c = getopt(3, argv2, "b:");
  CHECK("getopt missing argument -> '?'", c == '?' && optopt == 'b');
  optind = 1;
  getopt(3, argv2, ":b:");
  c = getopt(3, argv2, ":b:");
  CHECK("getopt missing argument with leading ':' -> ':'", c == ':');
  char *argv3[] = {"prog", "file", "-a", 0};
  optind = 1;
  CHECK("getopt stops at first non-option", getopt(3, argv3, "a") == -1 && optind == 1);
  opterr = 1;
}

static void test_libgen(void) {
  printf("libgen / realpath\n");
  struct { const char *in, *base, *dir; } cases[] = {
    {"/usr/lib", "lib", "/usr"}, {"/usr/", "usr", "/"}, {"usr", "usr", "."}, {"/", "/", "/"},
    {".", ".", "."}, {"..", "..", "."}, {"a/b//", "b", "a"}, {"//x", "x", "/"}, {"", ".", "."}};
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    char b1[32], b2[32], name[96];
    strcpy(b1, cases[i].in); strcpy(b2, cases[i].in);
    char *bn = basename(b1), *dn = dirname(b2);
    snprintf(name, sizeof name, "basename/dirname(\"%s\") = \"%s\" \"%s\"", cases[i].in, bn, dn);
    check(name, !strcmp(bn, cases[i].base) && !strcmp(dn, cases[i].dir));
  }
  char rp[PATH_MAX];
  mkdir("/tmp/rp", 0755);
  mkdir("/tmp/rp/sub", 0755);
  chdir("/tmp/rp/sub");
  CHECK("realpath ..", realpath("..", rp) && !strcmp(rp, "/tmp/rp"));
  CHECK("realpath ./../sub/.", realpath("./../sub/.", rp) && !strcmp(rp, "/tmp/rp/sub"));
  CHECK("realpath /../tmp//rp/", realpath("/../tmp//rp/", rp) && !strcmp(rp, "/tmp/rp"));
  char *m = realpath("/tmp/rp/sub/../..", 0);
  CHECK("realpath allocates", m && !strcmp(m, "/tmp"));
  free(m);
  CHECK("realpath missing -> ENOENT", !realpath("nope", rp) && errno == ENOENT);
  chdir("/");
  rmdir("/tmp/rp/sub");
  rmdir("/tmp/rp");
  CHECK("PATH_MAX / NAME_MAX", PATH_MAX >= 256 && NAME_MAX == 255);
}

static int got_sig;
static void on_sig(int s) { got_sig = s; }
static void test_posix(void) {
  printf("posix\n");
  CHECK("signal returns previous handler", signal(SIGINT, on_sig) == SIG_DFL && signal(SIGINT, SIG_IGN) == on_sig);
  CHECK("raise ignored signal", raise(SIGINT) == 0);
  signal(SIGUSR1, on_sig);
  CHECK("raise runs handler", raise(SIGUSR1) == 0 && got_sig == SIGUSR1);
  CHECK("raise SIGCHLD default is ignored", raise(SIGCHLD) == 0);
  CHECK("signal(SIGKILL) -> SIG_ERR", signal(SIGKILL, on_sig) == SIG_ERR);
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGTERM);
  CHECK("sigaction stores and returns", sigaction(SIGTERM, &sa, 0) == 0 && sigaction(SIGTERM, 0, &old) == 0 && old.sa_handler == on_sig && sigismember(&old.sa_mask, SIGTERM));
  sigset_t set;
  sigfillset(&set);
  sigdelset(&set, SIGINT);
  CHECK("sigset ops", !sigismember(&set, SIGINT) && sigismember(&set, SIGHUP));
  CHECK("sigprocmask", sigprocmask(SIG_BLOCK, &set, 0) == 0);
  sig_atomic_t flag = 1;
  CHECK("sig_atomic_t", flag == 1);
  CHECK("kill(self, 0)", kill(getpid(), 0) == 0);
  CHECK("kill(bad pid) -> ESRCH", kill(9999, SIGTERM) == -1 && errno == ESRCH);
  struct utsname u;
  CHECK("uname", uname(&u) == 0 && u.sysname[0] && !strcmp(u.machine, "wasm32"));
  char host[HOST_NAME_MAX + 1];
  CHECK("gethostname", gethostname(host, sizeof host) == 0 && !strcmp(host, u.nodename));
  struct sysinfo si;
  CHECK("sysinfo", sysinfo(&si) == 0 && si.totalram > 0 && si.procs >= 1);
  CHECK("sysconf", sysconf(_SC_PAGESIZE) == 65536 && sysconf(_SC_NPROCESSORS_ONLN) == 1 && sysconf(_SC_OPEN_MAX) == 32);
  struct termios tio;
  int tty = isatty(0);
  CHECK("tcgetattr on a non-tty -> ENOTTY", tty || (tcgetattr(0, &tio) == -1 && errno == ENOTTY));
  int ptm = open("/dev/ptmx", O_RDWR), pts = -1;
  if (ptm >= 0) {
    unsigned n;
    char name[32];
    if (ioctl(ptm, TIOCGPTN, &n) == 0) { snprintf(name, sizeof name, "/dev/pts/%u", n); pts = open(name, O_RDWR); }
  }
  CHECK("open a pty pair", pts >= 0);
  if (pts >= 0) {
    CHECK("tcgetattr", tcgetattr(pts, &tio) == 0 && (tio.c_lflag & ICANON));
    struct termios raw = tio;
    cfmakeraw(&raw);
    CHECK("cfmakeraw clears ICANON/ECHO/ISIG", !(raw.c_lflag & (ICANON | ECHO | ISIG)) && (raw.c_cflag & CS8) == CS8 && raw.c_cc[VMIN] == 1);
    CHECK("tcsetattr TCSANOW + read back", tcsetattr(pts, TCSANOW, &raw) == 0 && tcgetattr(pts, &tio) == 0 && !(tio.c_lflag & ICANON));
    CHECK("tcsetattr bad action -> EINVAL", tcsetattr(pts, 7, &raw) == -1 && errno == EINVAL);
    write(ptm, "k", 1);
    char ch = 0;
    CHECK("raw mode delivers a byte without newline", read(pts, &ch, 1) == 1 && ch == 'k');
    struct winsize ws = {30, 100, 0, 0}, ws2;
    CHECK("ioctl TIOCSWINSZ/TIOCGWINSZ", ioctl(ptm, TIOCSWINSZ, &ws) == 0 && ioctl(pts, TIOCGWINSZ, &ws2) == 0 && ws2.ws_col == 100);
    CHECK("tcsetpgrp / tcgetpgrp", tcsetpgrp(pts, getpid()) == 0 && tcgetpgrp(pts) == getpid());
    CHECK("isatty(pts)", isatty(pts));
    fd_set rd;
    FD_ZERO(&rd); FD_SET(pts, &rd);
    struct timeval tv = {0, 0};
    CHECK("select: nothing to read", select(pts + 1, &rd, 0, 0, &tv) == 0 && !FD_ISSET(pts, &rd));
    write(ptm, "z", 1);
    FD_ZERO(&rd); FD_SET(pts, &rd);
    tv.tv_sec = 1;
    CHECK("select: readable", select(pts + 1, &rd, 0, 0, &tv) == 1 && FD_ISSET(pts, &rd));
    close(pts);
  }
  if (ptm >= 0) close(ptm);
  errno = 0;
  int r = setpgid(0, 0);
  CHECK("setpgid succeeds or ENOSYS", r == 0 || errno == ENOSYS);
  pid_t pg = getpgrp();
  CHECK("getpgrp/getpgid succeed or ENOSYS", (pg > 0 && getpgid(0) == pg) || errno == ENOSYS);
  CHECK("setlocale C", !strcmp(setlocale(LC_ALL, ""), "C") && !setlocale(LC_ALL, "fr_FR") && !strcmp(localeconv()->decimal_point, "."));
  char *al = alloca(100);
  memset(al, 'a', 100);
  CHECK("alloca", al[99] == 'a');
  CHECK("PRId64 / PRIu32", (snprintf(sbuf, sizeof sbuf, "%" PRId64 " %" PRIu32 " %" PRIxPTR, (int64_t)-5, (uint32_t)7, (uintptr_t)255), !strcmp(sbuf, "-5 7 ff")));
  /* files */
  int fd = open("/tmp/ren.a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  write(fd, "x", 1);
  close(fd);
  struct stat st;
  CHECK("rename", rename("/tmp/ren.a", "/tmp/ren.b") == 0 && stat("/tmp/ren.b", &st) == 0 && stat("/tmp/ren.a", &st) == -1);
  unlink("/tmp/ren.b");
  CHECK("rename missing -> ENOENT", rename("/tmp/nothing", "/tmp/x") == -1 && errno == ENOENT);
}

static void test_spawn(const char *self) {
  printf("system / popen\n");
  struct stat st;
  int have_sh = stat("/bin/sh", &st) == 0;
  CHECK("system(NULL) reports /bin/sh", system(0) == have_sh);
  if (!have_sh) {
    CHECK("system without /bin/sh -> -1", system("true") == -1);
    CHECK("popen without /bin/sh -> NULL", popen("true", "r") == 0);
    printf("  (no /bin/sh in this image: skipping shell-dependent checks)\n");
    return;
  }
  int r = system("exit 3");
  CHECK("system returns wait status", WIFEXITED(r) && WEXITSTATUS(r) == 3);
  r = system("/bin/true");
  CHECK("system runs an external program", WIFEXITED(r) && WEXITSTATUS(r) == 0);
  FILE *p = popen("echo from child; echo line 2", "r");
  char buf[64] = "";
  CHECK("popen read", p && fgets(buf, sizeof buf, p) && !strcmp(buf, "from child\n") && fgets(buf, sizeof buf, p) && !strcmp(buf, "line 2\n") && !fgets(buf, sizeof buf, p));
  if (p) CHECK("pclose returns the exit status", WIFEXITED(pclose(p)));
  p = popen("exit 5", "r");
  CHECK("pclose exit 5", p && WEXITSTATUS(pclose(p)) == 5);
  if (stat("/bin/echo", &st) == 0) {
    p = popen("/bin/echo grandchild", "r");
    CHECK("popen read from an external program", p && fgets(buf, sizeof buf, p) && !strcmp(buf, "grandchild\n"));
    if (p) pclose(p);
  }
  if (stat("/bin/cat", &st) == 0) {
    p = popen("/bin/cat > /tmp/libctest.popen", "w");
    CHECK("popen write", p && fputs("written through a pipe\n", p) >= 0 && pclose(p) == 0);
    FILE *f = fopen("/tmp/libctest.popen", "r");
    CHECK("popen write reached the child", f && fgets(buf, sizeof buf, f) && !strcmp(buf, "written through a pipe\n"));
    if (f) fclose(f);
    unlink("/tmp/libctest.popen");
  }
  (void)self;
}

int main(int argc, char **argv) {
  printf("libctest\n");
  test_math();
  test_printf();
  test_scanf();
  test_strtod();
  test_string();
  test_qsort();
  test_malloc_limits();
  test_time();
  test_getopt();
  test_libgen();
  test_posix();
  test_spawn(argc > 0 && argv[0][0] == '/' ? argv[0] : "/bin/libctest");
  printf("libctest: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
