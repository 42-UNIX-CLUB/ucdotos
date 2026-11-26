// SPDX-License-Identifier: GPL-2.0-or-later
/* uconf: the .uconf dotfile parser. The format is described in uconf.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uconf.h"

#define MAX_DEPTH 16

static void complain(uconf *c, int line, const char *fmt, const char *arg) {
  fprintf(stderr, "%s:%d: ", c->name, line);
  fprintf(stderr, fmt, arg ? arg : "");
  fputc('\n', stderr);
  c->errors++;
}

/* Every string the parser keeps is allocated here and freed by uconf_free. */
static char *keep(uconf *c, const char *s, size_t n) {
  if (c->nblocks % 64 == 0) {
    char **b = realloc(c->blocks, (size_t)(c->nblocks + 64) * sizeof *b);
    if (!b) return NULL;
    c->blocks = b;
  }
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  c->blocks[c->nblocks++] = p;
  return p;
}

static int ident_char(int ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
}
static int var_char(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }
static int blank(int ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

const char *uconf_var(uconf *c, const char *name) {
  for (int i = c->nvars - 2; i >= 0; i -= 2) if (!strcmp(c->vars[i], name)) return c->vars[i + 1];
  return NULL;
}
static void set_var(uconf *c, const char *name, const char *value) {
  for (int i = 0; i < c->nvars; i += 2)
    if (!strcmp(c->vars[i], name)) { c->vars[i + 1] = (char *)value; return; }
  if (c->nvars % 32 == 0) {
    char **v = realloc(c->vars, (size_t)(c->nvars + 32) * sizeof *v);
    if (!v) return;
    c->vars = v;
  }
  c->vars[c->nvars++] = (char *)name;
  c->vars[c->nvars++] = (char *)value;
}

/* The value as written (raw, n bytes) with $variables expanded and one pair of outer
 * quotes removed. */
static char *finish_value(uconf *c, const char *raw, size_t n, int line) {
  size_t cap = n + 1, len = 0;
  char *out = malloc(cap);
  if (!out) return NULL;
  for (size_t i = 0; i < n; i++) {
    const char *add = raw + i;
    size_t addn = 1;
    if (raw[i] == '$' && i + 1 < n && raw[i + 1] == '$') { i++; } /* $$ is a dollar sign */
    else if (raw[i] == '$' && i + 1 < n && var_char((unsigned char)raw[i + 1])) {
      size_t j = i + 1;
      while (j < n && var_char((unsigned char)raw[j])) j++;
      char name[64];
      size_t ln = j - i - 1 < sizeof name - 1 ? j - i - 1 : sizeof name - 1;
      memcpy(name, raw + i + 1, ln);
      name[ln] = 0;
      const char *v = uconf_var(c, name);
      if (v) { add = v; addn = strlen(v); i = j - 1; }
      else complain(c, line, "unknown variable $%s (define it first: $name = value)", name);
    }
    if (len + addn + 1 > cap) {
      while (len + addn + 1 > cap) cap *= 2;
      char *b = realloc(out, cap);
      if (!b) { free(out); return NULL; }
      out = b;
    }
    memcpy(out + len, add, addn);
    len += addn;
  }
  out[len] = 0;
  /* "one quoted string": drop the quotes; in double quotes \" \\ \n \t are escapes */
  if (len >= 2 && (out[0] == '"' || out[0] == '\'') && out[len - 1] == out[0]) {
    char q = out[0];
    size_t k = 1, w = 0;
    int whole = 1;
    for (; k < len - 1; k++) {
      if (q == '"' && out[k] == '\\' && k + 1 < len - 1) k++;
      else if (out[k] == q) { whole = 0; break; } /* "a" "b": two strings, leave it */
    }
    if (whole) {
      for (k = 1; k < len - 1; k++) {
        char ch = out[k];
        if (q == '"' && ch == '\\' && k + 1 < len - 1) {
          ch = out[++k];
          ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch;
        }
        out[w++] = ch;
      }
      out[w] = 0;
    }
  }
  char *kept = keep(c, out, strlen(out));
  free(out);
  return kept;
}

static void add_entry(uconf *c, const char *key, const char *value, const char *raw, int line) {
  if (c->n == c->cap) {
    int cap = c->cap ? c->cap * 2 : 32;
    uconf_entry *e = realloc(c->e, (size_t)cap * sizeof *e);
    if (!e) return;
    c->e = e;
    c->cap = cap;
  }
  c->e[c->n++] = (uconf_entry){key, value, raw, line, 0};
}

/* Does p start ", key =" (after the comma: blanks, a name, blanks, one '=')? */
static int next_setting(const char *p) {
  p++;
  while (blank((unsigned char)*p)) p++;
  if (*p == '$') p++;
  if (!ident_char((unsigned char)*p)) return 0;
  while (ident_char((unsigned char)*p)) p++;
  while (blank((unsigned char)*p)) p++;
  return *p == '=' && p[1] != '=';
}

void uconf_parse(uconf *c, const char *text, const char *name) {
  if (name) snprintf(c->name, sizeof c->name, "%s", name);
  char section[128] = "";
  char path[MAX_DEPTH][64];
  int depth = 0, line = 1;
  const char *p = text;
  for (;;) {
    while (blank((unsigned char)*p)) p++;
    if (!*p) break;
    if (*p == '\n') { line++; p++; continue; }
    if (*p == '#') { while (*p && *p != '\n') p++; continue; }
    if (*p == ',' && depth) { p++; continue; } /* between settings on one line inside braces */
    if (*p == '}') {
      if (!depth) complain(c, line, "a } with no { before it%s", NULL);
      else depth--;
      p++;
      continue;
    }
    if (*p == '[') { /* [section] */
      const char *s = ++p;
      while (*p && *p != ']' && *p != '\n') p++;
      if (*p != ']') { complain(c, line, "a [section] header needs its ]%s", NULL); continue; }
      const char *e = p++;
      while (s < e && blank((unsigned char)*s)) s++;
      while (e > s && blank((unsigned char)e[-1])) e--;
      int ok = e > s && (size_t)(e - s) < sizeof section;
      for (const char *q = s; q < e && ok; q++) ok = ident_char((unsigned char)*q);
      if (!ok) { complain(c, line, "a section name is letters, digits, _ - and .%s", NULL); section[0] = 0; }
      else { memcpy(section, s, (size_t)(e - s)); section[e - s] = 0; }
      if (depth) { complain(c, line, "a [section] cannot start inside { }: closing the open blocks%s", NULL); depth = 0; }
      while (blank((unsigned char)*p)) p++;
      if (*p && *p != '\n' && *p != '#') { complain(c, line, "unexpected text after the [section] header%s", NULL); while (*p && *p != '\n') p++; }
      continue;
    }
    /* a setting (name = value), a variable ($name = value) or a block (name {) */
    const char *k = p;
    int is_var = *p == '$';
    if (is_var) p++;
    while (is_var ? var_char((unsigned char)*p) : ident_char((unsigned char)*p)) p++;
    size_t kn = (size_t)(p - k);
    if (kn == (size_t)is_var) {
      complain(c, line, "expected a setting (name = value), a block (name {) or a [section]%s", NULL);
      while (*p && *p != '\n') p++;
      continue;
    }
    char key[64];
    if (kn >= sizeof key) kn = sizeof key - 1;
    memcpy(key, k, kn);
    key[kn] = 0;
    while (blank((unsigned char)*p)) p++;
    if (*p == '{' && !is_var) {
      if (depth == MAX_DEPTH) complain(c, line, "blocks nested too deep%s", NULL);
      else snprintf(path[depth++], sizeof path[0], "%s", key);
      p++;
      continue;
    }
    if (*p != '=') {
      complain(c, line, "expected = or { after '%s'", key);
      while (*p && *p != '\n') p++;
      continue;
    }
    p++;
    while (blank((unsigned char)*p)) p++;
    /* the value: to the end of the line, a "# " comment, or (inside braces) a } or ", key =" */
    const char *v = p;
    char q = 0;
    for (; *p && *p != '\n'; p++) {
      if (q) {
        if (q == '"' && *p == '\\' && p[1] && p[1] != '\n') p++;
        else if (*p == q) q = 0;
        continue;
      }
      if (*p == '"' || *p == '\'') { q = *p; continue; }
      if (*p == '#' && p > v && blank((unsigned char)p[-1]) && (!p[1] || p[1] == '\n' || blank((unsigned char)p[1]))) break;
      if (depth && *p == '}') break;
      if (depth && *p == ',' && next_setting(p)) break;
    }
    const char *e = p;
    while (e > v && blank((unsigned char)e[-1])) e--;
    if (*p == '#') while (*p && *p != '\n') p++;
    if (q) complain(c, line, "a quote is not closed%s", NULL);
    char *value = finish_value(c, v, (size_t)(e - v), line);
    if (!value) continue;
    if (is_var) { set_var(c, keep(c, key + 1, strlen(key + 1)), value); continue; }
    char full[512];
    size_t fl = 0;
    full[0] = 0;
    if (section[0]) fl += (size_t)snprintf(full + fl, sizeof full - fl, "%s", section);
    for (int i = 0; i < depth && fl < sizeof full; i++) fl += (size_t)snprintf(full + fl, sizeof full - fl, "%s%s", fl ? "." : "", path[i]);
    if (fl < sizeof full) snprintf(full + fl, sizeof full - fl, "%s%s", fl ? "." : "", key);
    const char *raw = keep(c, v, (size_t)(e - v));
    add_entry(c, keep(c, full, strlen(full)), value, raw ? raw : value, line);
  }
  if (depth) complain(c, line, "the file ends inside { }: a } is missing%s", NULL);
}

int uconf_load(uconf *c, const char *path) {
  memset(c, 0, sizeof *c);
  const char *base = strrchr(path, '/');
  snprintf(c->name, sizeof c->name, "%s", strlen(path) < sizeof c->name ? path : base ? base + 1 : path);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  size_t cap = 4096, n = 0;
  char *s = malloc(cap);
  for (size_t r; s && (r = fread(s + n, 1, cap - n - 1, f)) > 0;) {
    n += r;
    if (n + 1 == cap) {
      char *b = cap < (1u << 20) ? realloc(s, cap *= 2) : NULL; /* a dotfile larger than 1 MiB is a mistake */
      if (!b) { free(s); s = NULL; }
      s = b;
    }
  }
  fclose(f);
  if (!s) return -1;
  s[n] = 0;
  uconf_parse(c, s, NULL);
  free(s);
  return 0;
}

void uconf_free(uconf *c) {
  for (int i = 0; i < c->nblocks; i++) free(c->blocks[i]);
  free(c->blocks); free(c->e); free(c->vars);
  memset(c, 0, sizeof *c);
}

/* ---------- readers ---------- */
static uconf_entry *last(uconf *c, const char *key) {
  for (int i = c->n - 1; i >= 0; i--)
    if (!strcmp(c->e[i].key, key)) {
      for (int j = 0; j <= i; j++) if (!strcmp(c->e[j].key, key)) c->e[j].used = 1; /* the earlier ones were overridden */
      return &c->e[i];
    }
  return NULL;
}
const char *uconf_str(uconf *c, const char *key, const char *def) {
  uconf_entry *e = last(c, key);
  return e ? e->value : def;
}
int uconf_int(uconf *c, const char *key, int def, int min, int max) {
  uconf_entry *e = last(c, key);
  if (!e) return def;
  char *end;
  long v = strtol(e->value, &end, 10);
  if (end == e->value || *end) { complain(c, e->line, "'%s' is not a whole number", e->value); return def; }
  if (v < min || v > max) {
    char msg[96];
    snprintf(msg, sizeof msg, "%ld is out of range (%d to %d)", v, min, max);
    complain(c, e->line, "%s", msg);
    return (int)(v < min ? min : max);
  }
  return (int)v;
}
double uconf_num(uconf *c, const char *key, double def) {
  uconf_entry *e = last(c, key);
  if (!e) return def;
  char *end;
  double v = strtod(e->value, &end);
  if (end == e->value || *end) { complain(c, e->line, "'%s' is not a number", e->value); return def; }
  return v;
}
static int same_word(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if ((*a | 32) != (*b | 32)) return 0;
  return !*a && !*b;
}
int uconf_bool(uconf *c, const char *key, int def) {
  uconf_entry *e = last(c, key);
  if (!e) return def;
  static const char *yes[] = {"true", "yes", "on", "1"}, *no[] = {"false", "no", "off", "0"};
  for (int i = 0; i < 4; i++) {
    if (same_word(e->value, yes[i])) return 1;
    if (same_word(e->value, no[i])) return 0;
  }
  complain(c, e->line, "'%s' is not true or false", e->value);
  return def;
}
static int hexval(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  ch |= 32;
  return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}
int uconf_parse_color(const char *s, unsigned *rgb) {
  if (*s == '#') s++;
  else if (s[0] == '0' && (s[1] | 32) == 'x') s += 2;
  size_t n = strlen(s);
  if (n != 6 && n != 3) return -1;
  unsigned v = 0;
  for (size_t i = 0; i < n; i++) {
    int h = hexval((unsigned char)s[i]);
    if (h < 0) return -1;
    v = n == 3 ? v << 8 | (unsigned)(h * 17) : v << 4 | (unsigned)h;
  }
  *rgb = v;
  return 0;
}
unsigned uconf_color(uconf *c, const char *key, unsigned def) {
  uconf_entry *e = last(c, key);
  unsigned v;
  if (!e) return def;
  if (uconf_parse_color(e->value, &v) < 0) { complain(c, e->line, "'%s' is not a colour (#rrggbb)", e->value); return def; }
  return v;
}
int uconf_next(uconf *c, const char *key, int after) {
  for (int i = after + 1; i < c->n; i++)
    if (!strcmp(c->e[i].key, key)) { c->e[i].used = 1; return i; }
  return -1;
}

int uconf_split(char *s, char **out, int max) {
  int n = 0;
  while (n < max) {
    while (*s == ' ' || *s == '\t') s++;
    char *start = s, *comma = n + 1 < max ? strchr(s, ',') : NULL;
    char *end = comma ? comma : s + strlen(s);
    char *e = end;
    while (e > start && (e[-1] == ' ' || e[-1] == '\t')) e--;
    out[n++] = start;
    if (!comma) { *e = 0; break; }
    *e = 0;
    s = comma + 1;
  }
  return n;
}

void uconf_warn_unused(uconf *c) {
  for (int i = 0; i < c->n; i++)
    if (!c->e[i].used) complain(c, c->e[i].line, "unknown setting '%s' (ignored)", c->e[i].key);
}
