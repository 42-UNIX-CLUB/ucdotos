// SPDX-License-Identifier: GPL-2.0-or-later
/* Line editor core: key handling only, no I/O (see lineedit.h). */
#include <string.h>
#include <stdlib.h>
#include "lineedit.h"

static int is_cont(char c) { return ((unsigned char)c & 0xc0) == 0x80; }
int le_cols(const char *s, int n) {
  int c = 0;
  for (int i = 0; i < n; i++) if (!is_cont(s[i])) c++;
  return c;
}
void le_free_cands(struct le *e) {
  for (int i = 0; i < e->ncands; i++) free(e->cands[i]);
  e->ncands = 0;
}
void le_reset(struct le *e, const char *const *hist, int nhist) {
  e->len = e->pos = 0;
  e->buf[0] = 0;
  e->hist = hist;
  e->nhist = nhist;
  e->hidx = nhist;
  e->esc = e->seqlen = 0;
  e->savedlen = 0;
}

static int insert(struct le *e, const char *s, int n) {
  if (e->len + n >= LE_MAX - 1) return LE_BELL;
  memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
  memcpy(e->buf + e->pos, s, n);
  e->len += n;
  e->pos += n;
  e->buf[e->len] = 0;
  return LE_REDRAW;
}
/* remove bytes [a, b); keep: copy them into the kill buffer */
static int cut(struct le *e, int a, int b, int keep) {
  if (a >= b) return LE_NONE;
  if (keep) { memcpy(e->kill, e->buf + a, b - a); e->killlen = b - a; }
  memmove(e->buf + a, e->buf + b, e->len - b);
  e->len -= b - a;
  e->buf[e->len] = 0;
  if (e->pos > b) e->pos -= b - a;
  else if (e->pos > a) e->pos = a;
  return LE_REDRAW;
}
static int prev_char(struct le *e, int p) { if (p > 0) p--; while (p > 0 && is_cont(e->buf[p])) p--; return p; }
static int next_char(struct le *e, int p) { if (p < e->len) p++; while (p < e->len && is_cont(e->buf[p])) p++; return p; }
static int is_space(char c) { return c == ' ' || c == '\t'; }
static int word_left(struct le *e, int p) {
  while (p > 0 && is_space(e->buf[p - 1])) p--;
  while (p > 0 && !is_space(e->buf[p - 1])) p--;
  return p;
}
static int word_right(struct le *e, int p) {
  while (p < e->len && is_space(e->buf[p])) p++;
  while (p < e->len && !is_space(e->buf[p])) p++;
  return p;
}
static void load(struct le *e, const char *s, int n) {
  if (n > LE_MAX - 1) n = LE_MAX - 1;
  memcpy(e->buf, s, n);
  e->len = e->pos = n;
  e->buf[n] = 0;
}
static int history(struct le *e, int dir) {
  if (dir < 0) {
    if (e->hidx <= 0) return LE_BELL;
    if (e->hidx == e->nhist) { memcpy(e->saved, e->buf, e->len); e->savedlen = e->len; }
    e->hidx--;
    load(e, e->hist[e->hidx], strlen(e->hist[e->hidx]));
  } else {
    if (e->hidx >= e->nhist) return LE_BELL;
    e->hidx++;
    if (e->hidx == e->nhist) load(e, e->saved, e->savedlen);
    else load(e, e->hist[e->hidx], strlen(e->hist[e->hidx]));
  }
  return LE_REDRAW;
}

static int tab(struct le *e) {
  if (!e->complete) return LE_BELL;
  int start = e->pos;
  while (start > 0 && !strchr(" \t;|&<>()", e->buf[start - 1])) start--;
  int k = start - 1;
  while (k >= 0 && is_space(e->buf[k])) k--;
  int is_cmd = k < 0 || strchr(";|&(", e->buf[k]) != NULL;
  char word[LE_MAX];
  int wl = e->pos - start;
  memcpy(word, e->buf + start, wl);
  word[wl] = 0;
  le_free_cands(e);
  int n = e->complete(e->user, word, is_cmd, e->cands, LE_MAXCANDS);
  e->ncands = n;
  if (n <= 0) { e->ncands = 0; return LE_BELL; }
  int lcp = strlen(e->cands[0]);
  for (int i = 1; i < n; i++) {
    int j = 0;
    while (j < lcp && e->cands[i][j] == e->cands[0][j]) j++;
    lcp = j;
  }
  while (lcp > 0 && is_cont(e->cands[0][lcp])) lcp--; /* never split a UTF-8 sequence */
  if (n == 1 || lcp > wl) {
    cut(e, start, e->pos, 0);
    insert(e, e->cands[0], lcp);
    if (n == 1 && lcp > 0 && e->cands[0][lcp - 1] != '/') insert(e, " ", 1);
    if (n == 1) le_free_cands(e);
    else le_free_cands(e);
    return LE_REDRAW;
  }
  return LE_LIST;
}

static int escape_seq(struct le *e) {
  const char *s = e->seq;
  if (!strcmp(s, "[A") || !strcmp(s, "OA")) return history(e, -1);
  if (!strcmp(s, "[B") || !strcmp(s, "OB")) return history(e, 1);
  if (!strcmp(s, "[C") || !strcmp(s, "OC")) { e->pos = next_char(e, e->pos); return LE_REDRAW; }
  if (!strcmp(s, "[D") || !strcmp(s, "OD")) { e->pos = prev_char(e, e->pos); return LE_REDRAW; }
  if (!strcmp(s, "[H") || !strcmp(s, "OH") || !strcmp(s, "[1~") || !strcmp(s, "[7~")) { e->pos = 0; return LE_REDRAW; }
  if (!strcmp(s, "[F") || !strcmp(s, "OF") || !strcmp(s, "[4~") || !strcmp(s, "[8~")) { e->pos = e->len; return LE_REDRAW; }
  if (!strcmp(s, "[3~")) return cut(e, e->pos, next_char(e, e->pos), 0);
  if (!strcmp(s, "[1;5C") || !strcmp(s, "[1;3C")) { e->pos = word_right(e, e->pos); return LE_REDRAW; }
  if (!strcmp(s, "[1;5D") || !strcmp(s, "[1;3D")) { e->pos = word_left(e, e->pos); return LE_REDRAW; }
  return LE_NONE;
}

int le_feed(struct le *e, int c) {
  c &= 0xff;
  if (e->esc == 1) { /* after ESC */
    e->esc = 0;
    if (c == '[' || c == 'O') { e->esc = 2; e->seq[0] = c; e->seqlen = 1; return LE_NONE; }
    if (c == 'b') { e->pos = word_left(e, e->pos); return LE_REDRAW; }
    if (c == 'f') { e->pos = word_right(e, e->pos); return LE_REDRAW; }
    if (c == 'd') return cut(e, e->pos, word_right(e, e->pos), 1);
    if (c == 127 || c == 8) return cut(e, word_left(e, e->pos), e->pos, 1);
    return LE_NONE;
  }
  if (e->esc == 2) { /* inside ESC [ ... final */
    if (e->seqlen < (int)sizeof e->seq - 1) e->seq[e->seqlen++] = c;
    if (c >= 0x40 && c <= 0x7e && !(e->seqlen == 2 && e->seq[0] == '[' && c == '[')) {
      e->seq[e->seqlen] = 0;
      e->esc = 0;
      return escape_seq(e);
    }
    return LE_NONE;
  }
  switch (c) {
  case 27: e->esc = 1; return LE_NONE;
  case 1: e->pos = 0; return LE_REDRAW;                                  /* ^A */
  case 5: e->pos = e->len; return LE_REDRAW;                             /* ^E */
  case 2: e->pos = prev_char(e, e->pos); return LE_REDRAW;               /* ^B */
  case 6: e->pos = next_char(e, e->pos); return LE_REDRAW;               /* ^F */
  case 3: e->len = e->pos = 0; e->buf[0] = 0; e->hidx = e->nhist; return LE_INTR; /* ^C */
  case 4: if (!e->len) return LE_EOF; return cut(e, e->pos, next_char(e, e->pos), 0); /* ^D */
  case 8: case 127: return cut(e, prev_char(e, e->pos), e->pos, 0);     /* backspace */
  case 9: return tab(e);
  case 10: case 13: return LE_ACCEPT;
  case 11: return cut(e, e->pos, e->len, 1);                             /* ^K */
  case 21: return cut(e, 0, e->pos, 1);                                  /* ^U */
  case 23: return cut(e, word_left(e, e->pos), e->pos, 1);               /* ^W */
  case 25: return e->killlen ? insert(e, e->kill, e->killlen) : LE_NONE; /* ^Y */
  case 12: return LE_CLEAR;                                              /* ^L */
  case 16: return history(e, -1);                                        /* ^P */
  case 14: return history(e, 1);                                         /* ^N */
  }
  if (c < 32) return LE_NONE;
  char ch = c;
  return insert(e, &ch, 1);
}
