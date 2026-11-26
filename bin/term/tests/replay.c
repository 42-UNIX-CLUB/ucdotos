// SPDX-License-Identifier: GPL-2.0-or-later
/* Replay a recorded byte stream through vt.c and print the screen at checkpoints.
 *   replay COLS ROWS FILE [OFFSET...]
 * For every offset (and the end of the file) prints:
 *   @ OFFSET CURSOR_X CURSOR_Y ALT
 *   one line per row: the text (UTF-8, trailing blanks removed)
 *   ~ one line per row: per-cell attribute codes  fg/bg as P<n>, R<hex> or -, then attr bits */
#include "../vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void color(char *o, uint32_t c) {
  if (VT_IS_PAL(c)) sprintf(o, "P%u", c & 255);
  else if (VT_IS_RGB(c)) sprintf(o, "R%06x", c & 0xffffff);
  else strcpy(o, "-");
}
static void dump(vt *t, long off) {
  printf("@ %ld %d %d %d\n", off, t->x, t->y, t->alt_active);
  static char line[1 << 16];
  for (int y = 0; y < t->rows; y++) { vt_row_text(t, y, line, sizeof line); printf("%s\n", line); }
  for (int y = 0; y < t->rows; y++) {
    printf("~");
    for (int x = 0; x < t->cols; x++) {
      vt_cell c = t->scr[y][x];
      char f[16], b[16];
      color(f, c.fg); color(b, c.bg);
      printf(" %s,%s,%x", f, b, c.attr);
    }
    printf("\n");
  }
}
int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: replay COLS ROWS FILE [OFFSET...]\n"); return 2; }
  FILE *f = fopen(argv[3], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = malloc((size_t)n + 1);
  if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);
  vt *t = vt_new(atoi(argv[1]), atoi(argv[2]), 2000);
  long pos = 0;
  for (int i = 4; i <= argc; i++) {
    long off = i < argc ? atol(argv[i]) : n;
    if (off > n) off = n;
    if (off > pos) { vt_feed(t, d + pos, (size_t)(off - pos)); pos = off; }
    t->reply_len = 0;
    dump(t, off);
  }
  vt_free(t);
  return 0;
}
