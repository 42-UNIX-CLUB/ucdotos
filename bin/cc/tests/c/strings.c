// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>
int main(void) {
  const char *s = "tab\there\\ \"quoted\" \x41\102\0hidden";
  printf("%s|%d\n", s, (int)sizeof("tab\there\\ \"quoted\" \x41\102\0hidden"));
  printf("%s\n", "caf\xc3\xa9 é \U0001F600");
  printf("%d %d %d\n", '\'', '\\', '\0');
  printf("%d\n", (int)strlen(u8"ü"));
  char a[] = {'h', 'i', 0};
  printf("%s\n", a);
  const char *lines = "one\n"
                      "two\n";
  fputs(lines, stdout);
  printf("%c%c\n", "xyz"[1], *("abc" + 2));
  unsigned short u16[] = u"hé";
  unsigned u32[] = U"\U0001F600";
  printf("%d %d %x\n", u16[1], (int)(sizeof u16 / 2), u32[0]);
  printf("%d\n", L'\x1234');
  printf("%s\n", __func__);
  return 0;
}
