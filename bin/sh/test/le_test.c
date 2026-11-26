// SPDX-License-Identifier: GPL-2.0-or-later
/* Host unit test for the line editor core. Build and run on Linux:
 *   cc -Wall -o /tmp/le_test bin/sh/test/le_test.c bin/sh/lineedit.c && /tmp/le_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lineedit.h"

static int fails, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("  ok    %s\n", name); else { fails++; printf("  FAIL  %s\n", name); } } while (0)

static int feed(struct le *e, const char *keys) {
  int r = LE_NONE;
  for (const char *k = keys; *k; k++) r = le_feed(e, (unsigned char)*k);
  return r;
}
static int line_is(struct le *e, const char *s, int pos) { return !strcmp(e->buf, s) && e->len == (int)strlen(s) && e->pos == pos; }

static const char *words[] = {"echo", "ed", "env", "exit", 0};
static int fake_complete(void *user, const char *word, int is_cmd, char **out, int max) {
  int n = 0;
  if (is_cmd) {
    for (int i = 0; words[i]; i++) if (!strncmp(words[i], word, strlen(word)) && n < max) out[n++] = strdup(words[i]);
  } else {
    static const char *files[] = {"README.md", "rootfs/", "run.mjs", 0};
    for (int i = 0; files[i]; i++) if (!strncmp(files[i], word, strlen(word)) && n < max) out[n++] = strdup(files[i]);
  }
  return n;
}

int main(void) {
  static struct le e;
  const char *hist[] = {"ls -l", "echo one"};
  le_reset(&e, hist, 2);
  CHECK("typing inserts", feed(&e, "hello") == LE_REDRAW && line_is(&e, "hello", 5));
  CHECK("left arrow", feed(&e, "\033[D\033[D") == LE_REDRAW && e.pos == 3);
  feed(&e, "X");
  CHECK("insert in the middle", line_is(&e, "helXlo", 4));
  feed(&e, "\001");
  CHECK("Ctrl-A goes home", e.pos == 0);
  feed(&e, "\005");
  CHECK("Ctrl-E goes to the end", e.pos == 6);
  feed(&e, "\033[H");
  CHECK("Home key", e.pos == 0);
  feed(&e, "\033[F");
  CHECK("End key", e.pos == 6);
  feed(&e, "\033[1~");
  CHECK("Home key, VT form", e.pos == 0);
  feed(&e, "\033[4~");
  CHECK("End key, VT form", e.pos == 6);
  feed(&e, "\177");
  CHECK("backspace", line_is(&e, "helXl", 5));
  feed(&e, "\001\033[C\033[3~");
  CHECK("right arrow and Delete", line_is(&e, "hlXl", 1));
  feed(&e, "\013");
  CHECK("Ctrl-K kills to the end", line_is(&e, "h", 1));
  feed(&e, "\031");
  CHECK("Ctrl-Y yanks it back", line_is(&e, "hlXl", 4));
  le_reset(&e, hist, 2);
  feed(&e, "echo foo bar");
  feed(&e, "\027");
  CHECK("Ctrl-W kills a word", line_is(&e, "echo foo ", 9));
  feed(&e, "\025");
  CHECK("Ctrl-U kills to the start", line_is(&e, "", 0));
  CHECK("Ctrl-D on an empty line is EOF", le_feed(&e, 4) == LE_EOF);
  feed(&e, "ab\001");
  CHECK("Ctrl-D deletes under the cursor", le_feed(&e, 4) == LE_REDRAW && line_is(&e, "b", 0));
  CHECK("Ctrl-C clears the line", le_feed(&e, 3) == LE_INTR && line_is(&e, "", 0));
  CHECK("Ctrl-L asks for a clear screen", le_feed(&e, 12) == LE_CLEAR);
  CHECK("Enter accepts", feed(&e, "ok\r") == LE_ACCEPT && !strcmp(e.buf, "ok"));
  le_reset(&e, hist, 2);
  CHECK("Enter as LF accepts", le_feed(&e, '\n') == LE_ACCEPT);

  /* history */
  le_reset(&e, hist, 2);
  feed(&e, "draft");
  feed(&e, "\033[A");
  CHECK("up: last history entry", line_is(&e, "echo one", 8));
  feed(&e, "\033[A");
  CHECK("up again: older entry", line_is(&e, "ls -l", 5));
  CHECK("up at the oldest entry rings", feed(&e, "\033[A") == LE_BELL);
  feed(&e, "\033[B\033[B");
  CHECK("down back to the draft", line_is(&e, "draft", 5));
  CHECK("down past the end rings", feed(&e, "\033[B") == LE_BELL);
  feed(&e, "\020");
  CHECK("Ctrl-P is up", line_is(&e, "echo one", 8));
  feed(&e, "\016");
  CHECK("Ctrl-N is down", line_is(&e, "draft", 5));
  feed(&e, "\033OA");
  CHECK("up arrow, application mode", line_is(&e, "echo one", 8));

  /* UTF-8 */
  le_reset(&e, hist, 2);
  feed(&e, "a\xe2\x80\xba" "b");
  CHECK("UTF-8 stored as bytes", e.len == 5 && le_cols(e.buf, e.len) == 3);
  feed(&e, "\033[D\033[D");
  CHECK("left skips a whole character", e.pos == 1);
  feed(&e, "\033[C\177");
  CHECK("backspace removes a whole character", line_is(&e, "ab", 1));

  /* word movement */
  le_reset(&e, hist, 2);
  feed(&e, "one two three");
  feed(&e, "\033b");
  CHECK("Alt-B: word left", e.pos == 8);
  feed(&e, "\033[1;5D");
  CHECK("Ctrl-Left: word left", e.pos == 4);
  feed(&e, "\033f");
  CHECK("Alt-F: word right", e.pos == 7);

  /* completion */
  le_reset(&e, hist, 2);
  e.complete = fake_complete;
  feed(&e, "ec");
  CHECK("Tab completes a unique command", feed(&e, "\t") == LE_REDRAW && line_is(&e, "echo ", 5));
  feed(&e, "R");
  CHECK("Tab completes a file name", feed(&e, "\t") == LE_REDRAW && line_is(&e, "echo README.md ", 15));
  feed(&e, "r");
  int r = feed(&e, "\t");
  CHECK("ambiguous file: list", r == LE_LIST && e.ncands == 2);
  le_free_cands(&e);
  feed(&e, "o");
  CHECK("directory completion adds no space", feed(&e, "\t") == LE_REDRAW && line_is(&e, "echo README.md rootfs/", 22));
  le_reset(&e, hist, 2);
  feed(&e, "e");
  r = feed(&e, "\t");
  CHECK("ambiguous command: list of 4", r == LE_LIST && e.ncands == 4 && line_is(&e, "e", 1));
  le_free_cands(&e);
  le_reset(&e, hist, 2);
  feed(&e, "ls; ex");
  CHECK("command position after ;", feed(&e, "\t") == LE_REDRAW && line_is(&e, "ls; exit ", 9));
  le_reset(&e, hist, 2);
  feed(&e, "zz");
  CHECK("no completion rings", feed(&e, "\t") == LE_BELL);

  printf("le_test: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
