// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit names, specifiers, links (or link files), finding and loading units, and the
 * [Install] logic behind enable, disable, is-enabled and mask (systemd.unit(5),
 * systemctl(1)). */
#include "svc.h"
#include "os.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *const tnames[] = {"service", "socket", "target", "timer", "path", "slice", "mount",
                                     "automount", "device", "swap", "scope"};
int svc_name_type(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return SVC_T_UNKNOWN;
  for (int i = 0; i < SVC_T_UNKNOWN; i++) if (!strcmp(dot + 1, tnames[i])) return i;
  return SVC_T_UNKNOWN;
}
const char *svc_type_name(int t) { return t >= 0 && t < SVC_T_UNKNOWN ? tnames[t] : "unknown"; }
int svc_name_valid(const char *name) {
  size_t n = strlen(name);
  if (!n || n > 255 || svc_name_type(name) == SVC_T_UNKNOWN) return 0;
  for (const char *p = name; *p; p++)
    if (!isalnum((unsigned char)*p) && !strchr(":-_.\\@", *p)) return 0;
  return name[0] != '.';
}
int svc_name_split(const char *name, char *tmpl, size_t tl, char *inst, size_t il) {
  const char *at = strchr(name, '@'), *dot = strrchr(name, '.');
  if (!at || !dot || dot < at) return 0;
  if (tmpl) snprintf(tmpl, tl, "%.*s%s", (int)(at - name + 1), name, dot);
  if (inst) snprintf(inst, il, "%.*s", (int)(dot - at - 1), at + 1);
  return dot == at + 1 ? 2 : 1;
}
void svc_unescape(const char *in, char *out, size_t ol) {
  size_t n = 0;
  for (const char *p = in; *p && n + 1 < ol; p++) {
    if (*p == '-') out[n++] = '/';
    else if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
      char h[3] = {p[2], p[3], 0};
      out[n++] = (char)strtol(h, 0, 16);
      p += 3;
    } else out[n++] = *p;
  }
  out[n] = 0;
}

char *svc_specifiers(const char *in, const struct svc_spec *sp, int *bad) {
  size_t cap = strlen(in) + 64, n = 0;
  char *o = malloc(cap);
  char prefix[256] = "", inst[256] = "", full[256], tmp[512];
  const char *name = sp && sp->name ? sp->name : "";
  snprintf(full, sizeof full, "%s", name);
  const char *dot = strrchr(full, '.');
  char noext[256];
  snprintf(noext, sizeof noext, "%.*s", dot ? (int)(dot - full) : (int)strlen(full), full);
  const char *at = strchr(noext, '@');
  if (at) { snprintf(prefix, sizeof prefix, "%.*s", (int)(at - noext), noext); snprintf(inst, sizeof inst, "%s", at + 1); }
  else snprintf(prefix, sizeof prefix, "%s", noext);
  for (const char *p = in; *p; p++) {
    const char *v = 0;
    if (*p == '%' && p[1]) {
      p++;
      switch (*p) {
      case '%': v = "%"; break;
      case 'n': v = full; break;
      case 'N': v = noext; break;
      case 'p': v = prefix; break;
      case 'P': svc_unescape(prefix, tmp, sizeof tmp); v = tmp; break;
      case 'i': v = inst; break;
      case 'I': svc_unescape(inst, tmp, sizeof tmp); v = tmp; break;
      case 'j': { const char *d = strrchr(prefix, '-'); v = d ? d + 1 : prefix; break; }
      case 'J': { const char *d = strrchr(prefix, '-'); svc_unescape(d ? d + 1 : prefix, tmp, sizeof tmp); v = tmp; break; }
      case 'f': { char u[256]; svc_unescape(inst[0] ? inst : prefix, u, sizeof u); snprintf(tmp, sizeof tmp, "%s%s", u[0] == '/' ? "" : "/", u); v = tmp; break; }
      case 't': v = "/run"; break;
      case 'S': v = "/var/lib"; break;
      case 'C': v = "/var/cache"; break;
      case 'L': v = "/var/log"; break;
      case 'E': v = "/etc"; break;
      case 'T': case 'V': v = "/tmp"; break;
      case 'h': v = sp && sp->home ? sp->home : "/root"; break;
      case 'u': case 'g': v = sp && sp->user ? sp->user : "root"; break;
      case 'U': case 'G': snprintf(tmp, sizeof tmp, "%d", sp ? sp->uid : 0); v = tmp; break;
      case 'H': case 'l': v = sp && sp->host ? sp->host : "localhost"; break;
      case 'v': v = sp && sp->release ? sp->release : ""; break;
      case 's': v = "/bin/sh"; break;
      default:
        if (bad) (*bad)++;
        tmp[0] = '%'; tmp[1] = *p; tmp[2] = 0; v = tmp;
      }
    }
    size_t vl = v ? strlen(v) : 1;
    if (n + vl + 1 > cap) { cap = (n + vl + 1) * 2; o = realloc(o, cap); }
    if (v) { memcpy(o + n, v, vl); n += vl; }
    else o[n++] = *p;
  }
  o[n] = 0;
  return o;
}

/* ---------- links ---------- */
int svc_have_symlinks(void) {
  static int have = -1;
  if (have < 0) { char b[4]; have = svc_readlink_raw("/", b, sizeof b) != -ENOSYS; }
  return have;
}
int svc_readlink(const char *path, char *buf, size_t len) {
  int r = svc_readlink_raw(path, buf, (int)len - 1);
  if (r >= 0) { buf[r] = 0; return 1; }
  struct stat st;
  if (stat(path, &st) < 0) return -1;
  if (!S_ISREG(st.st_mode) || st.st_size < 2 || st.st_size > 255) return 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  char b[256];
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n < 2 || b[0] != '/' || b[n - 1] != '\n') return 0;
  b[n - 1] = 0;
  for (ssize_t i = 0; i < n - 1; i++) if (b[i] == '\n' || b[i] == '[' || b[i] == '=') return 0;
  snprintf(buf, len, "%s", b);
  return 1;
}
int svc_mkdir_p(const char *path) {
  char b[512];
  snprintf(b, sizeof b, "%s", path);
  for (char *p = b + 1; *p; p++)
    if (*p == '/') { *p = 0; mkdir(b, 0755); *p = '/'; }
  if (mkdir(b, 0755) < 0 && errno != EEXIST) return -errno;
  return 0;
}
int svc_makelink(const char *target, const char *path) {
  char dir[512];
  snprintf(dir, sizeof dir, "%s", path);
  char *sl = strrchr(dir, '/');
  if (sl && sl != dir) { *sl = 0; svc_mkdir_p(dir); }
  unlink(path);
  int r = svc_symlink(target, path);
  if (r != -ENOSYS && r != -EPERM && r != -EOPNOTSUPP) return r;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -errno;
  char b[300];
  int n = snprintf(b, sizeof b, "%s\n", target);
  int ok = write(fd, b, n) == n;
  close(fd);
  return ok ? 0 : -EIO;
}

/* ---------- finding and loading ---------- */
const char *const svc_unit_dirs[] = {"/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system",
                                     "/usr/lib/systemd/system", "/lib/systemd/system", 0};

const char *svc_unit_dir(const struct svc_roots *r, int ri, int k, char *buf, size_t bl) {
  if (ri == 0 && r->extra[0]) {
    if (k == 0) { snprintf(buf, bl, "%s", r->extra); return buf; }
    k--;
  }
  for (int i = 0; i <= k; i++) if (!svc_unit_dirs[i]) return 0;
  snprintf(buf, bl, "%s%s", r->root[ri], svc_unit_dirs[k]);
  return buf;
}

void svc_roots_default(struct svc_roots *r, const char *deb) {
  memset(r, 0, sizeof *r);
  r->n = 1;
  if (!deb) deb = "/debian";
  struct stat st;
  if (deb[0] && stat(deb, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(r->root[1], sizeof r->root[1], "%s", deb); r->n = 2; }
  const char *up = getenv("SYSTEMD_UNIT_PATH"); /* one extra directory, searched first */
  if (up && up[0] == '/') { snprintf(r->extra, sizeof r->extra, "%s", up); char *c = strchr(r->extra, ':'); if (c) *c = 0; }
}
void svc_host_path(const struct svc_roots *r, int root, const char *p, char *out, size_t ol) {
  if (root > 0 && p[0] == '/') snprintf(out, ol, "%s%s", r->root[root], p);
  else snprintf(out, ol, "%s", p);
}
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }


int svc_find(const struct svc_roots *r, const char *name, char *path, size_t pl, int *rootp, char *real, size_t rl) {
  char p[512], t[512];
  for (int pass = 0; pass < 2; pass++) {
    char tmpl[256];
    const char *look = name;
    if (pass == 1) { if (svc_name_split(name, tmpl, sizeof tmpl, 0, 0) != 1) break; look = tmpl; }
    for (int ri = 0; ri < r->n; ri++)
      for (int d = 0; ; d++) {
        char ud[300];
        if (!svc_unit_dir(r, ri, d, ud, sizeof ud)) break;
        snprintf(p, sizeof p, "%s/%s", ud, look);
        int l = svc_readlink(p, t, sizeof t);
        if (l < 0) continue;
        if (l == 1) {
          if (!strcmp(t, "/dev/null")) { if (rootp) *rootp = ri; snprintf(path, pl, "%s", p); if (real) snprintf(real, rl, "%s", name); return 1; }
          /* follow (a few levels): an alias or an enabled copy */
          char hp[512];
          for (int k = 0; k < 4 && l == 1; k++) {
            if (t[0] == '/') snprintf(hp, sizeof hp, "%s%s", r->root[ri], t); /* link targets are relative to their root */
            else { snprintf(hp, sizeof hp, "%s/%s", ud, t); }
            snprintf(p, sizeof p, "%s", hp);
            l = svc_readlink(p, t, sizeof t);
          }
          if (l != 0) continue; /* dangling */
          snprintf(path, pl, "%s", p);
          if (rootp) *rootp = ri;
          if (real) {
            const char *b = base(p);
            /* an alias of a template keeps the instance */
            char inst[256];
            if (pass == 1 && svc_name_split(name, 0, 0, inst, sizeof inst) == 1 && svc_name_split(b, 0, 0, 0, 0) == 2) {
              const char *at = strchr(b, '@');
              snprintf(real, rl, "%.*s%s%s", (int)(at - b + 1), b, inst, strrchr(b, '.'));
            } else snprintf(real, rl, "%s", pass == 1 ? name : b);
          }
          return 0;
        }
        snprintf(path, pl, "%s", p);
        if (rootp) *rootp = ri;
        if (real) snprintf(real, rl, "%s", name);
        return 0;
      }
  }
  return -ENOENT;
}

static int cmpstr(const void *a, const void *b) { return strcmp(base(*(char *const *)a), base(*(char *const *)b)); }

/* Collect DIR/SUB/ *.conf of every unit directory of a root; a file name seen in a
 * higher priority directory hides the same name further down. */
static int dropins(const struct svc_roots *r, int ri, const char *sub, char ***list, int n) {
  char d[512];
  char ud[300];
  for (int k = 0; svc_unit_dir(r, ri, k, ud, sizeof ud); k++) {
    snprintf(d, sizeof d, "%s/%s", ud, sub);
    DIR *dir = opendir(d);
    if (!dir) continue;
    struct dirent *de;
    while ((de = readdir(dir))) {
      size_t l = strlen(de->d_name);
      if (l < 6 || strcmp(de->d_name + l - 5, ".conf")) continue;
      int dup = 0;
      for (int i = 0; i < n; i++) if (!strcmp(base((*list)[i]), de->d_name)) dup = 1;
      if (dup) continue;
      *list = realloc(*list, (n + 1) * sizeof **list);
      char full[600];
      snprintf(full, sizeof full, "%s/%s", d, de->d_name);
      (*list)[n++] = strdup(full);
    }
    closedir(dir);
  }
  return n;
}

int svc_load(const struct svc_roots *r, const char *name, struct svc_file *f, char *path, size_t pl, int *rootp,
             svc_warn_fn warn, void *ctx) {
  int ri = 0;
  char real[256];
  int e = svc_find(r, name, path, pl, &ri, real, sizeof real);
  if (rootp) *rootp = ri;
  if (e) return e;
  if (svc_parse_path(f, path, warn, ctx) < 0) return -EIO;
  /* drop-ins: the type's own ("service.d"), the template's, then the unit's (and an
   * alias's real name) */
  char sub[300], tmpl[256];
  const char *names[4];
  int nn = 0;
  static char tdir[32];
  snprintf(tdir, sizeof tdir, "%s", svc_type_name(svc_name_type(name)));
  names[nn++] = tdir;
  if (svc_name_split(name, tmpl, sizeof tmpl, 0, 0) == 1) names[nn++] = tmpl;
  names[nn++] = name;
  if (strcmp(real, name)) names[nn++] = real;
  for (int i = 0; i < nn; i++) {
    char **list = 0;
    snprintf(sub, sizeof sub, "%s.d", names[i]);
    int n = dropins(r, ri, sub, &list, 0);
    if (n > 1) qsort(list, n, sizeof *list, cmpstr);
    for (int k = 0; k < n; k++) { svc_parse_path(f, list[k], warn, ctx); free(list[k]); }
    free(list);
  }
  return 0;
}

int svc_dirdeps(const struct svc_roots *r, const char *name, const char *kind, char ***out) {
  char d[512];
  int n = 0;
  *out = 0;
  char ud[300];
  for (int ri = 0; ri < r->n; ri++)
    for (int k = 0; svc_unit_dir(r, ri, k, ud, sizeof ud); k++) {
      snprintf(d, sizeof d, "%s/%s.%s", ud, name, kind);
      DIR *dir = opendir(d);
      if (!dir) continue;
      struct dirent *de;
      while ((de = readdir(dir))) {
        if (de->d_name[0] == '.' || !svc_name_valid(de->d_name)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp((*out)[i], de->d_name)) dup = 1;
        if (dup) continue;
        *out = realloc(*out, (n + 1) * sizeof **out);
        (*out)[n++] = strdup(de->d_name);
      }
      closedir(dir);
    }
  return n;
}

/* ---------- [Install] ---------- */
static int add_change(struct svc_change *ch, int n, int max, const char *path, const char *target, int removed) {
  if (n < max) {
    snprintf(ch[n].path, sizeof ch[n].path, "%s", path);
    snprintf(ch[n].target, sizeof ch[n].target, "%s", target);
    ch[n].removed = removed;
  }
  return n + 1;
}

static int enable1(const struct svc_roots *r, const char *name, int dry, struct svc_change *ch, int n, int max,
                   char *err, size_t el, int depth) {
  struct svc_file f = {0};
  char path[512];
  int ri;
  int e = svc_load(r, name, &f, path, sizeof path, &ri, 0, 0);
  if (e == 1) { snprintf(err, el, "Unit %s is masked.", name); return -SVC_EMASKED; }
  if (e) { snprintf(err, el, "Unit %s does not exist.", name); svc_file_free(&f); return e; }
  /* the link target is the fragment as its own root sees it */
  const char *target = path + strlen(r->root[ri]);
  char inst_name[256];
  snprintf(inst_name, sizeof inst_name, "%s", name);
  if (svc_name_split(name, 0, 0, 0, 0) == 2) {
    const char *di = svc_get(&f, "Install", "DefaultInstance");
    if (di) { const char *at = strchr(name, '@'); snprintf(inst_name, sizeof inst_name, "%.*s%s%s", (int)(at - name + 1), name, di, strrchr(name, '.')); }
  }
  struct svc_spec sp = {inst_name, 0, 0, 0, 0, 0};
  const char *v[64];
  static const char *const kinds[][2] = {{"WantedBy", "wants"}, {"RequiredBy", "requires"}, {"UpheldBy", "upholds"}};
  char lp[600];
  for (int k = 0; k < 3; k++) {
    int c = svc_getall(&f, "Install", kinds[k][0], v, 64);
    for (int i = 0; i < c; i++) {
      char **w;
      char *x = svc_specifiers(v[i], &sp, 0);
      int nw = svc_words(x, &w);
      free(x);
      for (int j = 0; j < nw; j++) {
        if (svc_name_split(inst_name, 0, 0, 0, 0) == 2) continue; /* a template with no instance: nothing to link */
        snprintf(lp, sizeof lp, "%s/etc/systemd/system/%s.%s/%s", r->root[ri], w[j], kinds[k][1], inst_name);
        if (!dry) svc_makelink(target, lp);
        n = add_change(ch, n, max, lp, target, 0);
      }
      svc_free_words(w, nw);
    }
  }
  int c = svc_getall(&f, "Install", "Alias", v, 64);
  for (int i = 0; i < c; i++) {
    char **w;
    char *x = svc_specifiers(v[i], &sp, 0);
    int nw = svc_words(x, &w);
    free(x);
    for (int j = 0; j < nw; j++) {
      snprintf(lp, sizeof lp, "%s/etc/systemd/system/%s", r->root[ri], w[j]);
      if (!dry) svc_makelink(target, lp);
      n = add_change(ch, n, max, lp, target, 0);
    }
    svc_free_words(w, nw);
  }
  c = svc_getall(&f, "Install", "Also", v, 64);
  char also[64][128];
  int na = 0;
  for (int i = 0; i < c; i++) {
    char **w;
    int nw = svc_words(v[i], &w);
    for (int j = 0; j < nw && na < 64; j++) snprintf(also[na++], sizeof also[0], "%s", w[j]);
    svc_free_words(w, nw);
  }
  svc_file_free(&f);
  if (depth < 4)
    for (int i = 0; i < na; i++) {
      int m = enable1(r, also[i], dry, ch, n, max, err, el, depth + 1);
      if (m >= 0) n = m;
    }
  return n;
}
int svc_enable(const struct svc_roots *r, const char *name, int dry, struct svc_change *ch, int max, char *err, size_t el) {
  err[0] = 0;
  return enable1(r, name, dry, ch, 0, max, err, el, 0);
}

/* Remove every link to NAME (or its instances, for a template) under ROOT/etc. */
static int unlink_refs(const struct svc_roots *r, int ri, const char *name, const char *fragbase, struct svc_change *ch, int n, int max) {
  char d[512], p[800], t[512];
  snprintf(d, sizeof d, "%s/etc/systemd/system", r->root[ri]);
  DIR *dir = opendir(d);
  if (!dir) return n;
  struct dirent *de;
  int tmpl = svc_name_split(name, 0, 0, 0, 0) == 2;
  size_t pre = tmpl ? strchr(name, '@') - name + 1 : 0;
  while ((de = readdir(dir))) {
    if (de->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "%s/%s", d, de->d_name);
    size_t l = strlen(de->d_name);
    int isdir = (l > 6 && !strcmp(de->d_name + l - 6, ".wants")) || (l > 9 && !strcmp(de->d_name + l - 9, ".requires")) ||
                (l > 8 && !strcmp(de->d_name + l - 8, ".upholds"));
    if (isdir) {
      DIR *sd = opendir(p);
      if (!sd) continue;
      struct dirent *se;
      char victims[32][256];
      int nv = 0;
      while ((se = readdir(sd)) && nv < 32) {
        int hit = !strcmp(se->d_name, name) || (tmpl && !strncmp(se->d_name, name, pre) && svc_name_split(se->d_name, 0, 0, 0, 0) == 1 &&
                                                 !strcmp(strrchr(se->d_name, '.'), strrchr(name, '.')));
        if (hit) snprintf(victims[nv++], sizeof victims[0], "%s/%s", p, se->d_name);
      }
      closedir(sd);
      for (int i = 0; i < nv; i++) {
        if (svc_readlink(victims[i], t, sizeof t) != 1) continue;
        unlink(victims[i]);
        n = add_change(ch, n, max, victims[i], "", 1);
      }
      continue;
    }
    /* an alias: a link in etc whose target is this unit's fragment, under another name */
    if (strcmp(de->d_name, name) && svc_readlink(p, t, sizeof t) == 1 && fragbase && !strcmp(base(t), fragbase) && strcmp(t, "/dev/null")) {
      unlink(p);
      n = add_change(ch, n, max, p, "", 1);
    }
  }
  closedir(dir);
  return n;
}
int svc_disable(const struct svc_roots *r, const char *name, struct svc_change *ch, int max) {
  char path[512];
  int ri = 0;
  struct svc_file f = {0};
  int e = svc_load(r, name, &f, path, sizeof path, &ri, 0, 0);
  const char *fb = e == 0 ? base(path) : 0;
  int n = 0;
  for (int k = 0; k < r->n; k++) n = unlink_refs(r, k, name, fb, ch, n, max);
  if (e == 0) {
    const char *v[32];
    int c = svc_getall(&f, "Install", "Also", v, 32);
    for (int i = 0; i < c; i++) {
      char **w;
      int nw = svc_words(v[i], &w);
      for (int j = 0; j < nw; j++)
        for (int k = 0; k < r->n; k++) n = unlink_refs(r, k, w[j], 0, ch, n, max);
      svc_free_words(w, nw);
    }
  }
  svc_file_free(&f);
  return n;
}

static int linked(const struct svc_roots *r, const char *name) {
  char d[512], p[800];
  for (int ri = 0; ri < r->n; ri++) {
    snprintf(d, sizeof d, "%s/etc/systemd/system", r->root[ri]);
    DIR *dir = opendir(d);
    if (!dir) continue;
    struct dirent *de;
    int hit = 0;
    while (!hit && (de = readdir(dir))) {
      size_t l = strlen(de->d_name);
      if (!((l > 6 && !strcmp(de->d_name + l - 6, ".wants")) || (l > 9 && !strcmp(de->d_name + l - 9, ".requires")))) continue;
      snprintf(p, sizeof p, "%s/%s/%s", d, de->d_name, name);
      char t[512];
      if (svc_readlink(p, t, sizeof t) == 1) hit = 1;
    }
    closedir(dir);
    if (hit) return 1;
  }
  return 0;
}
const char *svc_is_enabled(const struct svc_roots *r, const char *name) {
  char path[512], real[256];
  int ri;
  int e = svc_find(r, name, path, sizeof path, &ri, real, sizeof real);
  if (e == 1) return "masked";
  if (e) return 0;
  if (strcmp(real, name) && svc_name_split(name, 0, 0, 0, 0) != 1) return "alias";
  struct svc_file f = {0};
  svc_load(r, name, &f, path, sizeof path, &ri, 0, 0);
  const char *v[4];
  int wb = svc_getall(&f, "Install", "WantedBy", v, 4) + svc_getall(&f, "Install", "RequiredBy", v, 4) +
           svc_getall(&f, "Install", "Alias", v, 4) + svc_getall(&f, "Install", "UpheldBy", v, 4);
  int also = svc_getall(&f, "Install", "Also", v, 4);
  svc_file_free(&f);
  if (linked(r, name)) return "enabled";
  if (!wb) return also ? "indirect" : "static";
  /* an alias link counts as enabled too */
  char d[512], p[800], t[512];
  snprintf(d, sizeof d, "%s/etc/systemd/system", r->root[ri]);
  DIR *dir = opendir(d);
  if (dir) {
    struct dirent *de;
    const char *fb = base(path);
    int hit = 0;
    while (!hit && (de = readdir(dir))) {
      if (!strcmp(de->d_name, name) || de->d_name[0] == '.') continue;
      snprintf(p, sizeof p, "%s/%s", d, de->d_name);
      if (svc_readlink(p, t, sizeof t) == 1 && !strcmp(base(t), fb)) hit = 1;
    }
    closedir(dir);
    if (hit) return "enabled";
  }
  return "disabled";
}
int svc_mask(const struct svc_roots *r, const char *name, int unmask) {
  char path[512], p[600], t[300];
  int ri = 0;
  if (svc_find(r, name, path, sizeof path, &ri, 0, 0) < 0) ri = 0;
  snprintf(p, sizeof p, "%s/etc/systemd/system/%s", r->root[ri], name);
  if (unmask) {
    int any = 0;
    for (int k = 0; k < r->n; k++) {
      snprintf(p, sizeof p, "%s/etc/systemd/system/%s", r->root[k], name);
      if (svc_readlink(p, t, sizeof t) == 1 && !strcmp(t, "/dev/null")) { unlink(p); any = 1; }
    }
    return any;
  }
  int l = svc_readlink(p, t, sizeof t);
  if (l == 0) return -EEXIST; /* a real file of the admin's is in the way */
  return svc_makelink("/dev/null", p);
}
