// SPDX-License-Identifier: GPL-2.0-or-later
/* The dpkg database, in dpkg's own on-disk formats so that the files are
 * recognisable and other tools can read them:
 *   ADMINDIR/status          one paragraph per known package, with
 *                            "Status: WANT EFLAG STATE"
 *   ADMINDIR/info/P.list     every path the package installed, one per line
 *   ADMINDIR/info/P.md5sums  "md5  path-without-leading-slash"
 *   ADMINDIR/info/P.conffiles, P.preinst, P.postinst, P.prerm, P.postrm, ...
 *   ADMINDIR/arch            architectures: native first, then foreign
 *   ADMINDIR/available       what dpkg --print-avail shows
 * P is "name", or "name:arch" for Multi-Arch: same packages.
 * status is rewritten whole, atomically (write, fsync, rename). dpkg also keeps
 * an updates/ journal between full rewrites; this code rewrites status after
 * every state change instead, which costs time but needs no journal replay. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

const char *const want_names[] = {"unknown", "install", "hold", "deinstall", "purge", 0};
const char *const eflag_names[] = {"ok", "reinstreq", 0};
const char *const status_names[] = {"not-installed", "config-files", "half-installed", "unpacked", "half-configured",
                                    "triggers-awaited", "triggers-pending", "installed", 0};

static int word_index(const char *const *names, const char *w, size_t n) {
  for (int i = 0; names[i]; i++) if (strlen(names[i]) == n && !strncmp(names[i], w, n)) return i;
  return -1;
}

const char *pi_version(const pkginfo *p) { return ctl_get(&p->f, "Version"); }
int pi_present(const pkginfo *p) { return p->status >= S_HALF_INSTALLED; }
int pi_configured(const pkginfo *p) { return p->status >= S_TRIGGERS_AWAITED; }
static int pi_ma_same(const pkginfo *p) { const char *m = ctl_get(&p->f, "Multi-Arch"); return m && !strcmp(m, "same"); }
const char *pi_key(const pkginfo *p, char *buf, size_t cap) {
  if (pi_ma_same(p) && p->arch) snprintf(buf, cap, "%s:%s", p->name, p->arch);
  else snprintf(buf, cap, "%s", p->name);
  return buf;
}
char *pi_infoname(const pkginfo *p, const char *ext) {
  char k[300];
  pi_key(p, k, sizeof k);
  char *rel = xasprintf("info/%s.%s", k, ext);
  char *r = path_join(pkg_cfg.admindir, rel);
  free(rel);
  return r;
}

static pkginfo *new_pkg(pkgdb *db, const char *name, const char *arch) {
  pkginfo *p = xcalloc(1, sizeof *p);
  p->name = xstrdup(name);
  p->arch = arch ? xstrdup(arch) : 0;
  if (db->n == db->cap) { db->cap = db->cap ? db->cap * 2 : 64; db->p = xrealloc(db->p, db->cap * sizeof *db->p); }
  db->p[db->n++] = p;
  void **slot = sm_slot(&db->byname, name);
  if (!*slot) *slot = p;
  return p;
}
pkginfo *db_find(pkgdb *db, const char *name, const char *arch) {
  pkginfo *first = sm_get(&db->byname, name);
  if (!first) return 0;
  pkginfo *any = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (strcmp(p->name, name)) continue;
    if (!arch) { if (pi_present(p) || p->status == S_CONFIG_FILES) return p; if (!any) any = p; continue; }
    const char *pa = p->arch ? p->arch : "";
    if (!strcmp(pa, arch)) return p;
    /* "all" packages count as native for identity */
    if (!strcmp(pa, "all") && !strcmp(arch, pkg_cfg.arch)) return p;
    if (!strcmp(arch, "all") && !strcmp(pa, pkg_cfg.arch)) return p;
  }
  return any;
}
pkginfo *db_get(pkgdb *db, const char *name, const char *arch) {
  pkginfo *p = db_find(db, name, arch);
  return p ? p : new_pkg(db, name, arch);
}
void db_remove_entry(pkgdb *db, pkginfo *p) {
  int j = 0;
  for (int i = 0; i < db->n; i++) if (db->p[i] != p) db->p[j++] = db->p[i];
  db->n = j;
  void **slot = sm_slot(&db->byname, p->name);
  if (*slot == p) {
    *slot = 0;
    for (int i = 0; i < db->n; i++) if (!strcmp(db->p[i]->name, p->name)) { *slot = db->p[i]; break; }
  }
  ctl_free(&p->f);
  free(p->name); free(p->arch); free(p);
  db->dirty = 1;
}

static int load_one(ctl_para *c, void *arg) {
  pkgdb *db = arg;
  const char *name = ctl_get(c, "Package"), *st = ctl_get(c, "Status");
  if (!name) { pkg_warn("status file: paragraph at line %d has no Package field; ignored", c->line); return 0; }
  const char *arch = ctl_get(c, "Architecture");
  pkginfo *p = new_pkg(db, name, arch);
  if (st) {
    const char *w[3]; size_t n[3]; int k = 0;
    for (const char *s = st; *s && k < 3;) {
      while (*s == ' ') s++;
      w[k] = s;
      while (*s && *s != ' ') s++;
      n[k] = s - w[k];
      if (n[k]) k++;
    }
    int a = k == 3 ? word_index(want_names, w[0], n[0]) : -1;
    int b = k == 3 ? word_index(eflag_names, w[1], n[1]) : -1;
    int s3 = k == 3 ? word_index(status_names, w[2], n[2]) : -1;
    if (a < 0 || s3 < 0) { pkg_warn("status file: package %s has a bad Status '%s'", name, st); a = a < 0 ? 0 : a; s3 = s3 < 0 ? 0 : s3; }
    if (b < 0) b = 1; /* dpkg's old "hold" and "hold-reinstreq" error flags mean reinstall required */
    p->want = a; p->eflag = b; p->status = s3;
  }
  for (int i = 0; i < c->n; i++) {
    const char *fn = ctl_name(c, i);
    if (!strcasecmp(fn, "Package") || !strcasecmp(fn, "Status")) continue;
    ctl_set(&p->f, fn, ctl_value(c, i));
  }
  return 0;
}

int db_load(pkgdb *db, int lock) {
  memset(db, 0, sizeof *db);
  db->lockfd = -1;
  char *st = path_join(pkg_cfg.admindir, "status");
  if (lock && !pkg_cfg.no_act) {
    if (mkdir_p(pkg_cfg.admindir, 0755) < 0) { pkg_error("cannot create %s: %s", pkg_cfg.admindir, strerror(errno)); free(st); return -1; }
    char *info = path_join(pkg_cfg.admindir, "info"), *upd = path_join(pkg_cfg.admindir, "updates"), *trg = path_join(pkg_cfg.admindir, "triggers");
    mkdir_p(info, 0755); mkdir_p(upd, 0755); mkdir_p(trg, 0755);
    free(info); free(upd); free(trg);
    char *lk = path_join(pkg_cfg.admindir, "lock");
    db->lockfd = open(lk, O_RDWR | O_CREAT, 0640);
    if (db->lockfd < 0) { pkg_error("cannot open the lock file %s: %s", lk, strerror(errno)); free(lk); free(st); return -1; }
#ifndef __wasm__
    struct flock fl = {0};
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    if (fcntl(db->lockfd, F_SETLK, &fl) < 0) {
      pkg_error("the dpkg database at %s is locked by another process", pkg_cfg.admindir);
      free(lk); free(st); close(db->lockfd); return -1;
    }
#endif
    free(lk);
    if (!path_exists(st)) {
      int fd = open(st, O_WRONLY | O_CREAT, 0644);
      if (fd >= 0) close(fd);
    }
  }
  int r = ctl_parse_file(st, load_one, db);
  free(st);
  if (r == -1) return -1;
  return 0;
}

static const char *const field_order[] = {
  "Package", "Essential", "Status", "Priority", "Section", "Installed-Size", "Origin", "Maintainer", "Bugs",
  "Architecture", "Multi-Arch", "Source", "Version", "Revision", "Config-Version", "Replaces", "Provides",
  "Depends", "Pre-Depends", "Recommends", "Suggests", "Breaks", "Conflicts", "Enhances", "Conffiles",
  "Filename", "Size", "MD5sum", "MSDOS-Filename", "Description", "Triggers-Pending", "Triggers-Awaited", 0};

void db_write_pkg(sbuf *b, const pkginfo *p) {
  for (int i = 0; field_order[i]; i++) {
    const char *f = field_order[i], *v;
    if (!strcmp(f, "Package")) v = p->name;
    else if (!strcmp(f, "Status")) { sb_printf(b, "Status: %s %s %s\n", want_names[p->want], eflag_names[p->eflag], status_names[p->status]); continue; }
    else v = ctl_get(&p->f, f);
    if (!v) continue;
    sb_printf(b, "%s:%s%s\n", f, *v && *v != '\n' ? " " : "", v);
  }
  for (int i = 0; i < p->f.n; i++) {
    const char *f = ctl_name(&p->f, i);
    int known = 0;
    for (int j = 0; field_order[j]; j++) if (!strcasecmp(field_order[j], f)) { known = 1; break; }
    if (known) continue;
    const char *v = ctl_value(&p->f, i);
    sb_printf(b, "%s:%s%s\n", f, *v && *v != '\n' ? " " : "", v);
  }
}
static int cmp_pkg(const void *a, const void *b) {
  const pkginfo *x = *(pkginfo *const *)a, *y = *(pkginfo *const *)b;
  int c = strcmp(x->name, y->name);
  if (c) return c;
  return strcmp(x->arch ? x->arch : "", y->arch ? y->arch : "");
}
int db_save(pkgdb *db) {
  if (pkg_cfg.no_act) return 0;
  xqsort(db->p, db->n, sizeof *db->p, cmp_pkg);
  sbuf b = {0};
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (p->status == S_NOT_INSTALLED && p->want <= W_UNKNOWN && p->eflag == E_OK) continue;
    if (p->status == S_NOT_INSTALLED && p->want == W_PURGE && p->eflag == E_OK) continue;
    db_write_pkg(&b, p);
    sb_putc(&b, '\n');
  }
  char *st = path_join(pkg_cfg.admindir, "status"), *old = path_join(pkg_cfg.admindir, "status-old");
  file_copy(st, old, 0644);   /* the previous state, as dpkg keeps it */
  int r = file_write_atomic(st, b.s ? b.s : "", b.n, 0644);
  if (r < 0) pkg_error("cannot write %s: %s", st, strerror(errno));
  free(st); free(old);
  sb_free(&b);
  db->dirty = 0;
  return r;
}
void db_free(pkgdb *db) {
  for (int i = 0; i < db->n; i++) { ctl_free(&db->p[i]->f); free(db->p[i]->name); free(db->p[i]->arch); free(db->p[i]); }
  free(db->p);
  sm_free(&db->byname);
  if (db->lockfd >= 0) close(db->lockfd);
  memset(db, 0, sizeof *db);
  db->lockfd = -1;
}

int db_read_list(const pkginfo *p, strlist *files) {
  char *path = pi_infoname(p, "list"), *data;
  int r = file_read(path, &data, 0);
  free(path);
  if (r < 0) return -1;
  for (char *s = data, *e; *s; s = e) {
    e = strchr(s, '\n');
    if (e) *e++ = 0; else e = s + strlen(s);
    if (*s) sl_add(files, s);
  }
  free(data);
  return 0;
}
int db_write_list(const pkginfo *p, const strlist *files) {
  sbuf b = {0};
  for (int i = 0; i < files->n; i++) { sb_puts(&b, files->v[i]); sb_putc(&b, '\n'); }
  char *path = pi_infoname(p, "list");
  int r = pkg_cfg.no_act ? 0 : file_write_atomic(path, b.s ? b.s : "", b.n, 0644);
  free(path);
  sb_free(&b);
  return r;
}
void db_file_owners(pkgdb *db, strmap *out, pkginfo *skip) {
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (p == skip || p->status == S_NOT_INSTALLED) continue;
    strlist l = {0};
    if (db_read_list(p, &l) == 0)
      for (int j = 0; j < l.n; j++) { void **s = sm_slot(out, l.v[j]); if (!*s) *s = p; }
    sl_free(&l);
  }
}

int conffiles_parse(const char *field, conffile **out) {
  *out = 0;
  int n = 0;
  if (!field) return 0;
  char *copy = xstrdup(field);
  for (char *line = copy, *next; line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *w[4] = {0};
    int k = 0;
    for (char *t = line; *t && k < 4;) {
      while (*t == ' ' || *t == '\t') t++;
      if (!*t) break;
      w[k++] = t;
      while (*t && *t != ' ' && *t != '\t') t++;
      if (*t) *t++ = 0;
    }
    if (k < 2) continue;
    *out = xrealloc(*out, (n + 1) * sizeof **out);
    conffile *c = &(*out)[n++];
    c->path = xstrdup(w[0]);
    snprintf(c->hash, sizeof c->hash, "%s", w[1]);
    c->obsolete = k >= 3 && !strcmp(w[2], "obsolete");
  }
  free(copy);
  return n;
}
void conffiles_free(conffile *c, int n) { for (int i = 0; i < n; i++) free(c[i].path); free(c); }

void db_log(const char *fmt, ...) {
  if (pkg_cfg.no_act) return;
  char *path = pkg_cfg.logfile ? xstrdup(pkg_cfg.logfile) : root_path("var/log/dpkg.log");
  char *dir = path_dirname(path);
  mkdir_p(dir, 0755);
  free(dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  free(path);
  if (fd < 0) return;
  time_t t = time(0);
  struct tm tm;
  gmtime_r(&t, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
  sbuf b = {0};
  sb_printf(&b, "%s ", ts);
  va_list ap, ap2;
  va_start(ap, fmt); va_copy(ap2, ap);
  int n = vsnprintf(0, 0, fmt, ap);
  va_end(ap);
  char *m = xmalloc(n + 1);
  vsnprintf(m, n + 1, fmt, ap2);
  va_end(ap2);
  sb_puts(&b, m);
  sb_putc(&b, '\n');
  free(m);
  if (write(fd, b.s, b.n) < 0) {}
  close(fd);
  sb_free(&b);
}
