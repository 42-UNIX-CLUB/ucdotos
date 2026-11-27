// SPDX-License-Identifier: GPL-2.0-or-later
/* Installing, configuring and removing packages, following Debian Policy
 * chapter 6 (maintainer scripts and the order things happen in) and the
 * package states of dpkg(1):
 *
 *   not-installed -> half-installed -> unpacked -> half-configured -> installed
 *   installed -> half-configured (prerm) -> half-installed (files go)
 *             -> config-files (conffiles stay) -> not-installed (purge)
 *
 * The status file is saved at every step, so an interrupted run leaves a state
 * the next run understands: half-installed means "unpack again" (the eflag is
 * reinstreq), unpacked and half-configured mean "dpkg --configure -a".
 *
 * Unpacking is in two phases. Phase one extracts every file next to its final
 * name as NAME.dpkg-new and checks for conflicts; if anything fails the new
 * files are deleted and nothing on disk has changed. Phase two renames them
 * into place. Conffiles stay as NAME.dpkg-new until configure decides between
 * the user's copy and the package's. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

extern strlist pkg_path_filters;
strlist dpkg_batch;

int pkg_glob(const char *p, const char *s) {
  for (;; p++, s++) {
    if (*p == '*') {
      while (*p == '*') p++;
      if (!*p) return 1;
      for (; *s; s++) if (pkg_glob(p, s)) return 1;
      return pkg_glob(p, s);
    }
    if (!*p) return !*s;
    if (!*s) return 0;
    if (*p == '?') continue;
    if (*p == '[') {
      const char *q = p + 1;
      int neg = *q == '!' || *q == '^', ok = 0;
      if (neg) q++;
      for (; *q && *q != ']'; q++) {
        if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= q[0] && *s <= q[2]) ok = 1; q += 2; }
        else if (*q == *s) ok = 1;
      }
      if (!*q) return 0;
      if (ok == neg) return 0;
      p = q;
      continue;
    }
    if (*p != *s) return 0;
  }
}
static int path_filtered_out(const char *path) {
  int out = 0;
  for (int i = 0; i < pkg_path_filters.n; i++)
    if (pkg_glob(pkg_path_filters.v[i] + 1, path)) out = pkg_path_filters.v[i][0] == '-';
  return out;
}

/* ---- the things the kernel may not support yet: try, and note ENOSYS ---- */
static int unsupported_errno(int e) { return e == ENOSYS || e == EOPNOTSUPP || e == ENOTSUP || e == EINVAL; }
static void apply_meta(const char *path, const tar_entry *e, int is_link) {
  if (!is_link && chmod(path, e->mode & 07777) < 0 && unsupported_errno(errno)) pkg_unsupported("set file modes", path);
  if (!(pkg_cfg.force & F_NOT_ROOT) && geteuid() == 0) {
    if (lchown(path, e->uid, e->gid) < 0 && unsupported_errno(errno)) pkg_unsupported("set file owners", path);
  }
  if (!is_link) {
    struct timespec ts[2] = {{e->mtime, 0}, {e->mtime, 0}};
    if (utimensat(AT_FDCWD, path, ts, 0) < 0 && unsupported_errno(errno)) pkg_unsupported("set file times", path);
  }
}

/* ---- dependency checks against the database ---- */
static int provides_match(const pkginfo *q, const dep_atom *a) {
  const char *pv = ctl_get(&q->f, "Provides");
  if (!pv) return 0;
  dep_list l;
  if (dep_parse(pv, DEP_PROVIDES, &l, 0) < 0) return 0;
  int ok = 0;
  for (int i = 0; i < l.n && !ok; i++) {
    dep_atom *b = &l.o[i].a[0];
    if (strcmp(b->name, a->name)) continue;
    if (a->op == REL_NONE) ok = 1;
    else if (b->op == REL_EQ && ver_satisfies(b->ver, a->op, a->ver)) ok = 1;
  }
  dep_free(&l);
  return ok;
}
static int ma_foreign(const pkginfo *q) { const char *m = ctl_get(&q->f, "Multi-Arch"); return m && !strcmp(m, "foreign"); }
static int ma_allowed(const pkginfo *q) { const char *m = ctl_get(&q->f, "Multi-Arch"); return m && !strcmp(m, "allowed"); }
/* Does installed package q satisfy atom a for a dependent of architecture deparch? */
static int pkg_satisfies(const pkginfo *q, const dep_atom *a, const char *deparch, int *via_provides) {
  const char *qa = q->arch ? q->arch : pkg_cfg.arch;
  int arch_ok;
  if (a->archq && !strcmp(a->archq, "any")) arch_ok = ma_allowed(q) || ma_foreign(q) || !strcmp(qa, "all") || !strcmp(qa, deparch);
  else if (a->archq && !strcmp(a->archq, "native")) arch_ok = !strcmp(qa, pkg_cfg.arch) || !strcmp(qa, "all");
  else if (a->archq) arch_ok = !strcmp(qa, a->archq);
  else arch_ok = !strcmp(qa, "all") || ma_foreign(q) || !strcmp(qa, deparch) || !strcmp(deparch, "all");
  if (via_provides) *via_provides = 0;
  if (!strcmp(q->name, a->name)) return arch_ok && ver_satisfies(pi_version(q), a->op, a->ver);
  if (arch_ok && provides_match(q, a)) { if (via_provides) *via_provides = 1; return 1; }
  return 0;
}
static int in_batch(const pkginfo *q) { char k[300]; return sl_find(&dpkg_batch, pi_key(q, k, sizeof k)) >= 0; }
int deps_satisfied_by_db(pkgdb *db, const dep_or *o, const char *deparch, int configured_only, pkginfo *skip) {
  for (int j = 0; j < o->n; j++)
    for (int i = 0; i < db->n; i++) {
      pkginfo *q = db->p[i];
      if (q == skip || !pi_present(q)) continue;
      if (q->mark == 2) continue;   /* being removed */
      if (configured_only && !pi_configured(q) && !(in_batch(q) && q->status >= S_UNPACKED)) continue;
      if (q->status < S_UNPACKED) continue;
      if (pkg_satisfies(q, &o->a[j], deparch, 0)) return 1;
    }
  return 0;
}
int dpkg_check_depends(pkgdb *db, pkginfo *p, const char *field, int configured_only, sbuf *why) {
  const char *v = ctl_get(&p->f, field);
  if (!v) return 1;
  dep_list l;
  char *err;
  if (dep_parse(v, DEP_ALT, &l, &err) < 0) { sb_printf(why, " %s: bad %s field: %s\n", p->name, field, err); free(err); return 0; }
  int ok = 1;
  const char *arch = p->arch ? p->arch : pkg_cfg.arch;
  for (int i = 0; i < l.n; i++) {
    if (deps_satisfied_by_db(db, &l.o[i], arch, configured_only, p)) continue;
    ok = 0;
    sbuf d = {0};
    dep_or one = l.o[i];
    dep_list tmp = {&one, 1};
    dep_format(&d, &tmp);
    sb_printf(why, " %s %s on %s; however it is not %s.\n", p->name, !strcmp(field, "Depends") ? "depends" : "pre-depends",
              d.s, configured_only ? "configured" : "installed");
    sb_free(&d);
  }
  dep_free(&l);
  return ok;
}

/* ---- small helpers ---- */
static void set_state(pkgdb *db, pkginfo *p, int st, const char *what) {
  p->status = st;
  db->dirty = 1;
  char k[300];
  db_log("status %s %s:%s %s", status_names[st], pi_key(p, k, sizeof k), p->arch ? p->arch : pkg_cfg.arch, pi_version(p) ? pi_version(p) : "<none>");
  (void)what;
  db_save(db);
}
static int run_script(pkginfo *p, const char *path, const char *name, const char *const *args, int *unrunnable) {
  char *why;
  int rc = script_run(p, path, name, args, &why);
  if (unrunnable) *unrunnable = 0;
  if (rc < 0) {
    if (pkg_cfg.force & F_UNRUNNABLE_SCRIPTS) {
      pkg_warn("%s %s cannot run here: %s; treating it as done", p->name, name, why);
      db_log("skipped %s %s: %s", p->name, name, why);
      char *u = pi_infoname(p, "unrun");
      int fd = pkg_cfg.no_act ? -1 : open(u, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd >= 0) {
        sbuf b = {0};
        sb_puts(&b, name);
        for (int i = 0; args && args[i]; i++) sb_printf(&b, " %s", args[i]);
        sb_printf(&b, "\t%s\n", why);
        if (write(fd, b.s, b.n) < 0) {}
        close(fd);
        sb_free(&b);
      }
      free(u);
      if (unrunnable) *unrunnable = 1;
      free(why);
      return 0;
    }
    pkg_error("%s %s subprocess cannot run: %s", p->name, name, why);
    free(why);
    return -1;
  }
  if (rc > 0) { pkg_error("%s package %s subprocess returned error exit status %d", p->name, name, rc); return rc; }
  return 0;
}
static int remove_tree_file(const char *full) {
  struct stat st;
  if (lstat(full, &st) < 0) return 0;
  if (S_ISDIR(st.st_mode)) return rmdir(full) < 0 ? -1 : 0;
  return unlink(full);
}
static int md5_of(const char *path, char out[40]) {
  char h[65];
  if (hex_digest_file(path, 1, h) < 0) return -1;
  memcpy(out, h, 33);
  return 0;
}
static int cmp_rev(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

/* Info files a package keeps: anything in ADMINDIR/info named PKG.* */
static const char *const info_exts[] = {"preinst", "postinst", "prerm", "postrm", "md5sums", "conffiles", "triggers",
                                        "shlibs", "symbols", "templates", "config", "list", "unrun", 0};
static void remove_info(pkginfo *p, int keep_postrm_and_list) {
  for (int i = 0; info_exts[i]; i++) {
    if (keep_postrm_and_list && (!strcmp(info_exts[i], "postrm") || !strcmp(info_exts[i], "list") || !strcmp(info_exts[i], "conffiles"))) continue;
    char *f = pi_infoname(p, info_exts[i]);
    if (!pkg_cfg.no_act) unlink(f);
    free(f);
  }
}

/* ---- triggers ---- */
static char *trig_path(const char *name) { char *r = xasprintf("triggers/%s", name); char *p = path_join(pkg_cfg.admindir, r); free(r); return p; }
static char *trig_cache;
static int trig_loaded;
static void trig_unregister(const pkginfo *p) {
  trig_loaded = 0;
  char k[300];
  pi_key(p, k, sizeof k);
  char *file = trig_path("File"), *data;
  if (file_read(file, &data, 0) == 0) {
    sbuf b = {0};
    for (char *s = data, *e; *s; s = e) {
      e = strchr(s, '\n'); if (e) *e++ = 0; else e = s + strlen(s);
      char *sp = strrchr(s, ' ');
      if (sp) { char who[300]; snprintf(who, sizeof who, "%s", sp + 1); char *sl = strchr(who, '/'); if (sl) *sl = 0; if (!strcmp(who, k)) continue; }
      if (*s) { sb_puts(&b, s); sb_putc(&b, '\n'); }
    }
    if (!pkg_cfg.no_act) file_write_atomic(file, b.s ? b.s : "", b.n, 0644);
    sb_free(&b);
    free(data);
  }
  free(file);
}
static void trig_register(const pkginfo *p, const char *triggers_text) {
  char k[300];
  pi_key(p, k, sizeof k);
  trig_unregister(p);
  if (!triggers_text) return;
  char *t = xstrdup(triggers_text);
  for (char *line = strtok(t, "\n"); line; line = strtok(0, "\n")) {
    char *w = line;
    while (*w == ' ' || *w == '\t') w++;
    if (*w == '#' || !*w) continue;
    char dir[32], arg[1024];
    if (sscanf(w, "%31s %1023s", dir, arg) != 2) continue;
    int noawait = strstr(dir, "noawait") != 0;
    if (!strncmp(dir, "interest", 8)) {
      char *f = arg[0] == '/' ? trig_path("File") : trig_path(arg);
      char *line2 = arg[0] == '/' ? xasprintf("%s %s%s\n", arg, k, noawait ? "/noawait" : "") : xasprintf("%s%s\n", k, noawait ? "/noawait" : "");
      int fd = pkg_cfg.no_act ? -1 : open(f, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd >= 0) { if (write(fd, line2, strlen(line2)) < 0) {} fsync_fd(fd, f); close(fd); }
      free(f); free(line2);
    }
  }
  free(t);
}
static void trig_pend(pkgdb *db, const char *who, const char *trigger) {
  char name[300];
  snprintf(name, sizeof name, "%s", who);
  char *sl = strchr(name, '/'); if (sl) *sl = 0;
  char *colon = strchr(name, ':');
  pkginfo *q = colon ? (*colon = 0, db_find(db, name, colon + 1)) : db_find(db, name, 0);
  if (!q || !pi_present(q) || q->status < S_UNPACKED) return;
  const char *cur = ctl_get(&q->f, "Triggers-Pending");
  if (cur) {
    char *c = xstrdup(cur);
    for (char *t = strtok(c, " \n"); t; t = strtok(0, " \n")) if (!strcmp(t, trigger)) { free(c); return; }
    free(c);
  }
  char *nv = cur ? xasprintf("%s %s", cur, trigger) : xstrdup(trigger);
  ctl_set(&q->f, "Triggers-Pending", nv);
  free(nv);
  if (q->status == S_INSTALLED) q->status = S_TRIGGERS_PENDING;
  db->dirty = 1;
}
static void trig_activate_file(pkgdb *db, const char *path) {
  if (pkg_cfg.no_triggers) return;
  if (!trig_loaded) { free(trig_cache); char *f = trig_path("File"); if (file_read(f, &trig_cache, 0) < 0) trig_cache = 0; free(f); trig_loaded = 1; }
  char *cache = trig_cache;
  if (!cache) return;
  char *c = xstrdup(cache);
  for (char *line = strtok(c, "\n"); line; line = strtok(0, "\n")) {
    char *sp = strrchr(line, ' ');
    if (!sp) continue;
    *sp = 0;
    size_t n = strlen(line);
    if (!strncmp(path, line, n) && (path[n] == 0 || path[n] == '/')) trig_pend(db, sp + 1, line);
  }
  free(c);
}
static void trig_activate_name(pkgdb *db, const char *name) {
  if (pkg_cfg.no_triggers) return;
  char *f = trig_path(name), *data;
  if (file_read(f, &data, 0) == 0) {
    for (char *t = strtok(data, "\n"); t; t = strtok(0, "\n")) trig_pend(db, t, name);
    free(data);
  }
  free(f);
}
static void trig_activate_from(pkgdb *db, const char *triggers_text) {
  if (!triggers_text) return;
  char *t = xstrdup(triggers_text);
  for (char *line = t, *next; line; line = next) {
    next = strchr(line, '\n'); if (next) *next++ = 0;
    char dir[32], arg[1024];
    if (sscanf(line, "%31s %1023s", dir, arg) == 2 && !strncmp(dir, "activate", 8)) trig_activate_name(db, arg);
  }
  free(t);
}
int dpkg_triggers_run(pkgdb *db) {
  if (pkg_cfg.no_triggers) return 0;
  int rc = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    const char *tp = ctl_get(&p->f, "Triggers-Pending");
    if (!tp || p->status != S_TRIGGERS_PENDING) continue;
    char *names = xstrdup(tp);
    for (char *s = names; *s; s++) if (*s == '\n') *s = ' ';
    pkg_msg("Processing triggers for %s (%s) ...\n", p->name, pi_version(p));
    const char *args[] = {"triggered", names, 0};
    if (run_script(p, 0, "postinst", args, 0) == 0) {
      ctl_del(&p->f, "Triggers-Pending");
      set_state(db, p, S_INSTALLED, "triggers");
    } else rc = 1;
    free(names);
  }
  return rc;
}

/* ---- unpack ---- */
typedef struct { char *path, *link; char type; int conffile; int existed_dir; } newfile;
typedef struct { newfile *v; int n, cap; } newfiles;
static void nf_add(newfiles *l, const char *path, char type, int conff) {
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = xrealloc(l->v, l->cap * sizeof *l->v); }
  newfile *f = &l->v[l->n++];
  memset(f, 0, sizeof *f);
  f->path = xstrdup(path); f->type = type; f->conffile = conff;
}
static void nf_free(newfiles *l) { for (int i = 0; i < l->n; i++) { free(l->v[i].path); free(l->v[i].link); } free(l->v); memset(l, 0, sizeof *l); }

static int conflicts_check(pkgdb *db, pkginfo *self, const ctl_para *ctl, const char *arch, strlist *to_remove) {
  int ok = 1;
  const char *fields[] = {"Conflicts", "Breaks"};
  dep_list rep = {0};
  dep_parse(ctl_get(ctl, "Replaces"), 0, &rep, 0);
  const char *myname = ctl_get(ctl, "Package"), *myver = ctl_get(ctl, "Version");
  for (int f = 0; f < 2; f++) {
    dep_list l;
    char *err;
    if (dep_parse(ctl_get(ctl, fields[f]), 0, &l, &err) < 0) { pkg_error("bad %s field: %s", fields[f], err); free(err); dep_free(&rep); return 0; }
    for (int i = 0; i < l.n; i++) {
      dep_atom *a = &l.o[i].a[0];
      for (int k = 0; k < db->n; k++) {
        pkginfo *q = db->p[k];
        if (q == self || !pi_present(q) || q->mark == 2) continue;
        int via;
        if (!pkg_satisfies(q, a, arch, &via)) continue;
        if (!strcmp(q->name, myname)) continue; /* a package never conflicts with itself */
        int replaced = 0;
        for (int r = 0; r < rep.n; r++) if (pkg_satisfies(q, &rep.o[r].a[0], arch, 0)) replaced = 1;
        if (f == 0 && replaced) {
          char kk[300];
          pkg_msg("dpkg: considering removing %s in favour of %s ...\n", q->name, myname);
          if (sl_find(to_remove, pi_key(q, kk, sizeof kk)) < 0) sl_add(to_remove, kk);
          continue;
        }
        if (pkg_cfg.force & (f == 0 ? F_CONFLICTS : F_BREAKS)) { pkg_warn("%s %s %s (%s); ignoring because of --force", myname, f ? "breaks" : "conflicts with", q->name, pi_version(q)); continue; }
        pkg_error("%s %s %s (%s), which is installed", myname, f ? "breaks" : "conflicts with", q->name, pi_version(q));
        ok = 0;
      }
    }
    dep_free(&l);
  }
  /* installed packages whose Conflicts or Breaks name the new package */
  for (int k = 0; k < db->n; k++) {
    pkginfo *q = db->p[k];
    if (q == self || !pi_present(q) || q->mark == 2) continue;
    for (int f = 0; f < 2; f++) {
      dep_list l;
      if (dep_parse(ctl_get(&q->f, fields[f]), 0, &l, 0) < 0) continue;
      for (int i = 0; i < l.n; i++) {
        dep_atom *a = &l.o[i].a[0];
        if (strcmp(a->name, myname) || !ver_satisfies(myver, a->op, a->ver)) continue;
        if (!strcmp(q->name, myname)) continue;
        char kk[300];
        if (sl_find(to_remove, pi_key(q, kk, sizeof kk)) >= 0) continue;
        if (pkg_cfg.force & (f == 0 ? F_CONFLICTS : F_BREAKS)) { pkg_warn("%s (%s) %s %s; ignoring because of --force", q->name, pi_version(q), f ? "breaks" : "conflicts with", myname); continue; }
        pkg_error("installed package %s (%s) %s %s (%s)", q->name, pi_version(q), f ? "breaks" : "conflicts with", myname, myver);
        ok = 0;
      }
      dep_free(&l);
    }
  }
  dep_free(&rep);
  return ok;
}

static int replaces_pkg(const ctl_para *ctl, const pkginfo *q, const char *arch) {
  dep_list rep;
  if (dep_parse(ctl_get(ctl, "Replaces"), 0, &rep, 0) < 0) return 0;
  int r = 0;
  for (int i = 0; i < rep.n && !r; i++) if (pkg_satisfies(q, &rep.o[i].a[0], arch, 0)) r = 1;
  dep_free(&rep);
  return r;
}

static int write_entry_data(deb *d, const tar_entry *e, const char *dest) {
  int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  char *buf = xmalloc(65536);
  long r;
  uint64_t total = 0;
  while ((r = tar_read(d->data, buf, 65536)) > 0) {
    if (write(fd, buf, r) != r) { r = -2; break; }
    total += r;
  }
  free(buf);
  if (r == 0 && total != e->size) r = -1;
  if (r == 0) fsync_fd(fd, dest);
  close(fd);
  return r < 0 ? -1 : 0;
}

static void install_info_files(pkginfo *p, deb *d) {
  /* remove the old set first so that a script the new version dropped is gone */
  remove_info(p, 0);
  for (int i = 0; i < d->ncf; i++) {
    const deb_cfile *c = &d->cf[i];
    if (!strcmp(c->name, "control")) continue;
    if (!ctl_field_name_ok(c->name, strlen(c->name)) || strchr(c->name, '.')) continue;
    char *f = pi_infoname(p, c->name);
    int scr = !strcmp(c->name, "preinst") || !strcmp(c->name, "postinst") || !strcmp(c->name, "prerm") || !strcmp(c->name, "postrm") || !strcmp(c->name, "config");
    if (!pkg_cfg.no_act && file_write_atomic(f, c->data, c->size, scr ? 0755 : 0644) < 0) pkg_error("cannot write %s: %s", f, strerror(errno));
    if (scr && chmod(f, 0755) < 0 && unsupported_errno(errno)) pkg_unsupported("set file modes", f);
    free(f);
  }
}

/* Files of the old version, minus those the new one ships, removed deepest
 * first. Directories go only when empty and not listed by another package. */
static void remove_obsolete(pkgdb *db, pkginfo *p, strlist *oldl, newfiles *nf, strmap *owners, conffile *oldcf, int noldcf) {
  strmap newset = {0};
  for (int i = 0; i < nf->n; i++) *sm_slot(&newset, nf->v[i].path) = (void *)1;
  strlist gone = {0};
  for (int i = 0; i < oldl->n; i++) {
    const char *f = oldl->v[i];
    if (sm_get(&newset, f) || !strcmp(f, "/.") || !strcmp(f, "/")) continue;
    int isconf = 0;
    for (int c = 0; c < noldcf; c++) if (!strcmp(oldcf[c].path, f)) isconf = 1;
    if (isconf) continue;   /* obsolete conffiles stay; the Conffiles field marks them */
    pkginfo *o = sm_get(owners, f);
    char *full = root_path(f);
    if (o && o != p && path_is_dir(full)) { free(full); continue; }
    if (o && o != p) { free(full); continue; }  /* now another package's file */
    sl_add(&gone, f);
    free(full);
  }
  xqsort(gone.v, gone.n, sizeof *gone.v, cmp_rev);
  for (int i = 0; i < gone.n; i++) {
    char *full = root_path(gone.v[i]);
    if (!pkg_cfg.no_act) {
      if (remove_tree_file(full) < 0 && errno != ENOTEMPTY && errno != EEXIST && errno != ENOENT)
        pkg_warn("while removing %s: %s", full, strerror(errno));
      else trig_activate_file(db, gone.v[i]);
    }
    free(full);
  }
  sl_free(&gone);
  sm_free(&newset);
}

/* Remove a path from another package's file list (it now belongs to us). If
 * that leaves the package with no files of its own, it disappears. */
static void take_over(pkgdb *db, pkginfo *q, strlist *paths, pkginfo *by) {
  strlist l = {0};
  if (db_read_list(q, &l) < 0) return;
  strlist keep = {0};
  int files_left = 0;
  for (int i = 0; i < l.n; i++) {
    if (sl_find(paths, l.v[i]) >= 0) continue;
    sl_add(&keep, l.v[i]);
    char *full = root_path(l.v[i]);
    if (!path_is_dir(full)) files_left++;
    free(full);
  }
  db_write_list(q, &keep);
  if (!files_left && !ctl_get(&q->f, "Conffiles") && pi_present(q) && !(q->mark == 2)) {
    const char *args[] = {"disappear", by->name, pi_version(by), 0};
    run_script(q, 0, "postrm", args, 0);
    pkg_msg("(Noting disappearance of %s, which has been completely replaced.)\n", q->name);
    remove_info(q, 0);
    q->want = W_UNKNOWN; q->eflag = E_OK;
    set_state(db, q, S_NOT_INSTALLED, "disappear");
  }
  sl_free(&l); sl_free(&keep);
}

static void put_conffiles_field(pkginfo *p, conffile *c, int n) {
  if (!n) { ctl_del(&p->f, "Conffiles"); return; }
  sbuf b = {0};
  for (int i = 0; i < n; i++) sb_printf(&b, "\n %s %s%s", c[i].path, c[i].hash, c[i].obsolete ? " obsolete" : "");
  ctl_set(&p->f, "Conffiles", b.s);
  sb_free(&b);
}

int dpkg_unpack(pkgdb *db, const char *debpath, int configure_after) {
  (void)configure_after;
  char *err;
  deb *d = deb_open(debpath, &err);
  if (!d) { pkg_error("%s", err); free(err); return 1; }
  const ctl_para *ctl = &d->control;
  const char *name = ctl_get(ctl, "Package"), *ver = ctl_get(ctl, "Version"), *arch = ctl_get(ctl, "Architecture");
  int rc = 1;
  if (!ver || !arch) { pkg_error("%s: control file lacks Version or Architecture", debpath); deb_close(d); return 1; }
  const char *vwhy = ver_check(ver);
  if (vwhy && !(pkg_cfg.force & F_BAD_VERSION)) { pkg_error("%s: bad version '%s': %s", debpath, ver, vwhy); deb_close(d); return 1; }
  if (!arch_is_known(arch) && !(pkg_cfg.force & F_ARCHITECTURE)) {
    pkg_error("package architecture (%s) does not match system (%s)", arch, pkg_cfg.arch);
    deb_close(d); return 1;
  }
  const char *idarch = !strcmp(arch, "all") ? pkg_cfg.arch : arch;
  pkginfo *p = db_get(db, name, idarch);
  if (!p->arch) p->arch = xstrdup(arch);
  int was_present = pi_present(p);
  char *oldver = was_present || p->status == S_CONFIG_FILES ? xstrdup(pi_version(p) ? pi_version(p) : "") : 0;
  char *oldcfgver = 0;
  if (p->status == S_INSTALLED || p->status == S_TRIGGERS_PENDING || p->status == S_TRIGGERS_AWAITED || p->status == S_CONFIG_FILES)
    oldcfgver = xstrdup(oldver ? oldver : "");
  else if (ctl_get(&p->f, "Config-Version")) oldcfgver = xstrdup(ctl_get(&p->f, "Config-Version"));
  if (p->want == W_HOLD && !(pkg_cfg.force & F_HOLD) && was_present) {
    pkg_warn("package %s is on hold, not touching it; use --force-hold to override", name);
    free(oldver); free(oldcfgver); deb_close(d); return 1;
  }
  if (was_present && oldver && *oldver) {
    int c = ver_cmp(ver, oldver);
    if (c < 0) {
      if (!(pkg_cfg.force & F_DOWNGRADE)) { pkg_error("downgrading %s from %s to %s is refused", name, oldver, ver); free(oldver); free(oldcfgver); deb_close(d); return 1; }
      pkg_warn("downgrading %s from %s to %s", name, oldver, ver);
    }
  }
  pkg_msg("%s %s (%s)%s%s%s ...\n", was_present ? "Preparing to unpack" : "Selecting previously unselected package", name, ver,
          was_present ? " over (" : "", was_present ? oldver : "", was_present ? ")" : "");
  /* Pre-Depends must be configured before anything happens */
  {
    pkginfo tmp = {0};
    tmp.name = (char *)name; tmp.arch = (char *)arch;
    ctl_copy(&tmp.f, ctl);
    sbuf why = {0};
    int ok = dpkg_check_depends(db, &tmp, "Pre-Depends", 1, &why);
    ctl_free(&tmp.f);
    if (!ok) {
      if (pkg_cfg.force & F_DEPENDS) pkg_warn("pre-dependency problem, continuing because of --force-depends:\n%s", why.s);
      else { pkg_error("pre-dependency problem - not installing %s:\n%s", name, why.s); sb_free(&why); free(oldver); free(oldcfgver); deb_close(d); return 1; }
    }
    sb_free(&why);
  }
  strlist to_remove = {0};
  if (!conflicts_check(db, p, ctl, arch, &to_remove)) { pkg_error("not installing %s", name); sl_free(&to_remove); free(oldver); free(oldcfgver); deb_close(d); return 1; }

  /* the new control files, where the new preinst can be run from */
  char *ci = path_join(pkg_cfg.admindir, "tmp.ci");
  if (!pkg_cfg.no_act) {
    mkdir_p(ci, 0755);
    for (int i = 0; i < d->ncf; i++) {
      if (strchr(d->cf[i].name, '/')) continue;
      char *f = path_join(ci, d->cf[i].name);
      file_write_atomic(f, d->cf[i].data, d->cf[i].size, 0755);
      chmod(f, 0755);
      free(f);
    }
  }
  /* scripts before the unpack (Policy 6.6) */
  if (was_present && p->status >= S_HALF_CONFIGURED) {
    const char *a1[] = {"upgrade", ver, 0};
    if (run_script(p, 0, "prerm", a1, 0) != 0) {
      char *np = path_join(ci, "prerm");
      const char *a2[] = {"failed-upgrade", oldver, 0};
      if (run_script(p, np, "prerm", a2, 0) != 0) {
        const char *a3[] = {"abort-upgrade", ver, 0};
        run_script(p, 0, "postinst", a3, 0);
        free(np);
        goto out;
      }
      free(np);
    }
  }
  for (int i = 0; i < to_remove.n; i++) {
    char *colon = strchr(to_remove.v[i], ':');
    pkginfo *q = colon ? (*colon = 0, db_find(db, to_remove.v[i], colon + 1)) : db_find(db, to_remove.v[i], 0);
    if (colon) *colon = ':';
    if (!q) continue;
    const char *a[] = {"remove", "in-favour", name, ver, 0};
    if (run_script(q, 0, "prerm", a, 0) != 0) goto out;
    q->mark = 2;
  }
  {
    char *np = path_join(ci, "preinst");
    int r;
    if (was_present && oldver) { const char *a[] = {"upgrade", oldver, ver, 0}; r = run_script(p, np, "preinst", a, 0); }
    else if (p->status == S_CONFIG_FILES && oldver) { const char *a[] = {"install", oldver, ver, 0}; r = run_script(p, np, "preinst", a, 0); }
    else { const char *a[] = {"install", 0}; r = run_script(p, np, "preinst", a, 0); }
    if (r != 0) {
      char *pr = path_join(ci, "postrm");
      if (was_present && oldver) {
        const char *a[] = {"abort-upgrade", oldver, ver, 0}; run_script(p, pr, "postrm", a, 0);
        const char *b[] = {"abort-upgrade", ver, 0}; run_script(p, 0, "postinst", b, 0);
      } else { const char *a[] = {"abort-install", 0}; run_script(p, pr, "postrm", a, 0); }
      free(pr); free(np);
      goto out;
    }
    free(np);
  }
  /* from here an interruption leaves half-installed + reinstreq */
  if (!was_present) {
    /* a fresh entry: take the new package's fields now so the entry is complete */
    ctl_para keep = {0};
    const char *cf = ctl_get(&p->f, "Conffiles");
    if (cf) ctl_set(&keep, "Conffiles", cf);
    ctl_copy(&p->f, ctl);
    ctl_del(&p->f, "Package");
    if (cf) ctl_set(&p->f, "Conffiles", ctl_get(&keep, "Conffiles"));
    ctl_free(&keep);
  }
  p->want = W_INSTALL;
  p->eflag = E_REINSTREQ;
  set_state(db, p, S_HALF_INSTALLED, "unpack");

  /* phase one: extract to .dpkg-new */
  strmap owners = {0};
  db_file_owners(db, &owners, p);
  conffile *newcf = 0;
  int nnewcf = 0;
  {
    const deb_cfile *c = deb_cfile_get(d, "conffiles");
    if (c) {
      char *t = xstrndup((const char *)c->data, c->size);
      for (char *line = strtok(t, "\n"); line; line = strtok(0, "\n")) {
        while (*line == ' ' || *line == '\t') line++;
        char *e = line + strlen(line);
        while (e > line && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\t')) *--e = 0;
        if (!*line) continue;
        /* dpkg 1.20+ allows a "remove-on-upgrade" flag before the path */
        if (!strncmp(line, "remove-on-upgrade ", 18)) continue;
        newcf = xrealloc(newcf, (nnewcf + 1) * sizeof *newcf);
        newcf[nnewcf].path = xstrdup(line);
        strcpy(newcf[nnewcf].hash, "newconffile");
        newcf[nnewcf].obsolete = 0;
        nnewcf++;
      }
      free(t);
    }
  }
  conffile *oldcf = 0;
  int noldcf = conffiles_parse(ctl_get(&p->f, "Conffiles"), &oldcf);
  newfiles nf = {0};
  strmap took = {0};           /* other package key -> (unused) */
  strlist tookpaths = {0};
  int failed = 0;
  if (deb_data_begin(d, 0, &err) < 0) { pkg_error("%s: %s", debpath, err); free(err); failed = 1; }
  tar_entry *e = xmalloc(sizeof *e);
  int k = 0;
  while (!failed && (k = tar_next(d->data, e)) == 1) {
    char np[4096];
    if (!deb_norm_path(e->path, np, sizeof np)) { pkg_error("%s: unsafe path '%s' in archive", debpath, e->path); failed = 1; break; }
    if (!strcmp(np, "/")) { nf_add(&nf, "/.", '5', 0); continue; }
    if (e->type != '5' && path_filtered_out(np)) continue;
    int isconf = 0;
    for (int c = 0; c < nnewcf; c++) if (!strcmp(newcf[c].path, np)) isconf = 1;
    char *full = root_path(np);
    char *dir = path_dirname(full);
    if (!pkg_cfg.no_act) mkdir_p(dir, 0755);
    free(dir);
    struct stat st;
    int exists = lstat(full, &st) == 0;
    if (e->type == '5') {
      nf_add(&nf, np, '5', 0);
      if (exists && S_ISDIR(st.st_mode)) { nf.v[nf.n - 1].existed_dir = 1; free(full); continue; }
      if (exists) {
        pkginfo *o = sm_get(&owners, np);
        if (o && !(pkg_cfg.force & F_OVERWRITE_DIR)) { pkg_error("trying to overwrite '%s' (owned by %s) with a directory", np, o->name); failed = 1; free(full); break; }
        if (!pkg_cfg.no_act) unlink(full);
      }
      if (!pkg_cfg.no_act) {
        if (mkdir(full, e->mode & 07777) < 0 && errno != EEXIST) { pkg_error("cannot create directory %s: %s", full, strerror(errno)); failed = 1; free(full); break; }
        apply_meta(full, e, 0);
      }
      free(full);
      continue;
    }
    /* a non-directory: check ownership */
    if (exists && S_ISDIR(st.st_mode) && !(pkg_cfg.force & F_OVERWRITE_DIR)) {
      pkg_error("trying to overwrite directory '%s' in package %s with a non-directory", np, name);
      failed = 1; free(full); break;
    }
    pkginfo *o = sm_get(&owners, np);
    if (o && o != p && o->mark != 2) {
      char ok[300];
      if (replaces_pkg(ctl, o, arch)) {
        pkg_msg("Replacing files in old package %s (%s) ...\n", o->name, pi_version(o));
        *sm_slot(&took, pi_key(o, ok, sizeof ok)) = o;
        sl_add(&tookpaths, np);
      } else if (pkg_cfg.force & F_OVERWRITE) {
        pkg_warn("overwriting '%s', which is also in package %s %s", np, o->name, pi_version(o));
        *sm_slot(&took, pi_key(o, ok, sizeof ok)) = o;
        sl_add(&tookpaths, np);
      } else {
        pkg_error("trying to overwrite '%s', which is also in package %s %s", np, o->name, pi_version(o) ? pi_version(o) : "");
        failed = 1; free(full); break;
      }
    }
    nf_add(&nf, np, e->type, isconf);
    char *tmp = xasprintf("%s.dpkg-new", full);
    if (!pkg_cfg.no_act) {
      unlink(tmp);
      if (e->type == '0' || e->type == '7') {
        if (write_entry_data(d, e, tmp) < 0) { pkg_error("cannot extract %s: %s", np, pkg_file_error(errno, tmp)); failed = 1; }
        else apply_meta(tmp, e, 0);
      } else if (e->type == '2') {
        if (symlink(e->link, tmp) < 0) {
          if (unsupported_errno(errno)) { pkg_unsupported("create symbolic links (copying targets instead)", np); nf.v[nf.n - 1].type = 'S'; nf.v[nf.n - 1].link = xstrdup(e->link); }
          else { pkg_error("cannot create symlink %s: %s", np, strerror(errno)); failed = 1; }
        } else apply_meta(tmp, e, 1);
      } else if (e->type == '1') {
        char tp[4096];
        if (!deb_norm_path(e->link, tp, sizeof tp)) { pkg_error("unsafe hard link target %s", e->link); failed = 1; }
        else {
          char *tfull = root_path(tp), *tnew = xasprintf("%s.dpkg-new", tfull);
          const char *src = path_exists(tnew) ? tnew : tfull;
          if (link(src, tmp) < 0) {
            if (unsupported_errno(errno) || errno == EPERM) { pkg_unsupported("create hard links (copied instead)", np); if (file_copy(src, tmp, 0644) < 0) failed = 1; }
            else { pkg_error("cannot create hard link %s: %s", np, strerror(errno)); failed = 1; }
          }
          free(tfull); free(tnew);
        }
      } else {
        pkg_unsupported("create device files and fifos", np);
        nf.n--; free(nf.v[nf.n].path);
      }
    }
    free(tmp);
    free(full);
  }
  if (k < 0 && !failed) { pkg_error("%s: corrupt data archive (%s)", debpath, arc_error(d->data)); failed = 1; }
  free(e);
  if (failed) {
    /* nothing has replaced anything yet: delete what phase one made */
    for (int i = 0; i < nf.n; i++) {
      if (nf.v[i].type == '5') continue;
      char *full = root_path(nf.v[i].path), *tmp = xasprintf("%s.dpkg-new", full);
      if (!pkg_cfg.no_act) unlink(tmp);
      free(full); free(tmp);
    }
    for (int i = nf.n - 1; i >= 0; i--)
      if (nf.v[i].type == '5' && !nf.v[i].existed_dir && strcmp(nf.v[i].path, "/.")) { char *full = root_path(nf.v[i].path); if (!pkg_cfg.no_act) rmdir(full); free(full); }
    if (was_present && oldver) {
      /* the old version is still complete on disk: put its state back */
      p->eflag = E_OK;
      const char *a[] = {"abort-upgrade", oldver, ver, 0};
      char *pr = path_join(ci, "postrm");
      run_script(p, pr, "postrm", a, 0);
      free(pr);
      const char *b[] = {"abort-upgrade", ver, 0};
      run_script(p, 0, "postinst", b, 0);
      set_state(db, p, oldcfgver ? S_INSTALLED : S_UNPACKED, "abort");
    } else {
      const char *a[] = {"abort-install", 0};
      char *pr = path_join(ci, "postrm");
      run_script(p, pr, "postrm", a, 0);
      free(pr);
      p->eflag = E_OK; p->want = W_UNKNOWN;
      set_state(db, p, noldcf ? S_CONFIG_FILES : S_NOT_INSTALLED, "abort");
    }
    nf_free(&nf); sm_free(&owners); sm_free(&took); sl_free(&tookpaths);
    conffiles_free(newcf, nnewcf); conffiles_free(oldcf, noldcf);
    goto out;
  }
  /* phase two: into place */
  strlist oldlist = {0};
  if (was_present || p->status == S_HALF_INSTALLED) db_read_list(p, &oldlist);
  for (int i = 0; i < nf.n; i++) {
    newfile *f = &nf.v[i];
    if (f->type == '5' || f->conffile || pkg_cfg.no_act) continue;
    char *full = root_path(f->path), *tmp = xasprintf("%s.dpkg-new", full);
    if (f->type == 'S') { free(full); free(tmp); continue; }
    if (rename(tmp, full) < 0) pkg_error("cannot move %s into place: %s", full, strerror(errno));
    trig_activate_file(db, f->path);
    free(full); free(tmp);
  }
  /* symlinks the kernel could not make: copy the target if it is a regular file we just installed */
  for (int i = 0; i < nf.n && !pkg_cfg.no_act; i++) {
    newfile *f = &nf.v[i];
    if (f->type != 'S' || !f->link) continue;
    char *dir = path_dirname(f->path);
    char *rel = f->link[0] == '/' ? xstrdup(f->link) : xasprintf("%s/%s", dir, f->link);
    char np[4096];
    if (deb_norm_path(rel, np, sizeof np)) {
      char *src = root_path(np), *dst = root_path(f->path);
      struct stat st;
      if (stat(src, &st) == 0 && S_ISREG(st.st_mode)) file_copy(src, dst, st.st_mode & 0777);
      free(src); free(dst);
    }
    free(dir); free(rel);
  }
  /* files other packages gave up */
  for (uint32_t i = 0; i < took.cap; i++) if (took.k && took.k[i]) take_over(db, took.v[i], &tookpaths, p);
  /* old files the new version does not ship */
  strmap owners2 = {0};
  db_file_owners(db, &owners2, p);
  remove_obsolete(db, p, &oldlist, &nf, &owners2, oldcf, noldcf);
  sm_free(&owners2);
  if (was_present && oldver) {
    const char *a[] = {"upgrade", ver, 0};
    if (run_script(p, 0, "postrm", a, 0) != 0) {
      char *pr = path_join(ci, "postrm");
      const char *b[] = {"failed-upgrade", oldver, 0};
      int r = run_script(p, pr, "postrm", b, 0);
      free(pr);
      if (r != 0) { nf_free(&nf); sm_free(&owners); sm_free(&took); sl_free(&tookpaths); sl_free(&oldlist); conffiles_free(newcf, nnewcf); conffiles_free(oldcf, noldcf); goto out; }
    }
  }
  /* new database entry */
  {
    strlist list = {0};
    for (int i = 0; i < nf.n; i++) if (sl_find(&list, nf.v[i].path) < 0 || i == 0) sl_add(&list, nf.v[i].path);
    install_info_files(p, d);
    db_write_list(p, &list);
    sl_free(&list);
    trig_register(p, deb_cfile_get(d, "triggers") ? (const char *)deb_cfile_get(d, "triggers")->data : 0);
    /* conffile hashes: the old recorded hash (what the user's copy is compared
     * with at configure), "newconffile" for new ones; old ones that are gone
     * stay as obsolete */
    for (int i = 0; i < nnewcf; i++)
      for (int j = 0; j < noldcf; j++) if (!strcmp(newcf[i].path, oldcf[j].path)) { strcpy(newcf[i].hash, oldcf[j].hash); oldcf[j].obsolete = 2; }
    int total = nnewcf;
    conffile *all = xcalloc(nnewcf + noldcf + 1, sizeof *all);
    for (int i = 0; i < nnewcf; i++) { all[i] = newcf[i]; all[i].path = xstrdup(newcf[i].path); }
    for (int j = 0; j < noldcf; j++) if (oldcf[j].obsolete != 2) {
      char *full = root_path(oldcf[j].path);
      if (path_exists(full)) { all[total] = oldcf[j]; all[total].path = xstrdup(oldcf[j].path); all[total].obsolete = 1; total++; }
      free(full);
    }
    ctl_para nfld = {0};
    ctl_copy(&nfld, ctl);
    ctl_del(&nfld, "Package");
    ctl_copy(&p->f, &nfld);
    ctl_free(&nfld);
    if (oldcfgver && *oldcfgver) ctl_set(&p->f, "Config-Version", oldcfgver);
    put_conffiles_field(p, all, total);
    conffiles_free(all, total);
    free(p->arch);
    p->arch = xstrdup(arch);
  }
  p->eflag = E_OK;
  set_state(db, p, S_UNPACKED, "unpacked");
  trig_activate_from(db, deb_cfile_get(d, "triggers") ? (const char *)deb_cfile_get(d, "triggers")->data : 0);
  db_save(db);
  /* conflicting packages that the new one replaces go now */
  for (int i = 0; i < to_remove.n; i++) {
    char *colon = strchr(to_remove.v[i], ':');
    pkginfo *q = colon ? (*colon = 0, db_find(db, to_remove.v[i], colon + 1)) : db_find(db, to_remove.v[i], 0);
    if (q && pi_present(q)) { q->mark = 0; dpkg_remove(db, q, 0); }
  }
  rc = 0;
  nf_free(&nf); sm_free(&owners); sm_free(&took); sl_free(&tookpaths); sl_free(&oldlist);
  conffiles_free(newcf, nnewcf); conffiles_free(oldcf, noldcf);
out:
  if (!pkg_cfg.no_act) {
    for (int i = 0; i < d->ncf; i++) { char *f = path_join(ci, d->cf[i].name); unlink(f); free(f); }
    rmdir(ci);
  }
  free(ci);
  sl_free(&to_remove);
  free(oldver); free(oldcfgver);
  deb_close(d);
  return rc;
}

/* ---- configure ---- */
static int resolve_conffiles(pkginfo *p) {
  conffile *cf;
  int n = conffiles_parse(ctl_get(&p->f, "Conffiles"), &cf);
  for (int i = 0; i < n; i++) {
    if (cf[i].obsolete) continue;
    char *full = root_path(cf[i].path), *nw = xasprintf("%s.dpkg-new", full);
    char hdist[40], hcur[40];
    if (md5_of(nw, hdist) < 0) { free(full); free(nw); continue; }  /* nothing new to decide */
    int have = md5_of(full, hcur) == 0;
    const char *hold = cf[i].hash;
    const char *what;
    if (!have) {
      if (!strcmp(hold, "newconffile") || (pkg_cfg.force & F_CONFMISS)) { rename(nw, full); what = "installed"; }
      else { unlink(nw); what = "kept deleted (the user removed it)"; }
    } else if (!strcmp(hcur, hdist)) { unlink(nw); what = "unchanged"; }
    else if (!strcmp(hcur, hold)) { rename(nw, full); what = "updated (not modified locally)"; }
    else if (!strcmp(hold, hdist)) { unlink(nw); what = "kept (modified locally, package version unchanged)"; }
    else if (pkg_cfg.force & F_CONFNEW) {
      char *old = xasprintf("%s.dpkg-old", full);
      rename(full, old); rename(nw, full);
      pkg_msg("Configuration file '%s': installing the package's version; yours is %s\n", cf[i].path, old);
      free(old);
      what = "replaced, old copy kept as .dpkg-old";
    } else {
      char *dist = xasprintf("%s.dpkg-dist", full);
      rename(nw, dist);
      pkg_msg("Configuration file '%s' was modified locally and the package has a new version.\n"
              " Keeping your version; the package's version is %s\n", cf[i].path, dist);
      free(dist);
      what = "kept, new version saved as .dpkg-dist";
    }
    if (pkg_verbose) pkg_msg("conffile %s: %s\n", cf[i].path, what);
    memcpy(cf[i].hash, hdist, 33);
    free(full); free(nw);
  }
  put_conffiles_field(p, cf, n);
  conffiles_free(cf, n);
  return 0;
}

int dpkg_configure(pkgdb *db, pkginfo *p, int in_batch_mode) {
  (void)in_batch_mode;
  if (p->status == S_INSTALLED) { pkg_msg("Package %s is already installed and configured.\n", p->name); return 0; }
  if (p->status == S_TRIGGERS_PENDING || p->status == S_TRIGGERS_AWAITED) return dpkg_triggers_run(db);
  if (p->status != S_UNPACKED && p->status != S_HALF_CONFIGURED) {
    pkg_error("package %s is not ready for configuration (current status '%s')", p->name, status_names[p->status]);
    return 1;
  }
  sbuf why = {0};
  int ok = dpkg_check_depends(db, p, "Pre-Depends", 1, &why) & dpkg_check_depends(db, p, "Depends", 1, &why);
  if (!ok) {
    if (pkg_cfg.force & F_DEPENDS) pkg_warn("dependency problems, configuring %s anyway:\n%s", p->name, why.s);
    else {
      pkg_error("dependency problems prevent configuration of %s:\n%s", p->name, why.s);
      sb_free(&why);
      return 1;
    }
  }
  sb_free(&why);
  pkg_msg("Setting up %s (%s) ...\n", p->name, pi_version(p));
  set_state(db, p, S_HALF_CONFIGURED, "configure");
  resolve_conffiles(p);
  db_save(db);
  const char *cv = ctl_get(&p->f, "Config-Version");
  char *cvc = xstrdup(cv ? cv : "");
  const char *args[] = {"configure", cvc, 0};
  int r = run_script(p, 0, "postinst", args, 0);
  free(cvc);
  if (r != 0) { pkg_error("error processing package %s (--configure): installed post-installation script failed", p->name); return 1; }
  ctl_del(&p->f, "Config-Version");
  ctl_del(&p->f, "Triggers-Pending");
  set_state(db, p, S_INSTALLED, "installed");
  return 0;
}

int dpkg_configure_pending(pkgdb *db) {
  /* Configure in dependency order: repeat passes, each configuring what is
   * now possible; then whatever is left (a cycle) with its batch partners. */
  int rc = 0, progress = 1;
  for (int i = 0; i < db->n; i++) if (db->p[i]->status == S_UNPACKED || db->p[i]->status == S_HALF_CONFIGURED) {
    char k[300];
    if (sl_find(&dpkg_batch, pi_key(db->p[i], k, sizeof k)) < 0) sl_add(&dpkg_batch, k);
  }
  while (progress) {
    progress = 0;
    for (int i = 0; i < db->n; i++) {
      pkginfo *p = db->p[i];
      if (p->status != S_UNPACKED && p->status != S_HALF_CONFIGURED) continue;
      /* strict pass: dependencies must be fully configured, not just in the batch */
      sbuf why = {0};
      strlist save = dpkg_batch;
      strlist none = {0};
      dpkg_batch = none;
      int ready = dpkg_check_depends(db, p, "Depends", 1, &why) && dpkg_check_depends(db, p, "Pre-Depends", 1, &why);
      dpkg_batch = save;
      sb_free(&why);
      if (!ready) continue;
      if (dpkg_configure(db, p, 1) == 0) progress = 1; else rc = 1;
    }
    if (!progress) {
      /* cycles: allow dependencies that are unpacked and in this batch */
      for (int i = 0; i < db->n; i++) {
        pkginfo *p = db->p[i];
        if (p->status != S_UNPACKED && p->status != S_HALF_CONFIGURED) continue;
        if (p->mark == 3) continue;
        p->mark = 3;
        if (dpkg_configure(db, p, 1) == 0) { progress = 1; break; }
        rc = 1;
      }
    }
  }
  for (int i = 0; i < db->n; i++) if (db->p[i]->mark == 3) db->p[i]->mark = 0;
  if (dpkg_triggers_run(db)) rc = 1;
  return rc;
}

/* ---- remove and purge ---- */
static int reverse_deps_block(pkgdb *db, pkginfo *p) {
  int blocked = 0;
  p->mark = 2;
  for (int i = 0; i < db->n; i++) {
    pkginfo *q = db->p[i];
    if (q == p || q->mark == 2 || q->status < S_UNPACKED) continue;
    const char *fields[] = {"Depends", "Pre-Depends"};
    for (int f = 0; f < 2; f++) {
      dep_list l;
      if (dep_parse(ctl_get(&q->f, fields[f]), DEP_ALT, &l, 0) < 0) continue;
      const char *qa = q->arch ? q->arch : pkg_cfg.arch;
      for (int j = 0; j < l.n; j++) {
        int uses_p = 0;
        for (int a = 0; a < l.o[j].n; a++) if (pkg_satisfies(p, &l.o[j].a[a], qa, 0)) uses_p = 1;
        if (!uses_p) continue;
        if (deps_satisfied_by_db(db, &l.o[j], qa, 0, 0)) continue;
        sbuf b = {0};
        dep_list one = {&l.o[j], 1};
        dep_format(&b, &one);
        if (pkg_cfg.force & F_DEPENDS) pkg_warn("%s depends on %s; removing anyway", q->name, b.s);
        else { pkg_error("dependency problems prevent removal of %s: %s depends on %s", p->name, q->name, b.s); blocked = 1; }
        sb_free(&b);
      }
      dep_free(&l);
    }
  }
  if (blocked) p->mark = 0;
  return blocked;
}

int dpkg_remove(pkgdb *db, pkginfo *p, int purge) {
  if (p->status == S_NOT_INSTALLED) { pkg_warn("ignoring request to remove %s which isn't installed", p->name); return 0; }
  const char *ess = ctl_get(&p->f, "Essential");
  if (ess && !strcasecmp(ess, "yes") && pi_present(p) && !(pkg_cfg.force & F_REMOVE_ESSENTIAL)) {
    pkg_error("this is an essential package; it should not be removed (use --force-remove-essential)");
    return 1;
  }
  if (p->eflag == E_REINSTREQ && !(pkg_cfg.force & F_REMOVE_REINSTREQ)) {
    pkg_error("package %s is in a very bad inconsistent state; reinstall it before removing (or --force-remove-reinstreq)", p->name);
    return 1;
  }
  if (pi_present(p)) {
    if (reverse_deps_block(db, p)) return 1;
    pkg_msg("Removing %s (%s) ...\n", p->name, pi_version(p));
    if (p->status >= S_HALF_CONFIGURED) {
      const char *a[] = {"remove", 0};
      if (run_script(p, 0, "prerm", a, 0) != 0) {
        const char *b[] = {"abort-remove", 0};
        run_script(p, 0, "postinst", b, 0);
        p->mark = 0;
        return 1;
      }
    }
    p->want = purge ? W_PURGE : W_DEINSTALL;
    set_state(db, p, S_HALF_INSTALLED, "remove");
    strlist l = {0};
    db_read_list(p, &l);
    conffile *cf;
    int ncf = conffiles_parse(ctl_get(&p->f, "Conffiles"), &cf);
    strmap owners = {0};
    db_file_owners(db, &owners, p);
    xqsort(l.v, l.n, sizeof *l.v, cmp_rev);
    strlist keep = {0};
    for (int i = 0; i < l.n; i++) {
      const char *f = l.v[i];
      if (!strcmp(f, "/.") || !strcmp(f, "/")) continue;
      int isconf = 0;
      for (int c = 0; c < ncf; c++) if (!strcmp(cf[c].path, f)) isconf = 1;
      char *full = root_path(f);
      if (isconf) { sl_add(&keep, f); free(full); continue; }
      if (sm_get(&owners, f)) { free(full); continue; }   /* shared directory, or taken over */
      if (!pkg_cfg.no_act) {
        char *nw = xasprintf("%s.dpkg-new", full), *tp = xasprintf("%s.dpkg-tmp", full);
        unlink(nw); unlink(tp);
        free(nw); free(tp);
        if (remove_tree_file(full) < 0 && errno != ENOENT) {
          if (errno == ENOTEMPTY || errno == EEXIST) { if (pkg_verbose) pkg_msg("directory %s not empty so not removed\n", f); sl_add(&keep, f); }
          else pkg_warn("while removing %s: %s", full, strerror(errno));
        } else trig_activate_file(db, f);
      }
      free(full);
    }
    sm_free(&owners);
    const char *a[] = {"remove", 0};
    run_script(p, 0, "postrm", a, 0);
    /* keep postrm, the list (conffiles and the directories that hold them) and conffiles */
    remove_info(p, 1);
    db_write_list(p, &keep);
    trig_unregister(p);
    sl_free(&l); sl_free(&keep);
    /* config-files while something is left for a purge to do: conffiles on
     * disk, or a postrm that must still be called with "purge" */
    int conf_left = 0;
    for (int c = 0; c < ncf; c++) { char *full = root_path(cf[c].path); if (path_exists(full)) conf_left = 1; free(full); }
    { char *pr = pi_infoname(p, "postrm"); if (path_exists(pr)) conf_left = 1; free(pr); }
    conffiles_free(cf, ncf);
    p->mark = 0;
    ctl_del(&p->f, "Triggers-Pending");
    if (conf_left || purge) { p->eflag = E_OK; set_state(db, p, S_CONFIG_FILES, "config-files"); }
    else {
      remove_info(p, 0);
      p->eflag = E_OK;
      set_state(db, p, S_NOT_INSTALLED, "not-installed");
      return 0;
    }
  }
  if (!purge) return 0;
  pkg_msg("Purging configuration files for %s (%s) ...\n", p->name, pi_version(p) ? pi_version(p) : "");
  conffile *cf;
  int ncf = conffiles_parse(ctl_get(&p->f, "Conffiles"), &cf);
  static const char *const suffixes[] = {"", ".dpkg-old", ".dpkg-new", ".dpkg-dist", ".dpkg-tmp", ".dpkg-bak", ".dpkg-remove", ".ucf-old", 0};
  for (int c = 0; c < ncf; c++) {
    char *full = root_path(cf[c].path);
    for (int s = 0; suffixes[s]; s++) { char *f = xasprintf("%s%s", full, suffixes[s]); if (!pkg_cfg.no_act) unlink(f); free(f); }
    free(full);
  }
  conffiles_free(cf, ncf);
  strlist l = {0};
  db_read_list(p, &l);
  xqsort(l.v, l.n, sizeof *l.v, cmp_rev);
  strmap owners = {0};
  db_file_owners(db, &owners, p);
  for (int i = 0; i < l.n; i++) {
    if (sm_get(&owners, l.v[i]) || !strcmp(l.v[i], "/.")) continue;
    char *full = root_path(l.v[i]);
    if (!pkg_cfg.no_act && path_is_dir(full)) rmdir(full);
    free(full);
  }
  sm_free(&owners);
  sl_free(&l);
  const char *a[] = {"purge", 0};
  run_script(p, 0, "postrm", a, 0);
  remove_info(p, 0);
  ctl_del(&p->f, "Conffiles");
  ctl_del(&p->f, "Config-Version");
  p->want = W_PURGE; p->eflag = E_OK;
  set_state(db, p, S_NOT_INSTALLED, "purged");
  return 0;
}
