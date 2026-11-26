// SPDX-License-Identifier: GPL-2.0-or-later
/* Lexer and recursive-descent parser. Words are kept raw (quotes included);
 * expansion happens at run time in expand.c. */
#include "sh.h"

enum { T_WORD, T_NL, T_EOF, T_SEMI, T_DSEMI, T_AMP, T_AND, T_PIPE, T_OR, T_LP, T_RP, T_REDIR };

void *amalloc(struct arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (!a->head || a->head->used + n > a->head->cap) {
    size_t cap = n > 4000 ? n : 4000;
    struct achunk *c = malloc(sizeof *c + cap + 8);
    if (!c) { outs(2, "sh: out of memory\n"); sh_exit(2); }
    c->next = a->head; c->used = 0; c->cap = cap; a->head = c;
  }
  char *base = (char *)(((size_t)a->head->data + 7) & ~(size_t)7);
  void *r = base + a->head->used;
  a->head->used += n;
  memset(r, 0, n);
  return r;
}
char *astrndup(struct arena *a, const char *s, size_t n) {
  char *r = amalloc(a, n + 1);
  memcpy(r, s, n);
  r[n] = 0;
  return r;
}
void afree(struct arena *a) {
  while (a->head) { struct achunk *n = a->head->next; free(a->head); a->head = n; }
}

/* ---- lexer ---- */
static int is_meta(char c) { return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')' || !c; }
static void fail(struct parser *p, const char *fmt, const char *arg) {
  if (p->status != P_OK) return;
  p->status = P_ERROR;
  snprintf(p->errmsg, sizeof p->errmsg, fmt, arg);
}
static void incomplete(struct parser *p) { if (p->status == P_OK) p->status = P_INCOMPLETE; }

static long skip_dollar(const char *s, long i);
static long skip_squote(const char *s, long i) { while (s[i] && s[i] != '\'') i++; return s[i] ? i + 1 : -1; }
static long skip_bquote(const char *s, long i) {
  while (s[i] && s[i] != '`') { if (s[i] == '\\' && s[i + 1]) i++; i++; }
  return s[i] ? i + 1 : -1;
}
static long skip_dquote(const char *s, long i) {
  while (i >= 0 && s[i] && s[i] != '"') {
    if (s[i] == '\\' && s[i + 1]) i += 2;
    else if (s[i] == '$') i = skip_dollar(s, i);
    else if (s[i] == '`') i = skip_bquote(s, i + 1);
    else i++;
  }
  return i >= 0 && s[i] ? i + 1 : -1;
}
long subst_end(const char *s, long i);
/* i points at '$'; returns the index just past the construct, or -1 at end of input */
static long skip_dollar(const char *s, long i) {
  if (s[i + 1] == '(') return subst_end(s, i);
  if (s[i + 1] == '{') {
    for (i += 2; i >= 0 && s[i] && s[i] != '}';) {
      if (s[i] == '\\' && s[i + 1]) i += 2;
      else if (s[i] == '\'') i = skip_squote(s, i + 1);
      else if (s[i] == '"') i = skip_dquote(s, i + 1);
      else if (s[i] == '$') i = skip_dollar(s, i);
      else i++;
    }
    return i >= 0 && s[i] ? i + 1 : -1;
  }
  return i + 1;
}
static long scan_word(const char *s, long i) {
  while (i >= 0 && !is_meta(s[i])) {
    char c = s[i];
    if (c == '\\') { if (!s[i + 1] || (s[i + 1] == '\n' && !s[i + 2])) return -1; i += 2; }
    else if (c == '\'') i = skip_squote(s, i + 1);
    else if (c == '"') i = skip_dquote(s, i + 1);
    else if (c == '`') i = skip_bquote(s, i + 1);
    else if (c == '$') i = skip_dollar(s, i);
    else i++;
  }
  return i;
}

/* the delimiter of a here-document with quotes removed */
static char *unquote_delim(struct parser *p, const char *w) {
  char *r = amalloc(p->ar, strlen(w) + 1), *o = r;
  for (; *w; w++) {
    if (*w == '\'' || *w == '"') continue;
    if (*w == '\\' && w[1]) w++;
    *o++ = *w;
  }
  return r;
}
static void read_heredocs(struct parser *p) {
  const char *s = p->s;
  long i = p->pos;
  for (int k = 0; k < p->nheredocs; k++) {
    struct redir *r = p->heredocs[k];
    char *delim = unquote_delim(p, r->word);
    size_t dl = strlen(delim), cap = 256, len = 0;
    char *body = malloc(cap);
    for (;;) {
      if (!s[i]) { free(body); incomplete(p); p->nheredocs = 0; return; }
      long e = i;
      while (s[e] && s[e] != '\n') e++;
      long b = i;
      if (r->quoted & 2) while (b < e && s[b] == '\t') b++;
      if ((size_t)(e - b) == dl && !memcmp(s + b, delim, dl)) { i = s[e] ? e + 1 : e; break; }
      if (!s[e]) { free(body); incomplete(p); p->nheredocs = 0; return; }
      while (len + (e - b) + 2 > cap) body = realloc(body, cap *= 2);
      memcpy(body + len, s + b, e - b);
      len += e - b;
      body[len++] = '\n';
      i = e + 1;
    }
    r->word = astrndup(p->ar, body, len);
    free(body);
  }
  p->nheredocs = 0;
  p->pos = i;
}

static int lex_redir(struct parser *p, long i, int fd) {
  const char *s = p->s;
  int t;
  if (s[i] == '<') {
    if (s[i + 1] == '<') { t = R_HEREDOC; i += 2; p->tokfd = 0; if (s[i] == '-') { p->tokfd = 0x100; i++; } }
    else if (s[i + 1] == '&') t = R_DUPIN, i += 2;
    else if (s[i + 1] == '>') t = R_RDWR, i += 2;
    else t = R_IN, i++;
    if (fd < 0) fd = 0;
  } else {
    if (s[i + 1] == '>') t = R_APPEND, i += 2;
    else if (s[i + 1] == '&') t = R_DUPOUT, i += 2;
    else if (s[i + 1] == '|') t = R_OUT, i += 2;
    else t = R_OUT, i++;
    if (fd < 0) fd = 1;
  }
  p->tokredir = t;
  p->tokfd = (t == R_HEREDOC ? (p->tokfd & 0x100) : 0) | fd;
  p->pos = i;
  return T_REDIR;
}

static int lex(struct parser *p) {
  const char *s = p->s;
  long i = p->pos;
  for (;;) {
    if (s[i] == ' ' || s[i] == '\t') i++;
    else if (s[i] == '\\' && s[i + 1] == '\n') { if (!s[i + 2]) { incomplete(p); return T_EOF; } i += 2; }
    else break;
  }
  if (s[i] == '#') while (s[i] && s[i] != '\n') i++;
  p->tokstr = NULL;
  char c = s[i];
  p->pos = i + 1;
  switch (c) {
  case 0: p->pos = i; return T_EOF;
  case '\n': if (p->nheredocs) read_heredocs(p); return T_NL;
  case '&':
    if (s[i + 1] == '&') { p->pos = i + 2; return T_AND; }
    if (s[i + 1] == '>') {
      p->tokredir = s[i + 2] == '>' ? R_BOTHAPP : R_BOTH;
      p->tokfd = 1;
      p->pos = i + (s[i + 2] == '>' ? 3 : 2);
      return T_REDIR;
    }
    return T_AMP;
  case '|': if (s[i + 1] == '|') { p->pos = i + 2; return T_OR; } return T_PIPE;
  case ';': if (s[i + 1] == ';') { p->pos = i + 2; return T_DSEMI; } return T_SEMI;
  case '(': return T_LP;
  case ')': return T_RP;
  case '<': case '>': return lex_redir(p, i, -1);
  }
  long e = scan_word(s, i);
  if (e < 0) { incomplete(p); return T_EOF; }
  long j = i;
  while (j < e && isdigit((unsigned char)s[j])) j++;
  if (j == e && e - i < 3 && (s[e] == '<' || s[e] == '>')) return lex_redir(p, e, atoi(s + i));
  p->tokstr = astrndup(p->ar, s + i, e - i);
  p->pos = e;
  return T_WORD;
}

static int peek(struct parser *p) {
  if (!p->peeked) { p->tok = p->status ? T_EOF : lex(p); p->peeked = 1; }
  return p->tok;
}
static void consume(struct parser *p) { p->peeked = 0; }
static int is_word(struct parser *p, const char *w) { return peek(p) == T_WORD && !strcmp(p->tokstr, w); }
static void skip_nl(struct parser *p) { while (peek(p) == T_NL) consume(p); }
static const char *tokname(struct parser *p) {
  static const char *names[] = {"word", "newline", "end of file", ";", ";;", "&", "&&", "|", "||", "(", ")", "redirection"};
  return p->tok == T_WORD ? p->tokstr : names[p->tok];
}
static void unexpected(struct parser *p) {
  if (peek(p) == T_EOF) incomplete(p);
  else fail(p, "syntax error near unexpected token `%s'", tokname(p));
}
static int expect(struct parser *p, const char *w) {
  if (is_word(p, w)) { consume(p); return 1; }
  unexpected(p);
  return 0;
}
int is_special_word(const char *s) {
  static const char *w[] = {"then", "else", "elif", "fi", "do", "done", "esac", "}", 0};
  for (int i = 0; w[i]; i++) if (!strcmp(s, w[i])) return 1;
  return 0;
}
static int at_terminator(struct parser *p) {
  int t = peek(p);
  return t == T_EOF || t == T_RP || t == T_DSEMI || (t == T_WORD && is_special_word(p->tokstr));
}
static node *mk(struct parser *p, int type) { node *n = amalloc(p->ar, sizeof *n); n->type = type; return n; }
static node *mk2(struct parser *p, int type, node *a, node *b) { node *n = mk(p, type); n->a = a; n->b = b; return n; }

/* growable pointer vector, copied into the arena when done */
struct vec { void **v; int n, cap; };
static void vpush(struct vec *v, void *x) {
  if (v->n + 1 >= v->cap) v->v = realloc(v->v, (v->cap = v->cap ? v->cap * 2 : 8) * sizeof *v->v);
  v->v[v->n++] = x;
}
static void **vdone(struct parser *p, struct vec *v) {
  void **r = amalloc(p->ar, (v->n + 1) * sizeof *r);
  if (v->n) memcpy(r, v->v, v->n * sizeof *r);
  free(v->v);
  return r;
}

static node *parse_and_or(struct parser *p);
static node *parse_command(struct parser *p);

static node *parse_list(struct parser *p) {
  node *res = NULL;
  skip_nl(p);
  while (!p->status && !at_terminator(p)) {
    node *n = parse_and_or(p);
    if (!n) return NULL;
    int t = peek(p);
    if (t == T_AMP) { consume(p); n = mk2(p, N_BG, n, NULL); }
    else if (t == T_SEMI) consume(p);
    else if (t != T_NL && !at_terminator(p)) { unexpected(p); return NULL; }
    res = res ? mk2(p, N_SEQ, res, n) : n;
    skip_nl(p);
  }
  return res;
}

/* paren counting, for $(( )) and as a fallback */
static long paren_end(const char *s, long i) {
  int depth = 0;
  for (i++; i >= 0 && s[i];) {
    char c = s[i];
    if (c == '(') depth++, i++;
    else if (c == ')') { i++; if (!--depth) return i; }
    else if (c == '\\' && s[i + 1]) i += 2;
    else if (c == '\'') i = skip_squote(s, i + 1);
    else if (c == '"') i = skip_dquote(s, i + 1);
    else if (c == '`') i = skip_bquote(s, i + 1);
    else i++;
  }
  return -1;
}
/* s[i] == '$', s[i + 1] == '(': the index just past the matching ')', found
 * by parsing the inside (so "case x in a) ...;; esac" works), or -1 */
long subst_end(const char *s, long i) {
  if (s[i + 2] == '(') {
    long e = paren_end(s, i);
    if (e > 0 && s[e - 2] == ')') return e; /* $(( arithmetic )) */
  }
  struct arena ar = {0};
  struct parser p;
  parser_init(&p, s + i + 2, &ar);
  p.noalias = 1;
  parse_list(&p);
  long r;
  if (p.status == P_INCOMPLETE || (p.status == P_OK && peek(&p) == T_EOF)) r = -1;
  else if (p.status == P_OK && peek(&p) == T_RP) r = i + 2 + p.pos;
  else r = paren_end(s, i);
  afree(&ar);
  return r;
}

static void parse_redir(struct parser *p, struct redir **list) {
  struct redir *r = amalloc(p->ar, sizeof *r);
  r->type = p->tokredir;
  r->fd = p->tokfd & 0xff;
  if (r->type == R_HEREDOC && (p->tokfd & 0x100)) r->quoted |= 2;
  consume(p);
  if (peek(p) != T_WORD) { unexpected(p); return; }
  r->word = p->tokstr;
  consume(p);
  if (r->type == R_HEREDOC) {
    if (strpbrk(r->word, "'\"\\")) r->quoted |= 1;
    if (p->nheredocs < 16) p->heredocs[p->nheredocs++] = r;
  }
  while (*list) list = &(*list)->next;
  *list = r;
}

static int is_assignment(const char *w) {
  const char *e = strchr(w, '=');
  return e && e > w && valid_name(w, e - w);
}

static node *parse_simple(struct parser *p) {
  node *n = mk(p, N_CMD);
  struct vec words = {0}, assigns = {0};
  p->nalias_seen = 0;
  for (;;) {
    int t = peek(p);
    if (t == T_REDIR) { parse_redir(p, &n->redir); if (p->status) break; continue; }
    if (t != T_WORD) break;
    char *w = p->tokstr;
    if (!words.n) {
      if (is_assignment(w)) { vpush(&assigns, w); consume(p); continue; }
      char *al = (p->noalias || strpbrk(w, "'\"\\")) ? NULL : getalias(w);
      int seen = 0;
      for (int i = 0; i < p->nalias_seen; i++) if (!strcmp(p->alias_seen[i], w)) seen = 1;
      if (al && !seen && p->nalias_seen < 16) {
        p->alias_seen[p->nalias_seen++] = w;
        size_t al_len = strlen(al), rest = strlen(p->s + p->pos);
        char *ns = amalloc(p->ar, al_len + rest + 1);
        memcpy(ns, al, al_len);
        memcpy(ns + al_len, p->s + p->pos, rest + 1);
        p->s = ns; p->pos = 0; p->peeked = 0;
        continue;
      }
    }
    consume(p);
    vpush(&words, w);
    if (words.n == 1 && !assigns.n && !n->redir && peek(p) == T_LP) { /* name() compound-command */
      consume(p);
      if (peek(p) != T_RP) { unexpected(p); break; }
      consume(p);
      skip_nl(p);
      node *f = mk(p, N_FUNC);
      f->name = w;
      f->a = parse_command(p);
      free(words.v); free(assigns.v);
      if (!f->a) { unexpected(p); return NULL; }
      p->keep = 1;
      return f;
    }
  }
  n->nwords = words.n; n->nassigns = assigns.n;
  n->words = (char **)vdone(p, &words);
  n->assigns = (char **)vdone(p, &assigns);
  if (p->status) return NULL;
  if (!n->nwords && !n->nassigns && !n->redir) { unexpected(p); return NULL; }
  return n;
}

static node *parse_if_rest(struct parser *p) {
  node *n = mk(p, N_IF);
  n->a = parse_list(p);
  if (!expect(p, "then")) return NULL;
  n->b = parse_list(p);
  if (p->status) return NULL;
  if (is_word(p, "elif")) { consume(p); n->c = parse_if_rest(p); }
  else if (is_word(p, "else")) { consume(p); n->c = parse_list(p); }
  return p->status ? NULL : n;
}

static node *parse_compound(struct parser *p) {
  char *w = peek(p) == T_WORD ? p->tokstr : NULL;
  node *n = NULL;
  if (peek(p) == T_LP) {
    consume(p);
    n = mk(p, N_SUBSHELL);
    n->a = parse_list(p);
    if (peek(p) != T_RP) { unexpected(p); return NULL; }
    consume(p);
  } else if (!strcmp(w, "{")) {
    consume(p);
    n = mk(p, N_GROUP);
    n->a = parse_list(p);
    if (!expect(p, "}")) return NULL;
  } else if (!strcmp(w, "if")) {
    consume(p);
    n = parse_if_rest(p);
    if (!n || !expect(p, "fi")) return NULL;
  } else if (!strcmp(w, "while") || !strcmp(w, "until")) {
    consume(p);
    n = mk(p, w[0] == 'w' ? N_WHILE : N_UNTIL);
    n->a = parse_list(p);
    if (!expect(p, "do")) return NULL;
    n->b = parse_list(p);
    if (!expect(p, "done")) return NULL;
  } else if (!strcmp(w, "for")) {
    consume(p);
    if (peek(p) != T_WORD || !valid_name(p->tokstr, strlen(p->tokstr))) { unexpected(p); return NULL; }
    n = mk(p, N_FOR);
    n->name = p->tokstr;
    consume(p);
    n->nwords = -1; /* no "in": iterate over "$@" */
    if (peek(p) == T_SEMI) consume(p);
    skip_nl(p);
    if (is_word(p, "in")) {
      consume(p);
      struct vec words = {0};
      while (peek(p) == T_WORD) { vpush(&words, p->tokstr); consume(p); }
      n->nwords = words.n;
      n->words = (char **)vdone(p, &words);
      if (peek(p) == T_SEMI || peek(p) == T_NL) consume(p);
      else { unexpected(p); return NULL; }
      skip_nl(p);
    }
    if (!expect(p, "do")) return NULL;
    n->b = parse_list(p);
    if (!expect(p, "done")) return NULL;
  } else if (!strcmp(w, "case")) {
    consume(p);
    if (peek(p) != T_WORD) { unexpected(p); return NULL; }
    n = mk(p, N_CASE);
    n->name = p->tokstr;
    consume(p);
    skip_nl(p);
    if (!expect(p, "in")) return NULL;
    struct casei **tail = &n->cases;
    for (;;) {
      skip_nl(p);
      if (is_word(p, "esac")) { consume(p); break; }
      if (peek(p) == T_LP) consume(p);
      struct vec pats = {0};
      while (peek(p) == T_WORD) {
        vpush(&pats, p->tokstr);
        consume(p);
        if (peek(p) == T_PIPE) consume(p); else break;
      }
      if (!pats.n || peek(p) != T_RP) { free(pats.v); unexpected(p); return NULL; }
      consume(p);
      struct casei *ci = amalloc(p->ar, sizeof *ci);
      ci->npats = pats.n;
      ci->pats = (char **)vdone(p, &pats);
      ci->body = parse_list(p);
      if (p->status) return NULL;
      *tail = ci; tail = &ci->next;
      if (peek(p) == T_DSEMI) consume(p);
      else if (!is_word(p, "esac")) { unexpected(p); return NULL; }
    }
  }
  return n;
}

static node *parse_command(struct parser *p) {
  int t = peek(p);
  node *n;
  if (t == T_WORD && !strcmp(p->tokstr, "function")) { /* bash style: function name { ... } */
    consume(p);
    if (peek(p) != T_WORD) { unexpected(p); return NULL; }
    node *f = mk(p, N_FUNC);
    f->name = p->tokstr;
    consume(p);
    if (peek(p) == T_LP) { consume(p); if (peek(p) != T_RP) { unexpected(p); return NULL; } consume(p); }
    skip_nl(p);
    f->a = parse_command(p);
    if (!f->a) return NULL;
    p->keep = 1;
    return f;
  }
  if (t == T_LP || (t == T_WORD && (!strcmp(p->tokstr, "{") || !strcmp(p->tokstr, "if") || !strcmp(p->tokstr, "while") ||
                                    !strcmp(p->tokstr, "until") || !strcmp(p->tokstr, "for") || !strcmp(p->tokstr, "case")))) {
    n = parse_compound(p);
    if (!n) return NULL;
    while (peek(p) == T_REDIR) { parse_redir(p, &n->redir); if (p->status) return NULL; }
    return n;
  }
  if (t == T_WORD && is_special_word(p->tokstr)) { unexpected(p); return NULL; }
  if (t == T_WORD || t == T_REDIR) return parse_simple(p);
  unexpected(p);
  return NULL;
}

static node *parse_pipeline(struct parser *p) {
  int bang = 0;
  if (is_word(p, "!")) { consume(p); bang = 1; }
  node *c = parse_command(p);
  if (!c) return NULL;
  if (peek(p) != T_PIPE && !bang) return c;
  struct vec kids = {0};
  vpush(&kids, c);
  while (peek(p) == T_PIPE) {
    consume(p);
    skip_nl(p);
    node *k = parse_command(p);
    if (!k) { free(kids.v); return NULL; }
    vpush(&kids, k);
  }
  node *n = mk(p, N_PIPE);
  n->bang = bang;
  n->nkids = kids.n;
  n->kids = (node **)vdone(p, &kids);
  return n;
}

static node *parse_and_or(struct parser *p) {
  node *n = parse_pipeline(p);
  while (n && (peek(p) == T_AND || peek(p) == T_OR)) {
    int t = p->tok;
    consume(p);
    skip_nl(p);
    node *r = parse_pipeline(p);
    if (!r) return NULL;
    n = mk2(p, t == T_AND ? N_AND : N_OR, n, r);
  }
  return n;
}

void parser_init(struct parser *p, const char *s, struct arena *ar) {
  memset(p, 0, sizeof *p);
  p->s = s;
  p->ar = ar;
}

node *parse_one(struct parser *p) {
  p->status = P_OK;
  p->nheredocs = 0;
  skip_nl(p);
  if (peek(p) == T_EOF) { if (!p->status) p->status = P_END; return NULL; }
  node *res = NULL;
  for (;;) {
    node *n = parse_and_or(p);
    if (!n || p->status) return NULL;
    int t = peek(p), sep = 0;
    if (t == T_AMP) { consume(p); n = mk2(p, N_BG, n, NULL); sep = 1; }
    else if (t == T_SEMI) { consume(p); sep = 1; }
    res = res ? mk2(p, N_SEQ, res, n) : n;
    t = peek(p);
    if (t == T_NL) { consume(p); break; }
    if (t == T_EOF) { if (p->status) return NULL; break; }
    if (!sep) { unexpected(p); return NULL; }
  }
  return res;
}
