// SPDX-License-Identifier: GPL-2.0-or-later
/* which [-a] name...: locate a command in $PATH */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
  int all = 0, rc = 0, i = 1;
  if (i < argc && !strcmp(argv[i], "-a")) all = 1, i++;
  if (i >= argc) { fprintf(stderr, "usage: which [-a] name...\n"); return 1; }
  const char *path = getenv("PATH");
  if (!path) path = "/bin";
  for (; i < argc; i++) {
    int found = 0;
    struct stat st;
    if (strchr(argv[i], '/')) {
      if (stat(argv[i], &st) == 0 && S_ISREG(st.st_mode)) { puts(argv[i]); found = 1; }
    } else {
      const char *p = path;
      while (1) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char buf[512];
        if (n + strlen(argv[i]) + 2 < sizeof buf) {
          snprintf(buf, sizeof buf, "%.*s%s%s", (int)n, p, n ? "/" : "", argv[i]);
          if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) { puts(buf); found = 1; if (!all) break; }
        }
        if (!e) break;
        p = e + 1;
      }
    }
    if (!found) rc = 1;
  }
  return rc;
}
