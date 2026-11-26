// SPDX-License-Identifier: GPL-2.0-or-later
/* c ? pointer : NULL and c ? NULL : pointer both have the pointer's type (C11 6.5.15p6),
 * so a member can be reached through the result. */
#include <stdio.h>
#include <stddef.h>
struct item { int id; const char *name; };
static struct item items[2] = {{7, "seven"}, {9, "nine"}};
static int cur = -1;
#define CUR (cur >= 0 ? &items[cur] : NULL)
#define RUC (cur < 0 ? NULL : &items[cur])
int main(void) {
  for (cur = -1; cur < 2; cur++) {
    if (CUR) printf("%d %s\n", CUR->id, CUR->name);
    if (RUC) printf("%d %s\n", RUC->id, RUC->name);
    printf("%d %d\n", (int)sizeof *(cur ? &items[0] : NULL), (int)sizeof *(cur ? NULL : &items[0]));
  }
  return 0;
}
