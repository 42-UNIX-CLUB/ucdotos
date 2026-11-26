// SPDX-License-Identifier: GPL-2.0-or-later
/* Examples from C11 6.10.3.5, turned into strings so the program can print them. */
#include <stdio.h>
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
#define x 3
#define f(a) f(x * (a))
#undef x
#define x 2
#define g f
#define z z[0]
#define h g(~
#define m(a) a(w)
#define w 0,1
#define t(a) a
#define p() int
#define q(x) x
#define r(x, y) x##y
#define str(s) #s
#define xstr(s) str(s)
#define debug(s, t) printf("x" #s "= %d, x" #t "= %s", x##s, x##t)
#define INCFILE(n) vers##n
#define glue(a, b) a##b
#define xglue(a, b) glue(a, b)
#define HIGHLOW "hello"
#define LOW LOW ", world"
#define hash_hash # ## #
#define mkstr(a) # a
#define in_between(a) mkstr(a)
#define join(c, d) in_between(c hash_hash d)
#define OBJ_LIKE (1-1)
#define FUNC_LIKE(a) ( a )
#define showlist(...) puts(#__VA_ARGS__)
#define report(test, ...) ((test) ? puts(#test) : printf(__VA_ARGS__))
#define EMPTY
#define LPAREN (
#define RPAREN )
#define F(x, y) x + y
#define ELLIP_FUNC(...) __VA_OPT__(__VA_ARGS__)
#define COUNT(...) sizeof((int[]){0, ##__VA_ARGS__}) / sizeof(int) - 1
int main(void) {
  puts(STR(f(y + 1) + f(f(z)) % t(t(g)(0) + t)(1);));
  puts(STR(p() i[q()] = {q(1), r(2, 3), r(4, ), r(, 5), r(, )};));
  puts(STR(char c[2][6] = {str(hello), str()};));
  puts(xstr(INCFILE(2).hh));
  puts(STR(glue(HIGH, LOW);));
  puts(STR(xglue(HIGH, LOW)));
  puts(join(x, y));
  showlist(The first, second, and third items.);
  int xa = 1;
  (void)xa;
  report(xa > 0, "x is %d but y is %d", xa, 2);
  puts(STR(ELLIP_FUNC(a, b) ELLIP_FUNC()));
  printf("%d %d\n", (int)(COUNT()), (int)(COUNT(1, 2, 3)));
  puts(STR(F(LPAREN, RPAREN)));
#define LINEA __LINE__
  printf("%d\n", LINEA);
#if defined(OBJ_LIKE) && !defined(NOPE) && (OBJ_LIKE == 0) && (1 ? 2 : (1 / 0)) && 0x10 == 16 && 'A' == 65
  puts("if ok");
#elif 1
  puts("elif wrong");
#else
  puts("else wrong");
#endif
#if -1 < 0u
  puts("unsigned wrong");
#else
  puts("unsigned ok");
#endif
#ifdef __STDC__
  puts("stdc");
#endif
#if __has_include(<stdio.h>) && !__has_include("nonexistent.h")
  puts("has_include ok");
#endif
  return 0;
}
