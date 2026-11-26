// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit file syntax (systemd.syntax(7)): sections, key = value, comments, line
 * continuation; values: booleans, time spans, word splitting with quotes and escapes,
 * and command lines with their prefixes and variable expansion (systemd.service(5),
 * "Command lines"). Written from the published documentation. */
#include "svc.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *xstrndup(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) return 0;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}
static void add_kv(struct svc_file *f, const char *sec, const char *key, size_t kl, const char *val, size_t vl,
                   const char *src, int line) {
  if (f->n == f->cap) {
    int nc = f->cap ? f->cap * 2 : 32;
    struct svc_kv *nk = realloc(f->kv, nc * sizeof *nk);
    if (!nk) return;
    f->kv = nk; f->cap = nc;
  }
  struct svc_kv *e = &f->kv[f->n];
  /* sections are few: share the string with the previous entry when equal */
  if (f->n && !strcmp(f->kv[f->n - 1].sec, sec)) e->sec = f->kv[f->n - 1].sec;
  else e->sec = strdup(sec);
  e->key = xstrndup(key, kl);
  e->val = xstrndup(val, vl);
  e->src = src; e->line = line;
  if (!e->sec || !e->key || !e->val) return;
  f->n++;
}
static void warnf(svc_warn_fn w, void *ctx, const char *src, int line, const char *fmt, const char *arg) {
  if (!w) return;
  char m[200];
  snprintf(m, sizeof m, fmt, arg);
  w(ctx, src, line, m);
}

int svc_parse(struct svc_file *f, const char *text, size_t len, const char *src0, svc_warn_fn warn, void *ctx) {
  char **ns = realloc(f->srcs, (f->nsrc + 1) * sizeof *ns);
  if (!ns) return 1;
  f->srcs = ns;
  char *src = strdup(src0 ? src0 : "?");
  f->srcs[f->nsrc++] = src;
  int bad = 0, lineno = 0;
  char sec[64] = "";
  /* a logical line can span several physical lines; build it here */
  size_t cap = 256, ll = 0;
  char *lbuf = malloc(cap);
  int startline = 0, cont = 0;
  const char *p = text, *end = text + len;
  while (p < end || cont) {
    const char *eol = p;
    while (eol < end && *eol != '\n') eol++;
    lineno++;
    const char *s = p, *e = eol;
    p = eol < end ? eol + 1 : end;
    if (e > s && e[-1] == '\r') e--;
    while (s < e && isspace((unsigned char)*s)) s++;
    if (cont && s < e && (*s == '#' || *s == ';')) continue; /* comments inside a continued line are dropped */
    if (!cont) {
      if (s == e || *s == '#' || *s == ';') { if (p >= end && eol >= end) break; continue; }
      ll = 0; startline = lineno;
    }
    int more = e > s && e[-1] == '\\';
    if (more) e--;
    size_t n = e - s;
    if (ll + n + 2 > cap) { while (ll + n + 2 > cap) cap *= 2; char *nb = realloc(lbuf, cap); if (!nb) break; lbuf = nb; }
    memcpy(lbuf + ll, s, n); ll += n;
    if (more && p < end) { lbuf[ll++] = ' '; cont = 1; continue; }
    cont = 0;
    lbuf[ll] = 0;
    char *l = lbuf;
    if (*l == '[') {
      char *r = strchr(l, ']');
      if (!r || r[1]) { warnf(warn, ctx, src, startline, "bad section header: %s", l); bad++; sec[0] = 0; continue; }
      size_t sl = r - l - 1;
      if (sl >= sizeof sec) sl = sizeof sec - 1;
      memcpy(sec, l + 1, sl); sec[sl] = 0;
      continue;
    }
    char *eq = strchr(l, '=');
    if (!eq) { warnf(warn, ctx, src, startline, "line is not an assignment: %s", l); bad++; continue; }
    if (!sec[0]) { warnf(warn, ctx, src, startline, "assignment outside a section: %s", l); bad++; continue; }
    char *k = l, *ke = eq;
    while (ke > k && isspace((unsigned char)ke[-1])) ke--;
    char *v = eq + 1;
    while (*v && isspace((unsigned char)*v)) v++;
    char *ve = v + strlen(v);
    while (ve > v && isspace((unsigned char)ve[-1])) ve--;
    if (ke == k) { warnf(warn, ctx, src, startline, "empty key: %s", l); bad++; continue; }
    add_kv(f, sec, k, ke - k, v, ve - v, src, startline);
    if (p >= end && eol >= end) break;
  }
  free(lbuf);
  return bad;
}

int svc_parse_path(struct svc_file *f, const char *path, svc_warn_fn warn, void *ctx) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  for (;;) {
    if (n == cap) { char *nb = realloc(b, cap * 2); if (!nb) break; b = nb; cap *= 2; }
    ssize_t r = read(fd, b + n, cap - n);
    if (r <= 0) break;
    n += r;
  }
  close(fd);
  int bad = svc_parse(f, b, n, path, warn, ctx);
  free(b);
  return bad;
}

void svc_file_free(struct svc_file *f) {
  for (int i = 0; i < f->n; i++) {
    if (!i || f->kv[i].sec != f->kv[i - 1].sec) free(f->kv[i].sec);
    free(f->kv[i].key); free(f->kv[i].val);
  }
  for (int i = 0; i < f->nsrc; i++) free(f->srcs[i]);
  free(f->kv); free(f->srcs);
  memset(f, 0, sizeof *f);
}

const char *svc_get(const struct svc_file *f, const char *sec, const char *key) {
  for (int i = f->n - 1; i >= 0; i--)
    if (!strcmp(f->kv[i].key, key) && !strcmp(f->kv[i].sec, sec)) return f->kv[i].val;
  return 0;
}
int svc_getall(const struct svc_file *f, const char *sec, const char *key, const char **out, int max) {
  int n = 0;
  for (int i = 0; i < f->n; i++) {
    if (strcmp(f->kv[i].key, key) || strcmp(f->kv[i].sec, sec)) continue;
    if (!f->kv[i].val[0]) { n = 0; continue; }
    if (n < max) out[n] = f->kv[i].val;
    n++;
  }
  return n < max ? n : max;
}
int svc_has_section(const struct svc_file *f, const char *sec) {
  for (int i = 0; i < f->n; i++) if (!strcmp(f->kv[i].sec, sec)) return 1;
  return 0;
}

int svc_bool(const char *s, int dflt) {
  if (!s) return dflt;
  static const char *const yes[] = {"1", "yes", "y", "true", "t", "on", 0}, *const no[] = {"0", "no", "n", "false", "f", "off", 0};
  for (int i = 0; yes[i]; i++) if (!strcasecmp(s, yes[i])) return 1;
  for (int i = 0; no[i]; i++) if (!strcasecmp(s, no[i])) return 0;
  return dflt;
}

/* "1min 30s", "100ms", "5", "infinity". A bare number uses default_unit_ms. */
int svc_timespan(const char *s, int64_t *ms, int64_t unit) {
  static const struct { const char *n; int64_t ms; } u[] = {
    {"usec", 0}, {"us", 0}, {"µs", 0}, {"msec", 1}, {"ms", 1}, {"seconds", 1000}, {"second", 1000}, {"sec", 1000},
    {"s", 1000}, {"minutes", 60000}, {"minute", 60000}, {"min", 60000}, {"m", 60000}, {"hours", 3600000},
    {"hour", 3600000}, {"hr", 3600000}, {"h", 3600000}, {"days", 86400000}, {"day", 86400000}, {"d", 86400000},
    {"weeks", 604800000}, {"week", 604800000}, {"w", 604800000}, {"months", 2629800000LL}, {"month", 2629800000LL},
    {"M", 2629800000LL}, {"years", 31557600000LL}, {"year", 31557600000LL}, {"y", 31557600000LL}, {0, 0}};
  while (isspace((unsigned char)*s)) s++;
  if (!strcmp(s, "infinity")) { *ms = SVC_INFINITY; return 0; }
  if (!*s) return -1;
  int64_t total = 0;
  while (*s) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) break;
    if (!isdigit((unsigned char)*s) && *s != '.') return -1;
    double v = 0, frac = 0.1;
    while (isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (isdigit((unsigned char)*s)) { v += (*s++ - '0') * frac; frac /= 10; } }
    while (*s == ' ') s++;
    const char *w = s;
    while (*s && !isspace((unsigned char)*s) && !isdigit((unsigned char)*s)) s++;
    size_t wl = s - w;
    int64_t m = unit;
    if (wl) {
      int i;
      for (i = 0; u[i].n; i++) if (strlen(u[i].n) == wl && !strncmp(u[i].n, w, wl)) break;
      if (!u[i].n) return -1;
      m = u[i].ms;
      if (!m) { total += (int64_t)(v / 1000); continue; }
    }
    total += (int64_t)(v * m + 0.5);
  }
  *ms = total;
  return 0;
}

/* One escape after a backslash; returns bytes consumed after the backslash. */
static int unescape1(const char *s, char *out, int *outn) {
  int n = 0;
  switch (*s) {
  case 'a': out[n++] = '\a'; break;
  case 'b': out[n++] = '\b'; break;
  case 'f': out[n++] = '\f'; break;
  case 'n': out[n++] = '\n'; break;
  case 'r': out[n++] = '\r'; break;
  case 't': out[n++] = '\t'; break;
  case 'v': out[n++] = '\v'; break;
  case 's': out[n++] = ' '; break;
  case 'x': {
    int v = 0, k;
    for (k = 1; k <= 2 && isxdigit((unsigned char)s[k]); k++) v = v * 16 + (isdigit((unsigned char)s[k]) ? s[k] - '0' : (tolower((unsigned char)s[k]) - 'a' + 10));
    out[n++] = (char)v;
    *outn = n;
    return k;
  }
  case '0': case '1': case '2': case '3': {
    int v = 0, k;
    for (k = 0; k < 3 && s[k] >= '0' && s[k] <= '7'; k++) v = v * 8 + (s[k] - '0');
    out[n++] = (char)v;
    *outn = n;
    return k;
  }
  default: out[n++] = *s; break; /* \\ \" \' \; \$ and anything else: the character itself */
  }
  *outn = n;
  return 1;
}

struct sbuf { char *b; size_t n, cap; };
static void sb_put(struct sbuf *s, const char *p, size_t n) {
  if (s->n + n + 1 > s->cap) {
    size_t nc = s->cap ? s->cap : 64;
    while (s->n + n + 1 > nc) nc *= 2;
    char *nb = realloc(s->b, nc);
    if (!nb) return;
    s->b = nb; s->cap = nc;
  }
  memcpy(s->b + s->n, p, n);
  s->n += n;
  s->b[s->n] = 0;
}

int svc_words(const char *s, char ***out) {
  char **w = 0;
  int n = 0, cap = 0;
  for (;;) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) break;
    struct sbuf b = {0};
    sb_put(&b, "", 0);
    char q = 0;
    while (*s && (q || !isspace((unsigned char)*s))) {
      if (!q && (*s == '"' || *s == '\'')) { q = *s++; continue; }
      if (q && *s == q) { q = 0; s++; continue; }
      if (*s == '\\' && s[1]) { char e[4]; int en; s += 1 + unescape1(s + 1, e, &en); sb_put(&b, e, en); continue; }
      sb_put(&b, s, 1); s++;
    }
    if (q) { free(b.b); svc_free_words(w, n); return -1; }
    if (n + 1 >= cap) { cap = cap ? cap * 2 : 8; w = realloc(w, cap * sizeof *w); }
    w[n++] = b.b;
  }
  if (!w) w = calloc(1, sizeof *w);
  else w[n] = 0;
  *out = w;
  return n;
}
void svc_free_words(char **w, int n) {
  if (!w) return;
  for (int i = 0; i < n; i++) free(w[i]);
  free(w);
}

/* Command lines. Each word may be: quoted parts, escapes, "${VAR}" anywhere (exact
 * value, never split), or "$VAR" as a whole word (split at whitespace into zero or
 * more words). "$$" is a dollar. A lone ";" ends a command. */
int svc_cmdline(const char *s, svc_env_fn env, void *ctx, struct svc_cmd **out, char *err, size_t errlen) {
  struct svc_cmd *cmds = 0;
  int nc = 0;
  for (;;) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) break;
    int flags = 0;
    for (;; s++) {
      if (*s == '-') flags |= SVC_EX_IGNORE;
      else if (*s == '@') flags |= SVC_EX_ARGV0;
      else if (*s == ':') flags |= SVC_EX_NOENV;
      else if (*s == '+' || *s == '!') flags |= SVC_EX_PRIV;
      else break;
    }
    char **w = 0;
    int n = 0, cap = 0, ended = 0;
    for (;;) {
      while (isspace((unsigned char)*s)) s++;
      if (!*s) break;
      if (*s == ';' && (!s[1] || isspace((unsigned char)s[1]))) { s++; ended = 1; break; }
      /* a whole-word $VAR */
      if (!(flags & SVC_EX_NOENV) && *s == '$' && (isalpha((unsigned char)s[1]) || s[1] == '_')) {
        const char *v = s + 1, *ve = v;
        while (isalnum((unsigned char)*ve) || *ve == '_') ve++;
        if (!*ve || isspace((unsigned char)*ve)) {
          char name[128];
          snprintf(name, sizeof name, "%.*s", (int)(ve - v), v);
          const char *val = env ? env(ctx, name) : 0;
          s = ve;
          if (val) {
            char **ws;
            int k = svc_words(val, &ws);
            /* split at whitespace only: quotes inside the value are kept as they are */
            if (k >= 0) {
              svc_free_words(ws, k);
              const char *q = val;
              for (;;) {
                while (isspace((unsigned char)*q)) q++;
                if (!*q) break;
                const char *qe = q;
                while (*qe && !isspace((unsigned char)*qe)) qe++;
                if (n + 2 >= cap) { cap = cap ? cap * 2 : 8; w = realloc(w, cap * sizeof *w); }
                w[n++] = strndup(q, qe - q);
                q = qe;
              }
            }
          }
          continue;
        }
      }
      struct sbuf b = {0};
      sb_put(&b, "", 0);
      char q = 0;
      while (*s && (q || !isspace((unsigned char)*s))) {
        if (!q && (*s == '"' || *s == '\'')) { q = *s++; continue; }
        if (q && *s == q) { q = 0; s++; continue; }
        if (*s == '\\' && s[1]) { char e[4]; int en; s += 1 + unescape1(s + 1, e, &en); sb_put(&b, e, en); continue; }
        if (*s == '$' && q != '\'' && !(flags & SVC_EX_NOENV)) {
          if (s[1] == '$') { sb_put(&b, "$", 1); s += 2; continue; }
          if (s[1] == '{') {
            const char *ce = strchr(s + 2, '}');
            if (ce) {
              char name[128];
              snprintf(name, sizeof name, "%.*s", (int)(ce - s - 2), s + 2);
              const char *val = env ? env(ctx, name) : 0;
              if (val) sb_put(&b, val, strlen(val));
              s = ce + 1;
              continue;
            }
          }
        }
        sb_put(&b, s, 1); s++;
      }
      if (q) {
        free(b.b); svc_free_words(w, n); svc_cmds_free(cmds, nc);
        if (err) snprintf(err, errlen, "unbalanced quoting");
        return -1;
      }
      if (n + 2 >= cap) { cap = cap ? cap * 2 : 8; w = realloc(w, cap * sizeof *w); }
      w[n++] = b.b;
    }
    if (!n) {
      free(w);
      if (flags || ended) { if (err) snprintf(err, errlen, "empty command"); svc_cmds_free(cmds, nc); return -1; }
      break;
    }
    w[n] = 0;
    struct svc_cmd c = {flags, 0, 0, 0};
    c.path = w[0];
    if (flags & SVC_EX_ARGV0) {
      if (n < 2) { svc_free_words(w, n); svc_cmds_free(cmds, nc); if (err) snprintf(err, errlen, "@ needs argv[0]"); return -1; }
      /* argv = w[1..], path = w[0] */
      c.argv = malloc(n * sizeof *c.argv);
      for (int i = 1; i < n; i++) c.argv[i - 1] = w[i];
      c.argv[n - 1] = 0;
      c.argc = n - 1;
      free(w);
    } else {
      c.argv = w;
      c.argc = n;
      c.path = strdup(w[0]);
    }
    cmds = realloc(cmds, (nc + 1) * sizeof *cmds);
    cmds[nc++] = c;
  }
  *out = cmds;
  return nc;
}
void svc_cmds_free(struct svc_cmd *c, int n) {
  if (!c) return;
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < c[i].argc; k++) free(c[i].argv[k]);
    free(c[i].argv);
    free(c[i].path);
  }
  free(c);
}
