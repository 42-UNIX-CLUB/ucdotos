// SPDX-License-Identifier: GPL-2.0-or-later
/* Navigation checks for blueprint, native: cc -o /tmp/navtest bin/blueprint/tests/navtest.c && /tmp/navtest */
#define main blueprint_main
#include "../blueprint.c"
#undef main
/* the 14 cases of the slides program's internal/navigation/navigation_test.go, 11 slides */
int main(void) {
  static char *one[] = {"x"};
  struct { const char *keys; int target; } t[] = {
    {"", 0}, {"l", 1}, {"jjjjjjjjjj", 10}, {"jjjjjjjjjjjjj", 10}, {"G", 10}, {"llgg", 0}, {"2j", 2},
    {"0j", 1}, {"-11G", 10}, {"0G", 0}, {"3G", 2}, {"11G", 10}, {"101G", 10}, {"nnN", 1}};
  nslides = 11; slides = calloc(11, sizeof *slides);
  for (int i = 0; i < 11; i++) { slides[i].line = one; slides[i].nlines = 1; }
  R = 40; int fail = 0;
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
    page = 0; scroll = 0; nav[0] = 0;
    for (const char *k = t[i].keys; *k; k++) key(*k);
    if (page != t[i].target || nav[0]) { printf("FAIL %s: page %d nav '%s'\n", t[i].keys, page, nav); fail++; }
  }
  /* a tall slide: next scrolls first, then moves on; back scrolls up first */
  static char *tall[60]; for (int i = 0; i < 60; i++) tall[i] = "y";
  slides[0].line = tall; slides[0].nlines = 60; R = 30; page = 0; scroll = 0;
  key(' '); int s1 = scroll; key(' '); int s2 = scroll; key(' '); int p3 = page; key('k'); int p4 = page, s4 = scroll;
  printf("tall: scroll %d then %d, then page %d; back: page %d scroll %d\n", s1, s2, p3, p4, s4);
  key('k'); printf("back again: page %d scroll %d\n", page, scroll);
  printf("%d of 14 cases failed\n", fail);
  return fail != 0;
}
