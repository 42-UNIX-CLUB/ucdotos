// SPDX-License-Identifier: GPL-2.0-or-later
/* clip: the clipboard from the shell.
 *
 *   ls | clip        copy what comes in on stdin (also: clip -i)
 *   clip -o [N]      print entry N (0, the newest, by default)
 *   clip -l          list the history, newest first: number, size, start of the text
 *   clip -s N        make entry N the newest, so that Ctrl+Shift+V pastes it
 *   clip -d N        delete entry N
 *   clip -c          clear the history
 *
 * With no option and stdin a terminal, it prints the newest entry, like clip -o. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "clip.h"

__attribute__((noreturn)) static void usage(void) {
  fprintf(stderr, "usage: clip [-i | -o [N] | -l | -s N | -d N | -c]\n");
  exit(2);
}
static int fail(int r) {
  const char *why = r == -ENOENT ? "no such entry" : r == -ENOSPC ? "too big (1 MiB at most)" :
                    r == -EIO ? "the clipboard (clipd) did not answer" : strerror(-r);
  fprintf(stderr, "clip: %s\n", why);
  return 1;
}
static int number(const char *s) {
  char *end;
  long n = strtol(s, &end, 10);
  if (!*s || *end || n < 0) usage();
  return (int)n;
}

static int copy_in(void) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    if (!buf) return fail(-ENOMEM);
    if (len == cap) {
      if (cap >= CLIP_MAX_ENTRY + 1) return fail(-ENOSPC);
      char *n = realloc(buf, cap *= 2);
      if (!n) free(buf);
      buf = n;
      continue;
    }
    long r = read(0, buf + len, cap - len);
    if (r < 0) { perror("clip: stdin"); return 1; }
    if (r == 0) break;
    len += (size_t)r;
  }
  int r = clip_put(buf, len);
  free(buf);
  return r ? fail(r) : 0;
}
static int paste_out(int i) {
  char *d;
  size_t n;
  int r = clip_get(i, &d, &n);
  if (r) return fail(r);
  for (size_t off = 0; off < n;) {
    long w = write(1, d + off, n - off);
    if (w <= 0) { free(d); return 1; }
    off += (size_t)w;
  }
  free(d);
  return 0;
}
static int list(void) {
  char *t;
  size_t n;
  int r = clip_list(&t, &n);
  if (r) return fail(r);
  int i = 0;
  for (char *line = t, *next; *line; line = next, i++) {
    next = strchr(line, '\n');
    if (next) *next++ = 0; else next = line + strlen(line);
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    printf("%3d %8s  %s\n", i, line, tab + 1);
  }
  if (!i) printf("the clipboard is empty\n");
  free(t);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 1) return isatty(0) ? paste_out(0) : copy_in();
  const char *o = argv[1];
  if (!strcmp(o, "-i") && argc == 2) return copy_in();
  if (!strcmp(o, "-o") && argc <= 3) return paste_out(argc == 3 ? number(argv[2]) : 0);
  if (!strcmp(o, "-l") && argc == 2) return list();
  if (!strcmp(o, "-c") && argc == 2) { int r = clip_clear(); return r ? fail(r) : 0; }
  if (!strcmp(o, "-s") && argc == 3) { int r = clip_select(number(argv[2])); return r ? fail(r) : 0; }
  if (!strcmp(o, "-d") && argc == 3) { int r = clip_remove(number(argv[2])); return r ? fail(r) : 0; }
  usage();
}
