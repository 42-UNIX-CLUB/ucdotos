// SPDX-License-Identifier: GPL-2.0-or-later
/* Characters to preprocessing tokens (C11 6.4). A whole file is lexed at once into an
 * array. Line splices are removed first; the newlines they removed are put back at
 * the end of the logical line so later line numbers stay right. */
#include "cc.h"

File *new_file(const char *name, const char *path, char *data, int len) {
  File *f = xalloc(sizeof *f);
  f->name = xstrdup(name);
  f->path = xstrdup(path);
  f->incdir = -1;
  const char *slash = strrchr(path, '/');
  f->dirlen = slash ? (int)(slash - path) : -1;
  /* phase 1 and 2: CR LF to LF, remove backslash-newline */
  char *out = malloc(len + 2);
  int n = 0, pending = 0;
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\r') {
      if (i + 1 < len && data[i + 1] == '\n') continue;
      c = '\n';
    }
    if (c == '\\') {
      int j = i + 1;
      if (j < len && data[j] == '\r') j++;
      if (j < len && data[j] == '\n') { i = j; pending++; continue; }
    }
    out[n++] = c;
    if (c == '\n')
      while (pending) { out[n++] = '\n'; pending--; }
  }
  if (n && out[n - 1] != '\n') out[n++] = '\n';
  while (pending) { out = xrealloc(out, n + pending + 1); out[n++] = '\n'; pending--; }
  out[n] = 0;
  f->data = out;
  f->len = n;
  return f;
}

static int is_idstart(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80; }
static int is_idchar(int c) { return is_idstart(c) || (c >= '0' && c <= '9'); }

static const struct { const char *s; int p; } puncts[] = {
  {"%:%:", P_HASHHASH}, {"...", P_ELLIPSIS}, {"<<=", P_SHLEQ}, {">>=", P_SHREQ},
  {"->", P_ARROW}, {"++", P_INC}, {"--", P_DEC}, {"<<", P_SHL}, {">>", P_SHR}, {"<=", P_LE},
  {">=", P_GE}, {"==", P_EQ}, {"!=", P_NE}, {"&&", P_LAND}, {"||", P_LOR}, {"*=", P_MULEQ},
  {"/=", P_DIVEQ}, {"%=", P_MODEQ}, {"+=", P_ADDEQ}, {"-=", P_SUBEQ}, {"&=", P_ANDEQ},
  {"^=", P_XOREQ}, {"|=", P_OREQ}, {"##", P_HASHHASH}, {"<:", '['}, {":>", ']'}, {"<%", '{'},
  {"%>", '}'}, {"%:", '#'},
};

Tok *lex_file(File *f, int *ntok) {
  int cap = f->len / 4 + 16, n = 0;
  Tok *toks = malloc(cap * sizeof *toks);
  const char *p = f->data, *linestart = p;
  int line = 1, bol = 1, space = 0;
  for (;;) {
    /* whitespace and comments */
    if (*p == '\n') { p++; line++; linestart = p; bol = 1; space = 0; continue; }
    if (*p == ' ' || *p == '\t' || *p == '\f' || *p == '\v' || *p == '\r') { p++; space = 1; continue; }
    if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; space = 1; continue; }
    if (p[0] == '/' && p[1] == '*') {
      const char *q = p + 2;
      for (;;) {
        if (!*q) {
          Tok t = {0}; t.file = f; t.line = line; t.col = (int)(p - linestart) + 1; t.s = p; t.len = 2;
          error_at(&t, "unterminated comment");
        }
        if (q[0] == '*' && q[1] == '/') break;
        if (*q == '\n') { line++; linestart = q + 1; }
        q++;
      }
      p = q + 2;
      space = 1;
      continue;
    }
    if (n + 1 >= cap) toks = xrealloc(toks, (cap *= 2) * sizeof *toks);
    Tok *t = &toks[n++];
    memset(t, 0, sizeof *t);
    t->file = f;
    t->line = line;
    t->col = (int)(p - linestart) + 1;
    t->bol = bol;
    t->space = space;
    t->s = p;
    bol = space = 0;
    if (!*p) { t->kind = T_EOF; break; }
    const char *q = p;
    /* string and character literals, with an optional encoding prefix */
    int pre = 0;
    if (q[0] == 'u' && q[1] == '8' && q[2] == '"') pre = 2;
    else if ((q[0] == 'L' || q[0] == 'u' || q[0] == 'U') && (q[1] == '"' || q[1] == '\'')) pre = 1;
    if (q[pre] == '"' || q[pre] == '\'') {
      char quote = q[pre];
      q += pre + 1;
      while (*q && *q != quote && *q != '\n') {
        if (*q == '\\' && q[1] && q[1] != '\n') q++;
        q++;
      }
      if (*q != quote) {
        /* an apostrophe in skipped text is not an error; the parser reports it if it gets there */
        t->kind = T_OTHER;
        t->len = 1;
        p++;
        continue;
      }
      q++;
      t->kind = quote == '"' ? T_STR : T_CHAR;
      t->len = (int)(q - p);
      p = q;
      continue;
    }
    if (is_idstart((u8)*q)) {
      while (is_idchar((u8)*q)) q++;
      t->kind = T_IDENT;
      t->len = (int)(q - p);
      t->id = intern(p, t->len);
      p = q;
      continue;
    }
    if ((*q >= '0' && *q <= '9') || (*q == '.' && q[1] >= '0' && q[1] <= '9')) {
      q++;
      for (;;) {
        if ((*q == 'e' || *q == 'E' || *q == 'p' || *q == 'P') && (q[1] == '+' || q[1] == '-')) q += 2;
        else if (is_idchar((u8)*q) || *q == '.') q++;
        else break;
      }
      t->kind = T_NUM;
      t->len = (int)(q - p);
      p = q;
      continue;
    }
    t->kind = T_PUNCT;
    for (size_t i = 0; i < sizeof puncts / sizeof *puncts; i++) {
      size_t l = strlen(puncts[i].s);
      if (!strncmp(q, puncts[i].s, l)) {
        t->punct = puncts[i].p;
        t->len = (int)l;
        p += l;
        goto next;
      }
    }
    if (strchr("[](){}.&*+-~!/%<>^|?:;=,#", *q)) {
      t->punct = (u8)*q;
      t->len = 1;
      p++;
      continue;
    }
    t->kind = T_OTHER; /* a stray character such as @ or a backslash */
    t->len = 1;
    p++;
  next:;
  }
  *ntok = n;
  return toks;
}
