// SPDX-License-Identifier: GPL-2.0-or-later
/* apt.conf(5) style configuration: a tree of "::"-separated keys.
 *   APT::Install-Recommends "false";
 *   Acquire::http { Command "curl -fsSL -o %o %u"; };
 *   APT::NeverAutoRemove { "^linux-.*"; };      (a list: anonymous values)
 * Comments: "#", "//" and C style. "#clear KEY" empties a key and its children,
 * "#include FILE" reads another file. Keys compare without regard to case.
 * Later settings win; lists keep every value. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

typedef struct { char *k, *v; } kv;
static kv *items;
static int nitems, capitems;

void aptconf_set(const char *key, const char *val) {
  size_t kl = strlen(key);
  int is_list = kl >= 2 && !strcmp(key + kl - 2, "::");
  if (!is_list)
    for (int i = nitems - 1; i >= 0; i--)
      if (!strcasecmp(items[i].k, key)) { free(items[i].v); items[i].v = xstrdup(val); return; }
  if (nitems == capitems) { capitems = capitems ? capitems * 2 : 64; items = xrealloc(items, capitems * sizeof *items); }
  items[nitems].k = xstrdup(key);
  items[nitems].v = xstrdup(val);
  nitems++;
}
const char *aptconf_get(const char *key, const char *dflt) {
  for (int i = nitems - 1; i >= 0; i--) if (!strcasecmp(items[i].k, key)) return items[i].v;
  return dflt;
}
int aptconf_bool(const char *key, int dflt) {
  const char *v = aptconf_get(key, 0);
  if (!v) return dflt;
  if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "enable")) return 1;
  if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0") || !strcasecmp(v, "disable")) return 0;
  return dflt;
}
void aptconf_list(const char *prefix, strlist *out) {
  char *k = xasprintf("%s::", prefix);
  for (int i = 0; i < nitems; i++) if (!strcasecmp(items[i].k, k)) sl_add(out, items[i].v);
  free(k);
}
static void clear(const char *key) {
  size_t n = strlen(key);
  int j = 0;
  for (int i = 0; i < nitems; i++) {
    if (!strncasecmp(items[i].k, key, n) && (items[i].k[n] == 0 || (items[i].k[n] == ':' && items[i].k[n + 1] == ':'))) { free(items[i].k); free(items[i].v); continue; }
    items[j++] = items[i];
  }
  nitems = j;
}
void aptconf_dump(sbuf *out) {
  for (int i = 0; i < nitems; i++) sb_printf(out, "%s \"%s\";\n", items[i].k, items[i].v);
}

/* ---- parser ---- */
typedef struct { const char *p, *origin; int line; int err; } lexer;
static void skip(lexer *L) {
  for (;;) {
    while (*L->p == ' ' || *L->p == '\t' || *L->p == '\r' || *L->p == '\n') { if (*L->p == '\n') L->line++; L->p++; }
    if (L->p[0] == '/' && L->p[1] == '/') { while (*L->p && *L->p != '\n') L->p++; continue; }
    if (L->p[0] == '/' && L->p[1] == '*') { L->p += 2; while (*L->p && !(L->p[0] == '*' && L->p[1] == '/')) { if (*L->p == '\n') L->line++; L->p++; } if (*L->p) L->p += 2; continue; }
    if (L->p[0] == '#' && strncmp(L->p, "#clear", 6) && strncmp(L->p, "#include", 8)) { while (*L->p && *L->p != '\n') L->p++; continue; }
    return;
  }
}
/* A token: a quoted string (quoted=1) or a bare word; NULL at end or on a single-char token. */
static char *token(lexer *L, int *quoted) {
  skip(L);
  *quoted = 0;
  if (*L->p == '"') {
    const char *s = ++L->p;
    while (*L->p && *L->p != '"') { if (*L->p == '\n') L->line++; L->p++; }
    char *t = xstrndup(s, L->p - s);
    if (*L->p) L->p++;
    *quoted = 1;
    return t;
  }
  const char *s = L->p;
  while (*L->p && !strchr(" \t\r\n{};\"", *L->p)) L->p++;
  if (L->p == s) return 0;
  return xstrndup(s, L->p - s);
}
static int parse_block(lexer *L, const char *prefix);
static void bad(lexer *L, const char *what) {
  pkg_error("%s:%d: syntax error: %s", L->origin, L->line, what);
  L->err = 1;
}
static int parse_block(lexer *L, const char *prefix) {
  for (;;) {
    skip(L);
    if (!*L->p) return 0;
    if (*L->p == '}') { L->p++; skip(L); if (*L->p == ';') L->p++; return 0; }
    if (*L->p == ';') { L->p++; continue; }
    if (!strncmp(L->p, "#clear", 6)) {
      L->p += 6;
      int q; char *k = token(L, &q);
      if (k) { char *full = *prefix ? xasprintf("%s::%s", prefix, k) : xstrdup(k); clear(full); free(full); free(k); }
      skip(L); if (*L->p == ';') L->p++;
      continue;
    }
    if (!strncmp(L->p, "#include", 8)) {
      L->p += 8;
      int q; char *f = token(L, &q);
      if (f) { aptconf_parse_file(f); free(f); }
      skip(L); if (*L->p == ';') L->p++;
      continue;
    }
    int q1;
    char *a = token(L, &q1);
    if (!a) { bad(L, "unexpected character"); return -1; }
    skip(L);
    if (q1 && (*L->p == ';' || *L->p == '}')) {
      /* anonymous value: a list item */
      char *k = xasprintf("%s::", prefix);
      aptconf_set(k, a);
      free(k); free(a);
      if (*L->p == ';') L->p++;
      continue;
    }
    char *key = *prefix ? xasprintf("%s::%s", prefix, a) : xstrdup(a);
    free(a);
    if (*L->p == '{') {
      L->p++;
      if (parse_block(L, key) < 0) { free(key); return -1; }
      free(key);
      continue;
    }
    int q2;
    char *v = token(L, &q2);
    if (!v) { bad(L, "missing value"); free(key); return -1; }
    skip(L);
    if (*L->p == '{') {
      /* "Key value { ... }" is not apt syntax; treat as error */
      bad(L, "unexpected '{'"); free(key); free(v); return -1;
    }
    aptconf_set(key, v);
    free(key); free(v);
    if (*L->p == ';') L->p++;
    else if (*L->p && *L->p != '}') { bad(L, "missing ';'"); return -1; }
  }
}
int aptconf_parse_text(const char *text, const char *origin) {
  lexer L = {text, origin, 1, 0};
  parse_block(&L, "");
  return L.err ? -1 : 0;
}
int aptconf_parse_file(const char *path) {
  char *data;
  if (file_read(path, &data, 0) < 0) return -2;
  int r = aptconf_parse_text(data, path);
  free(data);
  return r;
}

/* apt's directory keys: each is relative to its parent unless absolute. */
char *apt_dir(const char *key, const char *dflt) {
  const char *v = aptconf_get(key, dflt);
  if (!v) v = "";
  if (v[0] == '/' && strcasecmp(key, "Dir")) return xstrdup(v);
  if (!strcasecmp(key, "Dir")) return xstrdup(v && *v ? v : "/");
  const char *last = strrchr(key, ':');
  char parent[256];
  snprintf(parent, sizeof parent, "%.*s", (int)(last - key - 1), key);
  static const struct { const char *k, *d; } defaults[] = {
    {"Dir::State", "var/lib/apt"}, {"Dir::Cache", "var/cache/apt"}, {"Dir::Etc", "etc/apt"}, {"Dir::Log", "var/log/apt"}, {0, 0}};
  const char *pd = 0;
  for (int i = 0; defaults[i].k; i++) if (!strcasecmp(defaults[i].k, parent)) pd = defaults[i].d;
  char *base = apt_dir(parent, pd);
  char *r = path_join(base, v);
  free(base);
  return r;
}
