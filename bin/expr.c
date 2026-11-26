// SPDX-License-Identifier: GPL-2.0-or-later
/* expr EXPRESSION: evaluate and print. Operators, lowest precedence first:
 *   |   &   = != < <= > >=   + -   * / %   :   match substr index length + ( )
 * Comparisons are numeric when both sides are integers, else byte strings.
 * STRING : REGEX matches a basic regular expression anchored at the start.
 * Exit status: 0 result neither empty nor 0, 1 it is, 2 invalid expression,
 * 3 other error. Integers are 64-bit; overflow is an error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>
#include <limits.h>

static char **av;
static int ac, ip;

struct val { int isnum; long long n; char *s; };

static void die(int code, const char *msg) { fprintf(stderr, "expr: %s\n", msg); exit(code); }
static struct val vnum(long long n) { struct val v = {1, n, NULL}; return v; }
static struct val vstr(const char *s) { struct val v = {0, 0, strdup(s)}; return v; }
static int to_int(const char *s, long long *out) {
  const char *p = s;
  if (*p == '-') p++;
  if (!*p) return 0;
  for (const char *q = p; *q; q++) if (*q < '0' || *q > '9') return 0;
  errno = 0;
  long long v = strtoll(s, NULL, 10);
  if (errno == ERANGE) die(2, "integer is too large");
  *out = v;
  return 1;
}
static int is_int(struct val *v, long long *out) {
  if (v->isnum) { *out = v->n; return 1; }
  return to_int(v->s, out);
}
static char *str(struct val *v) {
  if (v->isnum) { char tmp[32]; snprintf(tmp, sizeof tmp, "%lld", v->n); v->s = strdup(tmp); v->isnum = 0; }
  return v->s;
}
static int is_null(struct val *v) {
  long long n;
  if (v->isnum) return v->n == 0;
  if (!*v->s) return 1;
  return to_int(v->s, &n) && n == 0;
}
static const char *peek(void) { return ip < ac ? av[ip] : NULL; }
static int accept(const char *op) { if (ip < ac && !strcmp(av[ip], op)) { ip++; return 1; } return 0; }
static const char *next_arg(void) {
  if (ip >= ac) {
    if (ip > 0) { fprintf(stderr, "expr: syntax error: missing argument after '%s'\n", av[ip - 1]); exit(2); }
    die(2, "missing operand");
  }
  return av[ip++];
}

static struct val parse_or(void);
static struct val do_match(struct val *, struct val *);
static struct val primary(void) {
  if (ip >= ac) next_arg();
  const char *t = peek();
  if (!strcmp(t, "(")) {
    ip++;
    struct val v = parse_or();
    if (!accept(")")) {
      if (ip < ac) { fprintf(stderr, "expr: syntax error: expecting ')' instead of '%s'\n", av[ip]); exit(2); }
      fprintf(stderr, "expr: syntax error: expecting ')' after '%s'\n", av[ip - 1]); exit(2);
    }
    return v;
  }
  if (!strcmp(t, ")")) { fprintf(stderr, "expr: syntax error: unexpected ')'\n"); exit(2); }
  if (!strcmp(t, "+")) { ip++; return vstr(next_arg()); }
  if (!strcmp(t, "length")) { ip++; struct val a = primary(); return vnum(strlen(str(&a))); }
  if (!strcmp(t, "match")) { ip++; struct val a = primary(), b = primary(); return do_match(&a, &b); }
  if (!strcmp(t, "index")) {
    ip++;
    struct val a = primary(), b = primary();
    size_t k = strcspn(str(&a), str(&b));
    return vnum(a.s[k] ? (long long)k + 1 : 0);
  }
  if (!strcmp(t, "substr")) {
    ip++;
    struct val a = primary(), p = primary(), l = primary();
    long long pos, len;
    const char *s = str(&a);
    long long slen = strlen(s);
    if (!is_int(&p, &pos) || !is_int(&l, &len) || pos < 1 || len < 1 || pos > slen) return vstr("");
    if (len > slen - pos + 1) len = slen - pos + 1;
    char *r = strndup(s + pos - 1, len);
    struct val v = {0, 0, r};
    return v;
  }
  ip++;
  return vstr(t);
}
static struct val do_match(struct val *a, struct val *b) {
  regex_t re;
  int rc = regcomp(&re, str(b), 0);
  if (rc) { char msg[128]; regerror(rc, &re, msg, sizeof msg); die(2, msg); }
  regmatch_t m[2];
  const char *s = str(a);
  int ok = regexec(&re, s, 2, m, 0) == 0 && m[0].rm_so == 0;
  struct val v;
  if (re.re_nsub > 0) v = ok && m[1].rm_so >= 0 ? (struct val){0, 0, strndup(s + m[1].rm_so, m[1].rm_eo - m[1].rm_so)} : vstr("");
  else v = vnum(ok ? m[0].rm_eo : 0);
  regfree(&re);
  return v;
}
static struct val parse_colon(void) {
  struct val a = primary();
  while (accept(":")) { struct val b = primary(); a = do_match(&a, &b); }
  return a;
}
static long long arith(struct val *a, struct val *b, int op) {
  long long x, y;
  if (!is_int(a, &x) || !is_int(b, &y)) die(2, "non-integer argument");
  switch (op) {
  /* overflow checks by hand: the 64-bit builtins need a runtime helper wasm32 does not have */
  case '+': if ((y > 0 && x > LLONG_MAX - y) || (y < 0 && x < LLONG_MIN - y)) die(2, "result too large"); return x + y;
  case '-': if ((y < 0 && x > LLONG_MAX + y) || (y > 0 && x < LLONG_MIN + y)) die(2, "result too large"); return x - y;
  case '*':
    /* compare with the limits by division before multiplying (no 128-bit helper needed) */
    if (x > 0 ? (y > 0 ? x > LLONG_MAX / y : y < LLONG_MIN / x) : (y > 0 ? x < LLONG_MIN / y : (x != 0 && y < LLONG_MAX / x)))
      die(2, "result too large");
    return x * y;
  default:
    if (y == 0) die(2, "division by zero");
    if (x == (-9223372036854775807LL - 1) && y == -1) die(2, "result too large");
    return op == '/' ? x / y : x % y;
  }
}
static struct val parse_mul(void) {
  struct val a = parse_colon();
  for (;;) {
    int op = accept("*") ? '*' : accept("/") ? '/' : accept("%") ? '%' : 0;
    if (!op) return a;
    struct val b = parse_colon();
    a = vnum(arith(&a, &b, op));
  }
}
static struct val parse_add(void) {
  struct val a = parse_mul();
  for (;;) {
    int op = accept("+") ? '+' : accept("-") ? '-' : 0;
    if (!op) return a;
    struct val b = parse_mul();
    a = vnum(arith(&a, &b, op));
  }
}
static struct val parse_cmp(void) {
  struct val a = parse_add();
  for (;;) {
    static const char *ops[] = {"=", "==", "!=", "<", "<=", ">", ">="};
    int k = -1;
    for (int i = 0; i < 7; i++) if (accept(ops[i])) { k = i; break; }
    if (k < 0) return a;
    struct val b = parse_add();
    long long x, y;
    int c;
    if (is_int(&a, &x) && is_int(&b, &y)) c = x < y ? -1 : x > y;
    else { c = strcmp(str(&a), str(&b)); c = c < 0 ? -1 : c > 0; }
    int r = k <= 1 ? c == 0 : k == 2 ? c != 0 : k == 3 ? c < 0 : k == 4 ? c <= 0 : k == 5 ? c > 0 : c >= 0;
    a = vnum(r);
  }
}
static struct val parse_and(void) {
  struct val a = parse_cmp();
  while (accept("&")) {
    struct val b = parse_cmp();
    if (is_null(&a) || is_null(&b)) a = vnum(0);
  }
  return a;
}
static struct val parse_or(void) {
  struct val a = parse_and();
  while (accept("|")) {
    struct val b = parse_and();
    if (is_null(&a)) a = is_null(&b) ? vnum(0) : b;
  }
  return a;
}

int main(int argc, char **argv) {
  av = argv + 1;
  ac = argc - 1;
  if (ac > 0 && !strcmp(av[0], "--")) { av++; ac--; }
  if (ac == 0) die(2, "missing operand");
  struct val v = parse_or();
  if (ip < ac) { fprintf(stderr, "expr: syntax error: unexpected argument '%s'\n", av[ip]); return 2; }
  puts(str(&v));
  if (fflush(stdout)) return 3;
  return is_null(&v) ? 1 : 0;
}
