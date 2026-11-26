// SPDX-License-Identifier: GPL-2.0-or-later
/* man [section] name: show /usr/share/man/manN/name.N through a small pager
 * (space: next page, enter: next line, q: quit) when on a terminal. Pages are
 * preformatted plain text; section headings are highlighted on a terminal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static char *find_page(const char *sec, const char *name) {
  for (int s = 1; s <= 9; s++) {
    if (sec && atoi(sec) != s) continue;
    char dir[64];
    snprintf(dir, sizeof dir, "/usr/share/man/man%d", s);
    DIR *d = opendir(dir);
    if (!d) continue;
    struct dirent *e;
    size_t nl = strlen(name);
    while ((e = readdir(d)))
      if (!strncmp(e->d_name, name, nl) && e->d_name[nl] == '.') {
        char *p = malloc(strlen(dir) + strlen(e->d_name) + 2);
        sprintf(p, "%s/%s", dir, e->d_name);
        closedir(d);
        return p;
      }
    closedir(d);
  }
  return NULL;
}
static char *slurp(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  size_t cap = 8192, n = 0;
  char *b = malloc(cap);
  long r;
  while ((r = read(fd, b + n, cap - n - 1)) > 0) { n += r; if (n + 1 >= cap) b = realloc(b, cap *= 2); }
  close(fd);
  b[n] = 0;
  *len = n;
  return b;
}
static int is_heading(const char *s, const char *e) {
  if (s == e || !isupper((unsigned char)*s)) return 0;
  for (; s < e; s++) if (islower((unsigned char)*s)) return 0;
  return 1;
}
static void put_line(const char *s, const char *e, int tty) {
  int h = tty && is_heading(s, e);
  if (h) fputs("\033[1;38;2;254;128;25m", stdout);
  fwrite(s, 1, e - s, stdout);
  if (h) fputs("\033[0m", stdout);
  putchar('\n');
}
int main(int argc, char **argv) {
  const char *sec = NULL, *name;
  if (argc == 3 && isdigit((unsigned char)argv[1][0])) sec = argv[1], name = argv[2];
  else if (argc == 2) name = argv[1];
  else { fprintf(stderr, "usage: man [section] name\n"); return 1; }
  char *path = find_page(sec, name);
  if (!path) { fprintf(stderr, "No manual entry for %s%s%s\n", name, sec ? " in section " : "", sec ? sec : ""); return 16; }
  size_t len;
  char *text = slurp(path, &len);
  if (!text) { perror(path); return 1; }
  struct termios orig, raw;
  int out_tty = __syscall3(SYS_ioctl, 1, TCGETS, &raw) == 0;
  int in_tty = __syscall3(SYS_ioctl, 0, TCGETS, &orig) == 0;
  if (!out_tty || !in_tty) { fwrite(text, 1, len, stdout); return 0; }
  struct winsize ws;
  int rows = 24, cols = 80;
  if (__syscall3(SYS_ioctl, 1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2) { rows = ws.ws_row; if (ws.ws_col > 10) cols = ws.ws_col; }
  raw = orig;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  __syscall3(SYS_ioctl, 0, TCSETSW, &raw);
  const char *p = text, *end = text + len;
  int nlines = 0, shown = 0;
  for (const char *q = text; q < end; q++) if (*q == '\n') nlines++;
  int budget = rows - 1;
  while (p < end) {
    while (p < end) {
      const char *e = memchr(p, '\n', end - p);
      if (!e) e = end;
      int w = 0; /* screen rows this line takes once the terminal wraps it */
      for (const char *q = p; q < e; q++) if (((unsigned char)*q & 0xc0) != 0x80) w++;
      int need = w ? (w + cols - 1) / cols : 1;
      if (need > budget && budget < rows - 1) break;
      put_line(p, e, 1);
      p = e < end ? e + 1 : end;
      shown++;
      budget -= need;
      if (budget <= 0) break;
    }
    if (p >= end) break;
    char prompt[256];
    int n = snprintf(prompt, sizeof prompt, " %s(%s) line %d/%d  space: page, enter: line, q: quit ", name, strrchr(path, '.') + 1, shown, nlines);
    if (n >= cols) n = cols - 1; /* one row only, so it can be erased */
    if (n < 0) n = 0;
    printf("\033[7m%.*s\033[0m", n, prompt);
    fflush(stdout);
    char c;
    if (read(0, &c, 1) != 1) c = 'q';
    printf("\r\033[K");
    if (c == 'q' || c == 'Q') break;
    budget = (c == '\n' || c == '\r' || c == 'j') ? 1 : rows - 1;
  }
  fflush(stdout);
  __syscall3(SYS_ioctl, 0, TCSETSW, &orig);
  return 0;
}
