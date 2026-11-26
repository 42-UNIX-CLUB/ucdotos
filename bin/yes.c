// SPDX-License-Identifier: GPL-2.0-or-later
/* yes [string...]: repeat a line until the reader goes away */
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  static char line[1024], buf[8192];
  size_t n = 0;
  if (argc < 2) strcpy(line, "y");
  for (int i = 1; i < argc; i++) {
    if (n + strlen(argv[i]) + 2 >= sizeof line) break;
    if (i > 1) line[n++] = ' ';
    strcpy(line + n, argv[i]);
    n += strlen(argv[i]);
  }
  n = strlen(line);
  line[n++] = '\n';
  size_t len = 0;
  while (len + n <= sizeof buf) { memcpy(buf + len, line, n); len += n; }
  for (;;) if (write(1, buf, len) < 0) return 1; /* SIGPIPE ends yes when the reader goes; with SIGPIPE ignored, EPIPE does */
}
