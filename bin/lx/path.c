// SPDX-License-Identifier: GPL-2.0-or-later
/* Path translation: see path.h. Tested on its own by tests/path_test.c. */
#include "path.h"
#include "lxlib.h"
#include <errno.h>

#define AT_FDCWD_ (-100)
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

static char root[LX_PATH_MAX];
static size_t rootlen; /* 0 means pass-through ("/") */
static char cwd[LX_PATH_MAX] = "/";
static const struct path_ops *ops;

int path_init(const char *r, const struct path_ops *o) {
  ops = o;
  size_t n = strlen(r);
  while (n > 1 && r[n - 1] == '/') n--;
  if (n >= sizeof root) return -ENAMETOOLONG;
  if (n == 1 && r[0] == '/') { rootlen = 0; root[0] = 0; }
  else { memcpy(root, r, n); root[n] = 0; rootlen = n; }
  strcpy(cwd, "/");
  return 0;
}
int path_is_rooted(void) { return rootlen != 0; }
const char *path_root(void) { return rootlen ? root : "/"; }
const char *path_cwd(void) { return cwd; }
void path_set_cwd(const char *g) {
  size_t n = strlen(g);
  if (n < sizeof cwd) memcpy(cwd, g, n + 1);
}

/* A path built as a stack of components inside a fixed buffer. */
struct pbuf { char s[LX_PATH_MAX]; size_t n; };
static int push(struct pbuf *p, const char *c, size_t len) {
  if (p->n + 1 + len + 1 > sizeof p->s) return -ENAMETOOLONG;
  p->s[p->n++] = '/';
  memcpy(p->s + p->n, c, len);
  p->n += len;
  p->s[p->n] = 0;
  return 0;
}
static void pop(struct pbuf *p) {
  while (p->n > 0 && p->s[p->n - 1] != '/') p->n--;
  if (p->n > 0) p->n--;
  p->s[p->n] = 0;
}
static int is_passthrough(const char *g) {
  return !strncmp(g, "/dev", 4) && (g[4] == 0 || g[4] == '/') ? 1 :
         !strncmp(g, "/proc", 5) && (g[5] == 0 || g[5] == '/') ? 1 : 0;
}
static int host_of(const char *g, char *out, size_t cap) {
  if (is_passthrough(g)) { if (strlen(g) + 1 > cap) return -ENAMETOOLONG; strcpy(out, g); return 0; }
  size_t n = strlen(g);
  if (rootlen + n + 1 > cap) return -ENAMETOOLONG;
  memcpy(out, root, rootlen);
  memcpy(out + rootlen, g, n + 1);
  if (rootlen + n == rootlen) { out[rootlen] = '/'; out[rootlen + 1] = 0; }
  return 0;
}

int64_t path_resolve(int dirfd, const char *g, int follow, char *out, size_t cap) {
  struct pbuf cur;
  cur.n = 0; cur.s[0] = 0;
  if (!*g) return -ENOENT;
  /* pending: the components still to walk, as one string we consume from the front */
  static char pend[2][LX_PATH_MAX];
  int pi = 0;
  if (strlen(g) >= LX_PATH_MAX) return -ENAMETOOLONG;
  strcpy(pend[pi], g);
  if (g[0] != '/') {
    const char *base = cwd;
    if (dirfd != AT_FDCWD_) {
      base = ops && ops->fd_path ? ops->fd_path(dirfd) : 0;
      if (!base) return -EBADF;
    }
    size_t bn = strlen(base);
    if (bn >= sizeof cur.s) return -ENAMETOOLONG;
    memcpy(cur.s, base, bn + 1);
    cur.n = bn;
    while (cur.n > 0 && cur.s[cur.n - 1] == '/') cur.s[--cur.n] = 0;
  }
  int links = 0;
  const char *p = pend[pi];
  for (;;) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != '/') e++;
    size_t len = (size_t)(e - p);
    const char *next = e;
    while (*next == '/') next++;
    int last = *next == 0;
    if (len == 1 && p[0] == '.') { p = e; continue; }
    if (len == 2 && p[0] == '.' && p[1] == '.') { pop(&cur); p = e; continue; }
    int r = push(&cur, p, len);
    if (r < 0) return r;
    p = e;
    if (rootlen && ops && (follow || !last) && !is_passthrough(cur.s)) {
      char hp[LX_PATH_MAX];
      if (host_of(cur.s, hp, sizeof hp) < 0) return -ENAMETOOLONG;
      int k = ops->kind(hp);
      if (k == 1) {
        if (++links > 40) return -ELOOP;
        char tgt[LX_PATH_MAX];
        int64_t tl = ops->readlink(hp, tgt, sizeof tgt);
        if (tl < 0) return tl;
        pop(&cur);
        if (tgt[0] == '/') { cur.n = 0; cur.s[0] = 0; }
        /* new pending = target + "/" + rest */
        int pj = pi ^ 1;
        size_t rl = strlen(p);
        if ((size_t)tl + 1 + rl + 1 > LX_PATH_MAX) return -ENAMETOOLONG;
        memcpy(pend[pj], tgt, (size_t)tl);
        pend[pj][tl] = '/';
        memcpy(pend[pj] + tl + 1, p, rl + 1);
        pi = pj;
        p = pend[pi];
      } else if (k < 0 && !last) {
        /* a missing directory: let the host call report it; stop translating links */
        size_t rl = strlen(p);
        if (cur.n + rl + 1 > sizeof cur.s) return -ENAMETOOLONG;
        memcpy(cur.s + cur.n, p, rl + 1);
        cur.n += rl;
        break;
      }
    }
  }
  if (cur.n == 0) { cur.s[0] = '/'; cur.s[1] = 0; cur.n = 1; }
  if (cur.n + 1 > cap) return -ENAMETOOLONG;
  memcpy(out, cur.s, cur.n + 1);
  return 0;
}

int64_t path_xlate(int dirfd, const char *g, int follow, char *out, size_t cap, int *hostdirfd) {
  if (!rootlen) {
    size_t n = strlen(g);
    if (n + 1 > cap) return -ENAMETOOLONG;
    memcpy(out, g, n + 1);
    *hostdirfd = dirfd;
    return 0;
  }
  char gp[LX_PATH_MAX];
  int64_t r = path_resolve(dirfd, g, follow, gp, sizeof gp);
  if (r < 0) return r;
  *hostdirfd = AT_FDCWD_;
  return host_of(gp, out, cap);
}

int64_t path_to_host(int dirfd, const char *g, int follow, char *out, size_t cap) {
  int hd;
  int64_t r = path_xlate(dirfd, g, follow, out, cap, &hd);
  if (r < 0) return r;
  if (hd != AT_FDCWD_ && out[0] != '/') {
    const char *base = ops && ops->fd_path ? ops->fd_path(hd) : 0;
    if (!base) return -EBADF;
    char tmp[LX_PATH_MAX];
    if (strlen(base) + 1 + strlen(out) + 1 > sizeof tmp) return -ENAMETOOLONG;
    snprintf(tmp, sizeof tmp, "%s/%s", base, out);
    strcpy(out, tmp);
  }
  return 0;
}

int64_t path_from_host(const char *h, char *out, size_t cap) {
  const char *g = h;
  if (rootlen) {
    if (strncmp(h, root, rootlen) || (h[rootlen] != 0 && h[rootlen] != '/')) {
      if (is_passthrough(h)) g = h; else g = 0;
    } else {
      g = h + rootlen;
      if (!*g) g = "/";
    }
  }
  if (!g) return -ENOENT;
  size_t n = strlen(g);
  if (n + 1 > cap) return -ENAMETOOLONG;
  memcpy(out, g, n + 1);
  return 0;
}
