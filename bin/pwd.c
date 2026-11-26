// SPDX-License-Identifier: GPL-2.0-or-later
/* pwd: print the working directory */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int main(void) {
  char buf[1024];
  if (!getcwd(buf, sizeof buf)) { fprintf(stderr, "pwd: %s\n", strerror(errno)); return 1; }
  puts(buf);
  return 0;
}
