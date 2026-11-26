// SPDX-License-Identifier: GPL-2.0-or-later
/* Line editor core: a pure state machine fed one byte at a time. It does no
 * I/O, so it can be unit-tested on the host (see test/le_test.c). */
#ifndef LINEEDIT_H
#define LINEEDIT_H
#define LE_MAX 4096
#define LE_MAXCANDS 512

/* what the front end must do after a byte */
enum { LE_NONE, LE_REDRAW, LE_ACCEPT, LE_EOF, LE_INTR, LE_CLEAR, LE_LIST, LE_BELL };

/* fills out[] with malloc'ed completions of word (each one a full replacement
 * for the word; directories end in '/'); returns how many */
typedef int (*le_complete_fn)(void *user, const char *word, int is_cmd, char **out, int max);

struct le {
  char buf[LE_MAX];
  int len, pos;
  char kill[LE_MAX];
  int killlen;
  char saved[LE_MAX]; /* the line being edited while browsing history */
  int savedlen;
  const char *const *hist;
  int nhist, hidx;
  int esc, seqlen;
  char seq[16];
  le_complete_fn complete;
  void *user;
  char *cands[LE_MAXCANDS]; /* filled by Tab; the caller frees them (le_free_cands) */
  int ncands;
};

void le_reset(struct le *e, const char *const *hist, int nhist);
int le_feed(struct le *e, int byte);
int le_cols(const char *s, int n); /* display columns of n bytes of UTF-8 */
void le_free_cands(struct le *e);
#endif
