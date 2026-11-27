// SPDX-License-Identifier: GPL-2.0-or-later
/* Command lines of dpkg, dpkg-query and dpkg-deb. Option spellings and output
 * formats follow dpkg(1), dpkg-query(1) and dpkg-deb(1) so that Debian
 * documentation and scripts apply. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

int pkg_glob(const char *pat, const char *s);
void db_write_pkg(sbuf *b, const pkginfo *p);

/* ---------------- dpkg-deb ---------------- */
static void mode_str(const tar_entry *e, char *m) {
  char t = '-';
  switch (e->type) { case '5': t = 'd'; break; case '2': t = 'l'; break; case '1': t = 'h'; break; case '3': t = 'c'; break; case '4': t = 'b'; break; case '6': t = 'p'; break; }
  m[0] = t;
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) m[i + 1] = (e->mode >> (8 - i)) & 1 ? rwx[i] : '-';
  if (e->mode & 04000) m[3] = m[3] == 'x' ? 's' : 'S';
  if (e->mode & 02000) m[6] = m[6] == 'x' ? 's' : 'S';
  if (e->mode & 01000) m[9] = m[9] == 'x' ? 't' : 'T';
  m[10] = 0;
}
static void list_entry(const tar_entry *e) {
  char m[11], ug[80], sz[32], date[32];
  mode_str(e, m);
  snprintf(ug, sizeof ug, "%s/%s", e->uname[0] ? e->uname : "0", e->gname[0] ? e->gname : "0");
  snprintf(sz, sizeof sz, "%llu", (unsigned long long)(e->type == '0' || e->type == '7' ? e->size : 0));
  time_t t = e->mtime;
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
  int pad = 19 - (int)strlen(ug) - (int)strlen(sz);
  if (pad < 1) pad = 1;
  printf("%s %s%*s%s %s %s", m, ug, pad, "", sz, date, e->path);
  if (e->type == '2') printf(" -> %s", e->link);
  if (e->type == '1') printf(" link to %s", e->link);
  printf("\n");
}

static int extract_to(deb *d, const char *dir, int verbose) {
  char *err;
  if (deb_data_begin(d, 0, &err) < 0) { pkg_error("%s", err); free(err); return 1; }
  mkdir_p(dir, 0755);
  tar_entry *e = xmalloc(sizeof *e);
  int k, rc = 0;
  char *buf = xmalloc(65536);
  while ((k = tar_next(d->data, e)) == 1) {
    char np[4096];
    if (!deb_norm_path(e->path, np, sizeof np)) { pkg_error("unsafe path %s", e->path); rc = 1; break; }
    if (verbose) printf("%s\n", e->path);
    if (!strcmp(np, "/")) continue;
    char *full = path_join(dir, np), *pd = path_dirname(full);
    mkdir_p(pd, 0755);
    free(pd);
    if (e->type == '5') { if (mkdir(full, e->mode & 07777) < 0 && errno != EEXIST) rc = 1; chmod(full, e->mode & 07777); }
    else if (e->type == '0' || e->type == '7') {
      unlink(full);
      int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, e->mode & 0777);
      if (fd < 0) { pkg_error("cannot create %s: %s", full, strerror(errno)); rc = 1; free(full); continue; }
      long r;
      while ((r = tar_read(d->data, buf, 65536)) > 0) if (write(fd, buf, r) != r) { rc = 1; break; }
      if (r < 0) rc = 1;
      close(fd);
      if (chmod(full, e->mode & 07777) < 0 && errno == ENOSYS) pkg_unsupported("set file modes", full);
    } else if (e->type == '2') {
      unlink(full);
      if (symlink(e->link, full) < 0) { if (errno == ENOSYS) pkg_unsupported("create symbolic links", full); else rc = 1; }
    } else if (e->type == '1') {
      char tp[4096];
      if (deb_norm_path(e->link, tp, sizeof tp)) {
        char *t = path_join(dir, tp);
        unlink(full);
        if (link(t, full) < 0) file_copy(t, full, 0644);
        free(t);
      }
    }
    free(full);
  }
  if (k < 0) { pkg_error("corrupt data archive: %s", arc_error(d->data)); rc = 1; }
  free(buf); free(e);
  return rc;
}

static int script_interp(const deb_cfile *c, char *out, size_t cap) {
  out[0] = 0;
  if (c->size > 2 && c->data[0] == '#' && c->data[1] == '!') {
    size_t n = 0;
    while (n < c->size && c->data[n] != '\n' && n + 1 < cap) { out[n] = c->data[n]; n++; }
    out[n] = 0;
    return 1;
  }
  return 0;
}

static int deb_info(const char *path, char **files, int nfiles) {
  char *err;
  deb *d = deb_open(path, &err);
  if (!d) { pkg_error("%s", err); free(err); return 2; }
  if (nfiles) {
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
      const deb_cfile *c = deb_cfile_get(d, files[i]);
      if (!c) { pkg_error("%s does not contain a control component '%s'", path, files[i]); rc = 1; continue; }
      fwrite(c->data, 1, c->size, stdout);
    }
    deb_close(d);
    return rc;
  }
  printf(" new Debian package, version %s.\n", d->fmtver);
  printf(" size %llu bytes: control archive=%llu bytes.\n", (unsigned long long)d->file_size, (unsigned long long)d->control_size);
  for (int i = 0; i < d->ncf; i++) {
    const deb_cfile *c = &d->cf[i];
    int lines = 0;
    for (size_t j = 0; j < c->size; j++) if (c->data[j] == '\n') lines++;
    char in[128];
    int scr = script_interp(c, in, sizeof in);
    printf(" %7lu bytes, %5d lines   %c  %-20s%s%s\n", (unsigned long)c->size, lines, (c->mode & 0111) ? '*' : ' ', c->name, scr ? " " : "", scr ? in : "");
  }
  sbuf b = {0};
  ctl_write(&b, &d->control);
  for (char *s = b.s, *e; s && *s; s = e) {
    e = strchr(s, '\n'); if (e) *e++ = 0; else e = s + strlen(s);
    printf(" %s\n", s);
  }
  sb_free(&b);
  deb_close(d);
  return 0;
}

static int deb_contents(const char *path) {
  char *err;
  deb *d = deb_open(path, &err);
  if (!d) { pkg_error("%s", err); free(err); return 2; }
  if (deb_data_begin(d, 0, &err) < 0) { pkg_error("%s", err); free(err); deb_close(d); return 2; }
  tar_entry *e = xmalloc(sizeof *e);
  int k;
  while ((k = tar_next(d->data, e)) == 1) list_entry(e);
  int rc = 0;
  if (k < 0) { pkg_error("corrupt data archive: %s", arc_error(d->data)); rc = 2; }
  free(e);
  deb_close(d);
  return rc;
}

static int deb_field(const char *path, char **fields, int n) {
  char *err;
  deb *d = deb_open(path, &err);
  if (!d) { pkg_error("%s", err); free(err); return 2; }
  if (!n) { sbuf b = {0}; ctl_write(&b, &d->control); fputs(b.s, stdout); sb_free(&b); }
  else for (int i = 0; i < n; i++) {
    const char *v = ctl_get(&d->control, fields[i]);
    if (!v) continue;
    if (n == 1) printf("%s\n", v);
    else printf("%s:%s%s\n", fields[i], *v && *v != '\n' ? " " : "", v);
  }
  deb_close(d);
  return 0;
}

static int deb_control(const char *path, const char *dir) {
  char *err;
  deb *d = deb_open(path, &err);
  if (!d) { pkg_error("%s", err); free(err); return 2; }
  if (mkdir_p(dir, 0755) < 0) { pkg_error("cannot create %s", dir); deb_close(d); return 2; }
  for (int i = 0; i < d->ncf; i++) {
    char *f = path_join(dir, d->cf[i].name);
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, d->cf[i].mode & 0777);
    if (fd >= 0) { if (write(fd, d->cf[i].data, d->cf[i].size) < 0) {} close(fd); chmod(f, d->cf[i].mode & 0777); }
    free(f);
  }
  deb_close(d);
  return 0;
}

static int deb_fsys_tar(const char *path) {
  char *err;
  deb *d = deb_open(path, &err);
  if (!d) { pkg_error("%s", err); free(err); return 2; }
  if (deb_data_begin(d, 1, &err) < 0) { pkg_error("%s", err); free(err); deb_close(d); return 2; }
  char *buf = xmalloc(65536);
  long r;
  fflush(stdout);
  while ((r = arc_read(d->data, buf, 65536)) > 0) if (write(1, buf, r) != r) break;
  free(buf);
  int rc = r < 0 ? 2 : 0;
  deb_close(d);
  return rc;
}

static void deb_usage(void) {
  printf("Usage: dpkg-deb [OPTION...] COMMAND\n\n"
         "Commands:\n"
         "  -I|--info FILE.deb [CONTROLFILE...]   show information about a package\n"
         "  -c|--contents FILE.deb                list its files\n"
         "  -W|--show FILE.deb                    show name and version\n"
         "  -f|--field FILE.deb [FIELD...]        show control fields\n"
         "  -e|--control FILE.deb [DIR]           extract control files (default DEBIAN)\n"
         "  -x|--extract FILE.deb DIR             extract the files\n"
         "  -X|--vextract FILE.deb DIR            extract and list the files\n"
         "  -R|--raw-extract FILE.deb DIR         extract files and control files (DIR/DEBIAN)\n"
         "  --fsys-tarfile FILE.deb               write the uncompressed file archive to stdout\n"
         "Not implemented: --build, --ctrl-tarfile.\n");
}

int dpkg_deb_main(int argc, char **argv) {
  pkg_progname = "dpkg-deb";
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) { deb_usage(); return 0; }
    if (!strcmp(a, "--version")) { printf("dpkg-deb (UC.OS libpkg) 0.1\n"); return 0; }
    if (!strcmp(a, "-I") || !strcmp(a, "--info")) { if (i + 1 >= argc) break; return deb_info(argv[i + 1], argv + i + 2, argc - i - 2); }
    if (!strcmp(a, "-c") || !strcmp(a, "--contents")) { if (i + 1 >= argc) break; return deb_contents(argv[i + 1]); }
    if (!strcmp(a, "-f") || !strcmp(a, "--field")) { if (i + 1 >= argc) break; return deb_field(argv[i + 1], argv + i + 2, argc - i - 2); }
    if (!strcmp(a, "-W") || !strcmp(a, "--show")) {
      if (i + 1 >= argc) break;
      char *err; deb *d = deb_open(argv[i + 1], &err);
      if (!d) { pkg_error("%s", err); free(err); return 2; }
      printf("%s\t%s\n", ctl_get(&d->control, "Package"), ctl_get(&d->control, "Version") ? ctl_get(&d->control, "Version") : "");
      deb_close(d); return 0;
    }
    if (!strcmp(a, "-e") || !strcmp(a, "--control")) { if (i + 1 >= argc) break; return deb_control(argv[i + 1], i + 2 < argc ? argv[i + 2] : "DEBIAN"); }
    if (!strcmp(a, "-x") || !strcmp(a, "--extract") || !strcmp(a, "-X") || !strcmp(a, "--vextract") || !strcmp(a, "-R") || !strcmp(a, "--raw-extract")) {
      if (i + 2 >= argc) { pkg_error("--%s needs a .deb file and a target directory", a); return 2; }
      char *err; deb *d = deb_open(argv[i + 1], &err);
      if (!d) { pkg_error("%s", err); free(err); return 2; }
      int verbose = !strcmp(a, "-X") || !strcmp(a, "--vextract");
      int rc = extract_to(d, argv[i + 2], verbose);
      deb_close(d);
      if (!rc && (!strcmp(a, "-R") || !strcmp(a, "--raw-extract"))) { char *c = path_join(argv[i + 2], "DEBIAN"); rc = deb_control(argv[i + 1], c); free(c); }
      pkg_unsupported_report();
      return rc;
    }
    if (!strcmp(a, "--fsys-tarfile")) { if (i + 1 >= argc) break; return deb_fsys_tar(argv[i + 1]); }
    if (!strcmp(a, "-b") || !strcmp(a, "--build") || !strcmp(a, "--ctrl-tarfile")) { pkg_error("%s is not implemented yet", a); return 2; }
    pkg_error("unknown option %s", a);
    return 2;
  }
  pkg_error("need an action option");
  deb_usage();
  return 2;
}

/* ---------------- queries ---------------- */
static const char want_ch[] = "uihrp", stat_ch[] = "ncHUFWti";
static int pkg_cmp_name(const void *a, const void *b) {
  const pkginfo *x = *(pkginfo *const *)a, *y = *(pkginfo *const *)b;
  int c = strcmp(x->name, y->name);
  return c ? c : strcmp(x->arch ? x->arch : "", y->arch ? y->arch : "");
}
static int matches_any(const pkginfo *p, char **pats, int n) {
  if (!n) return 1;
  for (int i = 0; i < n; i++) {
    char k[300];
    snprintf(k, sizeof k, "%s:%s", p->name, p->arch ? p->arch : "");
    if (pkg_glob(pats[i], p->name) || pkg_glob(pats[i], k)) return 1;
  }
  return 0;
}
static int cmd_list(pkgdb *db, char **pats, int n) {
  xqsort(db->p, db->n, sizeof *db->p, pkg_cmp_name);
  int nw = 14, vw = 12, aw = 12, shown = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (!matches_any(p, pats, n)) continue;
    if (!n && p->status == S_NOT_INSTALLED) continue;
    int l = strlen(p->name); if (l > nw) nw = l;
    const char *v = pi_version(p); if (v && (int)strlen(v) > vw) vw = strlen(v);
  }
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (!matches_any(p, pats, n) || (!n && p->status == S_NOT_INSTALLED)) continue;
    if (!shown++) {
      printf("Desired=Unknown/Install/Remove/Purge/Hold\n"
             "| Status=Not/Inst/Conf-files/Unpacked/halF-conf/Half-inst/trig-aWait/Trig-pend\n"
             "|/ Err?=(none)/Reinst-required (Status,Err: uppercase=bad)\n");
      printf("||/ %-*s %-*s %-*s %s\n", nw, "Name", vw, "Version", aw, "Architecture", "Description");
      printf("+++-");
      int widths[3] = {nw, vw, aw};
      for (int w = 0; w < 3; w++) { for (int j = 0; j < widths[w]; j++) putchar('='); putchar('-'); }
      printf("==============================\n");
    }
    const char *d = ctl_get(&p->f, "Description");
    char desc[61] = "";
    if (d) { size_t l = strcspn(d, "\n"); snprintf(desc, sizeof desc, "%.*s", (int)(l > 60 ? 60 : l), d); }
    printf("%c%c%c %-*s %-*s %-*s %s\n", want_ch[p->want], stat_ch[p->status], p->eflag ? 'R' : ' ', nw, p->name, vw,
           pi_version(p) ? pi_version(p) : "<none>", aw, p->arch ? p->arch : "<none>", d ? desc : "(no description available)");
  }
  if (!shown) {
    if (n) { for (int i = 0; i < n; i++) fprintf(stderr, "dpkg-query: no packages found matching %s\n", pats[i]); return 1; }
  }
  return 0;
}
static pkginfo *find_arg(pkgdb *db, const char *arg) {
  char name[256];
  snprintf(name, sizeof name, "%s", arg);
  char *colon = strchr(name, ':');
  if (colon) { *colon = 0; return db_find(db, name, colon + 1); }
  return db_find(db, name, 0);
}
static int cmd_status(pkgdb *db, char **args, int n) {
  int rc = 0;
  for (int i = 0; i < n; i++) {
    pkginfo *p = find_arg(db, args[i]);
    if (!p || (p->status == S_NOT_INSTALLED && p->want == W_UNKNOWN)) { fprintf(stderr, "dpkg-query: package '%s' is not installed and no information is available\n", args[i]); rc = 1; continue; }
    sbuf b = {0};
    db_write_pkg(&b, p);
    if (i) putchar('\n');
    fputs(b.s, stdout);
    sb_free(&b);
  }
  return rc;
}
static int cmd_listfiles(pkgdb *db, char **args, int n) {
  int rc = 0;
  for (int i = 0; i < n; i++) {
    pkginfo *p = find_arg(db, args[i]);
    if (!p || p->status == S_NOT_INSTALLED) { fprintf(stderr, "dpkg-query: package '%s' is not installed\n", args[i]); rc = 1; continue; }
    strlist l = {0};
    if (db_read_list(p, &l) < 0 || !l.n) printf("Package '%s' does not contain any files (!)\n", p->name);
    for (int j = 0; j < l.n; j++) printf("%s\n", l.v[j]);
    sl_free(&l);
  }
  return rc;
}
static int cmd_search(pkgdb *db, char **pats, int n) {
  int rc = 0;
  for (int k = 0; k < n; k++) {
    const char *pat = pats[k];
    int glob = strpbrk(pat, "*?[") != 0;
    char *sub = glob || pat[0] == '/' ? 0 : xasprintf("*%s*", pat);
    strmap seen = {0};
    int found = 0;
    for (int i = 0; i < db->n; i++) {
      pkginfo *p = db->p[i];
      if (p->status == S_NOT_INSTALLED) continue;
      strlist l = {0};
      db_read_list(p, &l);
      for (int j = 0; j < l.n; j++) {
        int m = sub ? pkg_glob(sub, l.v[j]) : glob ? pkg_glob(pat, l.v[j]) : !strcmp(pat, l.v[j]);
        if (!m) continue;
        void **s = sm_slot(&seen, l.v[j]);
        sbuf *b = *s;
        if (!b) { b = xcalloc(1, sizeof *b); *s = b; sb_puts(b, p->name); }
        else sb_printf(b, ", %s", p->name);
        found = 1;
      }
      sl_free(&l);
    }
    strlist keys = {0};
    for (uint32_t i = 0; i < seen.cap; i++) if (seen.k && seen.k[i]) sl_add(&keys, seen.k[i]);
    sl_sort(&keys);
    for (int i = 0; i < keys.n; i++) { sbuf *b = sm_get(&seen, keys.v[i]); printf("%s: %s\n", b->s, keys.v[i]); sb_free(b); free(b); }
    sl_free(&keys);
    sm_free(&seen);
    free(sub);
    if (!found) { fprintf(stderr, "dpkg-query: no path found matching pattern %s\n", pat); rc = 1; }
  }
  return rc;
}
static void show_format(const pkginfo *p, const char *fmt) {
  sbuf out = {0};
  for (const char *s = fmt; *s; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      sb_putc(&out, *s == 'n' ? '\n' : *s == 't' ? '\t' : *s);
      continue;
    }
    if (*s == '$' && s[1] == '{') {
      const char *e = strchr(s, '}');
      if (!e) { sb_puts(&out, s); break; }
      char name[128];
      snprintf(name, sizeof name, "%.*s", (int)(e - s - 2), s + 2);
      int width = 0;
      char *semi = strchr(name, ';');
      if (semi) { *semi = 0; width = atoi(semi + 1); }
      char tmp[400];
      const char *v = 0;
      if (!strcasecmp(name, "Package")) v = p->name;
      else if (!strcasecmp(name, "binary:Package")) { char k[300]; v = strcpy(tmp, pi_key(p, k, sizeof k)); }
      else if (!strcasecmp(name, "Status")) { snprintf(tmp, sizeof tmp, "%s %s %s", want_names[p->want], eflag_names[p->eflag], status_names[p->status]); v = tmp; }
      else if (!strcasecmp(name, "db:Status-Abbrev")) { snprintf(tmp, sizeof tmp, "%c%c%c", want_ch[p->want], stat_ch[p->status], p->eflag ? 'R' : ' '); v = tmp; }
      else if (!strcasecmp(name, "db:Status-Want")) v = want_names[p->want];
      else if (!strcasecmp(name, "db:Status-Eflag")) v = eflag_names[p->eflag];
      else if (!strcasecmp(name, "db:Status-Status")) v = status_names[p->status];
      else if (!strcasecmp(name, "binary:Summary")) { const char *d = ctl_get(&p->f, "Description"); snprintf(tmp, sizeof tmp, "%.*s", d ? (int)strcspn(d, "\n") : 0, d ? d : ""); v = tmp; }
      else v = ctl_get(&p->f, name);
      if (!v) v = "";
      if (width > 0) sb_printf(&out, "%-*s", width, v);
      else if (width < 0) sb_printf(&out, "%*s", -width, v);
      else sb_puts(&out, v);
      s = e;
      continue;
    }
    sb_putc(&out, *s);
  }
  if (out.s) fputs(out.s, stdout);
  sb_free(&out);
}
static int cmd_show(pkgdb *db, char **pats, int n, const char *fmt) {
  if (!fmt) fmt = "${binary:Package}\t${Version}\n";
  xqsort(db->p, db->n, sizeof *db->p, pkg_cmp_name);
  int any = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (!n && p->status == S_NOT_INSTALLED) continue;
    if (!matches_any(p, pats, n)) continue;
    show_format(p, fmt);
    any = 1;
  }
  if (n && !any) { for (int i = 0; i < n; i++) fprintf(stderr, "dpkg-query: no packages found matching %s\n", pats[i]); return 1; }
  return 0;
}
static int cmd_control_path(pkgdb *db, const char *pkg, const char *file) {
  pkginfo *p = find_arg(db, pkg);
  if (!p || p->status == S_NOT_INSTALLED) { fprintf(stderr, "dpkg-query: package '%s' is not installed\n", pkg); return 1; }
  static const char *const ex[] = {"preinst", "postinst", "prerm", "postrm", "md5sums", "conffiles", "triggers", "shlibs", "symbols", "templates", "config", 0};
  for (int i = 0; ex[i]; i++) {
    if (file && strcmp(file, ex[i])) continue;
    char *f = pi_infoname(p, ex[i]);
    if (path_exists(f)) printf("%s\n", f);
    free(f);
  }
  return 0;
}
static int cmd_audit(pkgdb *db) {
  int bad = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    const char *why = 0;
    if (p->eflag == E_REINSTREQ) why = "needs to be reinstalled (it is broken)";
    else if (p->status == S_HALF_INSTALLED) why = "is only half installed";
    else if (p->status == S_UNPACKED) why = "is unpacked but not configured";
    else if (p->status == S_HALF_CONFIGURED) why = "is only half configured";
    else if (p->status == S_TRIGGERS_PENDING) why = "has triggers pending";
    if (!why) continue;
    printf(" %-20s %s\n", p->name, why);
    bad = 1;
  }
  return bad;
}
static int cmd_verify(pkgdb *db, char **args, int n) {
  int rc = 0;
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (!pi_present(p) || !matches_any(p, args, n)) continue;
    char *f = pi_infoname(p, "md5sums"), *data;
    if (file_read(f, &data, 0) == 0) {
      for (char *line = strtok(data, "\n"); line; line = strtok(0, "\n")) {
        char sum[40], path[4096];
        if (sscanf(line, "%39s %4095[^\n]", sum, path) != 2) continue;
        char *pp = path; while (*pp == ' ') pp++;
        char *full = root_path(pp), h[65];
        if (hex_digest_file(full, 1, h) < 0) { printf("missing   /%s\n", pp); rc = 1; }
        else if (strcmp(h, sum)) { printf("??5??????   /%s\n", pp); rc = 1; }
        free(full);
      }
      free(data);
    }
    free(f);
  }
  return rc;
}
static int cmd_selections(pkgdb *db, char **pats, int n) {
  xqsort(db->p, db->n, sizeof *db->p, pkg_cmp_name);
  for (int i = 0; i < db->n; i++) {
    pkginfo *p = db->p[i];
    if (p->want == W_UNKNOWN || !matches_any(p, pats, n)) continue;
    char k[300];
    printf("%-50s %s\n", pi_key(p, k, sizeof k), want_names[p->want]);
  }
  return 0;
}
static int cmd_set_selections(pkgdb *db) {
  char *line = 0; size_t cap = 0;
  while (getline(&line, &cap, stdin) > 0) {
    char name[256], want[32];
    if (line[0] == '#' || sscanf(line, "%255s %31s", name, want) != 2) continue;
    int w = -1;
    for (int i = 0; want_names[i]; i++) if (!strcmp(want_names[i], want)) w = i;
    if (w < 0) { pkg_warn("unknown wanted status '%s'", want); continue; }
    char *colon = strchr(name, ':');
    pkginfo *p = colon ? (*colon = 0, db_get(db, name, colon + 1)) : db_get(db, name, 0);
    p->want = w;
    db->dirty = 1;
  }
  free(line);
  return db_save(db) < 0;
}
struct avail_ctx { strmap *seen; sbuf *out; int pass; };
static int avail_fn(ctl_para *p, void *arg) {
  struct avail_ctx *c = arg;
  const char *n = ctl_get(p, "Package");
  if (!n) return 0;
  void **s = sm_slot(c->seen, n);
  if (*s && c->pass == 1) return 0;  /* new information wins over the old available file */
  *s = (void *)1;
  ctl_write(c->out, p);
  sb_putc(c->out, '\n');
  return 0;
}
static int avail_update(int merge, const char *file) {
  char *av = path_join(pkg_cfg.admindir, "available");
  strmap seen = {0};
  sbuf out = {0};
  int rc = 0;
  struct avail_ctx c = {&seen, &out, 0};
  if (ctl_parse_file(file, avail_fn, &c) != 0) rc = 1;
  if (merge) { c.pass = 1; ctl_parse_file(av, avail_fn, &c); }
  if (!rc && !pkg_cfg.no_act) { mkdir_p(pkg_cfg.admindir, 0755); file_write_atomic(av, out.s ? out.s : "", out.n, 0644); }
  sm_free(&seen); sb_free(&out); free(av);
  return rc;
}
static int cmd_print_avail(char **args, int n) {
  char *av = path_join(pkg_cfg.admindir, "available"), *data;
  int rc = 1;
  if (file_read(av, &data, 0) == 0) {
    ctl_reader *r = ctl_open_mem(data, strlen(data));
    ctl_para p = {0};
    while (ctl_next(r, &p) == 1)
      for (int i = 0; i < n; i++) if (ctl_get(&p, "Package") && !strcmp(ctl_get(&p, "Package"), args[i])) { sbuf b = {0}; ctl_write(&b, &p); fputs(b.s, stdout); sb_free(&b); rc = 0; }
    ctl_free(&p); ctl_close(r); free(data);
  }
  if (rc) for (int i = 0; i < n; i++) fprintf(stderr, "dpkg-query: package '%s' is not available\n", args[i]);
  free(av);
  return rc;
}

/* ---------------- dpkg ---------------- */
static void dpkg_usage(void) {
  printf("Usage: dpkg [OPTION...] ACTION\n\n"
         "Actions:\n"
         "  -i|--install FILE.deb...       unpack and configure\n"
         "  --unpack FILE.deb...           unpack only\n"
         "  --configure PACKAGE...|-a      configure unpacked packages\n"
         "  -r|--remove PACKAGE...         remove, keep configuration files\n"
         "  -P|--purge PACKAGE...          remove everything\n"
         "  --triggers-only PACKAGE...|-a  run pending triggers\n"
         "  -l|--list [PATTERN...]         list packages\n"
         "  -s|--status PACKAGE...         show status\n"
         "  -L|--listfiles PACKAGE...      list the files of a package\n"
         "  -S|--search PATTERN...         find the package owning a file\n"
         "  -p|--print-avail PACKAGE...    show available version details\n"
         "  -C|--audit                     find broken packages\n"
         "  -V|--verify [PACKAGE...]       check installed files against md5sums\n"
         "  --get-selections [PATTERN...]  --set-selections\n"
         "  --merge-avail FILE  --update-avail FILE  --clear-avail\n"
         "  --print-architecture  --print-foreign-architectures\n"
         "  --add-architecture ARCH  --remove-architecture ARCH\n"
         "  --compare-versions A OP B      OP: lt le eq ne ge gt (empty is lowest), lt-nl le-nl ge-nl gt-nl, < << <= = >= >> >\n"
         "  -I|--info  -c|--contents  -x|--extract  -X  -e|--control  -f|--field  (as dpkg-deb)\n\n"
         "Options:\n"
         "  --root=DIR  --instdir=DIR  --admindir=DIR  --no-act  --force-THING  --refuse-THING\n"
         "  --force-help lists the THINGs. Defaults come from %s/dpkg.cfg.\n", pkg_cfg.confdir ? pkg_cfg.confdir : "/etc/dpkg");
}

static int compare_versions(const char *a, const char *op, const char *b) {
  const char *why;
  if (*a && (why = ver_check(a)) && strcmp(why, "version number does not start with digit")) { pkg_error("version '%s' has bad syntax: %s", a, why); return 2; }
  if (*b && (why = ver_check(b)) && strcmp(why, "version number does not start with digit")) { pkg_error("version '%s' has bad syntax: %s", b, why); return 2; }
  int nl = strstr(op, "-nl") != 0;
  int c;
  if (!*a || !*b) {
    if (!*a && !*b) c = 0;
    else if (!*a) c = nl ? 1 : -1;
    else c = nl ? -1 : 1;
  } else c = ver_cmp(a, b);
  int r;
  if (!strcmp(op, "lt") || !strcmp(op, "lt-nl") || !strcmp(op, "<<")) r = c < 0;
  else if (!strcmp(op, "le") || !strcmp(op, "le-nl") || !strcmp(op, "<=") || !strcmp(op, "<")) r = c <= 0;
  else if (!strcmp(op, "eq") || !strcmp(op, "=")) r = c == 0;
  else if (!strcmp(op, "ne")) r = c != 0;
  else if (!strcmp(op, "ge") || !strcmp(op, "ge-nl") || !strcmp(op, ">=") || !strcmp(op, ">")) r = c >= 0;
  else if (!strcmp(op, "gt") || !strcmp(op, "gt-nl") || !strcmp(op, ">>")) r = c > 0;
  else { pkg_error("unknown operator '%s'", op); return 2; }
  return r ? 0 : 1;
}

static int write_arch_file(void) {
  sbuf b = {0};
  sb_printf(&b, "%s\n", pkg_cfg.arch);
  for (int i = 0; i < pkg_cfg.foreign.n; i++) sb_printf(&b, "%s\n", pkg_cfg.foreign.v[i]);
  mkdir_p(pkg_cfg.admindir, 0755);
  char *af = path_join(pkg_cfg.admindir, "arch");
  int r = file_write_atomic(af, b.s, b.n, 0644);
  free(af); sb_free(&b);
  return r < 0;
}

static int is_dir_arg(const char *p) { return path_is_dir(p); }
static void collect_debs(const char *dir, strlist *out) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  strlist names = {0};
  while ((e = readdir(d))) if (e->d_name[0] != '.') sl_add(&names, e->d_name);
  closedir(d);
  sl_sort(&names);
  for (int i = 0; i < names.n; i++) {
    char *p = path_join(dir, names.v[i]);
    if (path_is_dir(p)) collect_debs(p, out);
    else if (strlen(names.v[i]) > 4 && !strcmp(names.v[i] + strlen(names.v[i]) - 4, ".deb")) sl_add(out, p);
    free(p);
  }
  sl_free(&names);
}

int dpkg_main(int argc, char **argv, int as_query) {
  pkg_progname = as_query ? "dpkg-query" : "dpkg";
  pkg_config_init();
  const char *action = 0, *showfmt = 0;
  int recursive = 0, all = 0;
  strlist args = {0};
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (a[0] != '-' || !a[1]) { sl_add(&args, a); continue; }
    if (!strcmp(a, "--")) { for (i++; i < argc; i++) sl_add(&args, argv[i]); break; }
    if (a[1] != '-') {
      /* short options, possibly bundled: -iR, -l */
      for (char *c = a + 1; *c; c++) {
        switch (*c) {
        case 'i': action = "install"; break;
        case 'r': action = "remove"; break;
        case 'P': action = "purge"; break;
        case 'l': action = "list"; break;
        case 'L': action = "listfiles"; break;
        case 's': action = "status"; break;
        case 'S': action = "search"; break;
        case 'p': action = "print-avail"; break;
        case 'C': action = "audit"; break;
        case 'V': action = "verify"; break;
        case 'W': action = "show"; break;
        case 'c': action = as_query ? "control-path" : "contents"; break;
        case 'I': action = "info"; break;
        case 'x': action = "extract"; break;
        case 'X': action = "vextract"; break;
        case 'e': action = "control"; break;
        case 'f':
          if (as_query) { showfmt = c[1] ? c + 1 : (i + 1 < argc ? argv[++i] : ""); while (c[1]) c++; }
          else action = "field";
          break;
        case 'R': recursive = 1; break;
        case 'a': all = 1; break;
        case 'G': break;
        case 'E': break;
        case 'O': break;
        case 'B': break;
        case 'D': pkg_verbose = 1; while (c[1]) c++; break;
        case 'h': dpkg_usage(); return 0;
        default: pkg_error("unknown option -%c", *c); return 2;
        }
      }
      continue;
    }
    char *opt = a + 2, *val = strchr(opt, '=');
    char optname[128];
    snprintf(optname, sizeof optname, "%.*s", (int)(val ? val - opt : (long)strlen(opt)), opt);
    if (val) val++;
    static const char *const actions[] = {"install", "unpack", "configure", "remove", "purge", "list", "listfiles", "status", "search",
                                          "print-avail", "audit", "verify", "get-selections", "set-selections", "merge-avail", "update-avail",
                                          "clear-avail", "print-architecture", "print-foreign-architectures", "add-architecture",
                                          "remove-architecture", "compare-versions", "info", "contents", "extract", "vextract", "control",
                                          "field", "show", "control-path", "control-list", "control-show", "triggers-only", "fsys-tarfile",
                                          "raw-extract", "validate-pkgname", "validate-version", 0};
    int is_action = 0;
    for (int k = 0; actions[k]; k++) if (!strcmp(optname, actions[k])) { action = actions[k]; is_action = 1; }
    if (is_action) { if (val) sl_add(&args, val); continue; }
    if (!strcmp(optname, "help")) { dpkg_usage(); return 0; }
    if (!strcmp(optname, "version")) { printf("Debian '%s' package management program version 1.22.0-ucos (libpkg 0.1).\n", pkg_progname); return 0; }
    if (!strcmp(optname, "force-help")) { fputs(pkg_force_help(), stdout); return 0; }
    if (!strcmp(optname, "recursive")) { recursive = 1; continue; }
    if (!strcmp(optname, "pending")) { all = 1; continue; }
    if (!strcmp(optname, "showformat")) { if (!val && i + 1 < argc) val = argv[++i]; showfmt = val; continue; }
    if (!strcmp(optname, "verbose")) { pkg_verbose = 1; continue; }
    if (!strcmp(optname, "debug")) { pkg_verbose = 1; continue; }
    int needs_val = !strcmp(optname, "root") || !strcmp(optname, "instdir") || !strcmp(optname, "admindir") || !strcmp(optname, "log") ||
                    !strcmp(optname, "path-exclude") || !strcmp(optname, "path-include") || !strcmp(optname, "script-runner") ||
                    !strcmp(optname, "native-architecture");
    if (needs_val && !val) { if (i + 1 >= argc) { pkg_error("--%s needs a value", optname); return 2; } val = argv[++i]; }
    int r = pkg_config_option(optname, val);
    if (r == 0) { pkg_error("unknown option --%s", optname); return 2; }
    if (r < 0) return 2;
  }
  if (!action) { if (as_query) { pkg_error("need an action option"); return 2; } dpkg_usage(); return 2; }

  /* actions that need no database */
  if (!strcmp(action, "compare-versions")) {
    if (args.n != 3) { pkg_error("--compare-versions takes three arguments: <version> <relation> <version>"); return 2; }
    return compare_versions(args.v[0], args.v[1], args.v[2]);
  }
  if (!strcmp(action, "validate-version")) { const char *w = args.n ? ver_check(args.v[0]) : "missing"; if (w) { pkg_error("%s", w); return 1; } return 0; }
  if (!strcmp(action, "validate-pkgname")) { dep_list l; char *e; int r = args.n ? dep_parse(args.v[0], 0, &l, &e) : -1; if (r == 0) dep_free(&l); return r == 0 && l.n == 1 ? 0 : 1; }
  if (!strcmp(action, "info") || !strcmp(action, "contents") || !strcmp(action, "extract") || !strcmp(action, "vextract") ||
      !strcmp(action, "control") || !strcmp(action, "field") || !strcmp(action, "fsys-tarfile") || !strcmp(action, "raw-extract") ||
      (!strcmp(action, "show") && args.n && strstr(args.v[0], ".deb"))) {
    static const struct { const char *a, *o; } map[] = {{"info", "-I"}, {"contents", "-c"}, {"extract", "-x"}, {"vextract", "-X"}, {"control", "-e"},
                                                        {"field", "-f"}, {"fsys-tarfile", "--fsys-tarfile"}, {"raw-extract", "-R"}, {"show", "-W"}, {0, 0}};
    char **nv = xcalloc(args.n + 3, sizeof *nv);
    nv[0] = "dpkg-deb";
    for (int k = 0; map[k].a; k++) if (!strcmp(map[k].a, action)) nv[1] = (char *)map[k].o;
    for (int k = 0; k < args.n; k++) nv[k + 2] = args.v[k];
    return dpkg_deb_main(args.n + 2, nv);
  }
  pkg_config_finish();
  if (!strcmp(action, "print-architecture")) { printf("%s\n", pkg_cfg.arch); return 0; }
  if (!strcmp(action, "print-foreign-architectures")) { for (int k = 0; k < pkg_cfg.foreign.n; k++) printf("%s\n", pkg_cfg.foreign.v[k]); return 0; }
  if (!strcmp(action, "add-architecture") || !strcmp(action, "remove-architecture")) {
    if (args.n != 1) { pkg_error("--%s takes one architecture", action); return 2; }
    const char *ar = args.v[0];
    if (action[0] == 'a') {
      if (!strcmp(ar, "all") || !strcmp(ar, "any")) { pkg_error("architecture '%s' is reserved and cannot be added", ar); return 2; }
      if (strcmp(ar, pkg_cfg.arch) && sl_find(&pkg_cfg.foreign, ar) < 0) sl_add(&pkg_cfg.foreign, ar);
    } else {
      int k = sl_find(&pkg_cfg.foreign, ar);
      if (!strcmp(ar, pkg_cfg.arch)) { pkg_error("cannot remove the native architecture"); return 2; }
      pkgdb db;
      if (db_load(&db, 0) == 0) {
        for (int j = 0; j < db.n; j++) if (pi_present(db.p[j]) && db.p[j]->arch && !strcmp(db.p[j]->arch, ar)) { pkg_error("cannot remove architecture '%s' currently in use by the database", ar); db_free(&db); return 2; }
        db_free(&db);
      }
      if (k >= 0) { free(pkg_cfg.foreign.v[k]); pkg_cfg.foreign.v[k] = pkg_cfg.foreign.v[--pkg_cfg.foreign.n]; }
    }
    return write_arch_file();
  }
  if (!strcmp(action, "clear-avail")) { char *av = path_join(pkg_cfg.admindir, "available"); if (!pkg_cfg.no_act) unlink(av); free(av); return 0; }
  if (!strcmp(action, "merge-avail") || !strcmp(action, "update-avail")) {
    if (args.n != 1) { pkg_error("--%s takes one Packages file", action); return 2; }
    return avail_update(action[0] == 'm', args.v[0]);
  }
  if (!strcmp(action, "print-avail")) return cmd_print_avail(args.v, args.n);

  int writing = !strcmp(action, "install") || !strcmp(action, "unpack") || !strcmp(action, "configure") || !strcmp(action, "remove") ||
                !strcmp(action, "purge") || !strcmp(action, "set-selections") || !strcmp(action, "triggers-only");
  pkgdb db;
  if (db_load(&db, writing) < 0) return 2;
  int rc = 0;
  if (!strcmp(action, "list")) rc = cmd_list(&db, args.v, args.n);
  else if (!strcmp(action, "status")) rc = cmd_status(&db, args.v, args.n);
  else if (!strcmp(action, "listfiles")) rc = cmd_listfiles(&db, args.v, args.n);
  else if (!strcmp(action, "search")) rc = cmd_search(&db, args.v, args.n);
  else if (!strcmp(action, "show")) rc = cmd_show(&db, args.v, args.n, showfmt);
  else if (!strcmp(action, "audit")) rc = cmd_audit(&db);
  else if (!strcmp(action, "verify")) rc = cmd_verify(&db, args.v, args.n);
  else if (!strcmp(action, "get-selections")) rc = cmd_selections(&db, args.v, args.n);
  else if (!strcmp(action, "set-selections")) rc = cmd_set_selections(&db);
  else if (!strcmp(action, "control-path")) rc = args.n ? cmd_control_path(&db, args.v[0], args.n > 1 ? args.v[1] : 0) : 2;
  else if (!strcmp(action, "control-list")) rc = args.n ? cmd_control_path(&db, args.v[0], 0) : 2;
  else if (!strcmp(action, "control-show")) {
    if (args.n < 2) rc = 2;
    else { pkginfo *p = find_arg(&db, args.v[0]); char *f = p ? pi_infoname(p, args.v[1]) : 0, *data; if (f && file_read(f, &data, 0) == 0) { fputs(data, stdout); free(data); } else rc = 1; free(f); }
  } else if (!strcmp(action, "install") || !strcmp(action, "unpack")) {
    strlist files = {0};
    for (int k = 0; k < args.n; k++) { if (recursive && is_dir_arg(args.v[k])) collect_debs(args.v[k], &files); else sl_add(&files, args.v[k]); }
    if (!files.n) { pkg_error("--%s needs at least one package archive file argument", action); rc = 2; }
    for (int k = 0; k < files.n; k++) {
      pkg_msg("%s %s ...\n", "Unpacking", files.v[k]);
      if (dpkg_unpack(&db, files.v[k], action[0] == 'i') != 0) rc = 1;
    }
    if (!strcmp(action, "install")) {
      /* configure everything this run unpacked, as a batch (dependency cycles are allowed inside it) */
      if (dpkg_configure_pending(&db) != 0) rc = 1;
    } else if (dpkg_triggers_run(&db)) rc = 1;
    sl_free(&files);
  } else if (!strcmp(action, "configure")) {
    if (all) rc = dpkg_configure_pending(&db);
    else {
      for (int k = 0; k < args.n; k++) { pkginfo *p = find_arg(&db, args.v[k]); char kk[300]; if (p) sl_add(&dpkg_batch, pi_key(p, kk, sizeof kk)); }
      for (int k = 0; k < args.n; k++) {
        pkginfo *p = find_arg(&db, args.v[k]);
        if (!p) { pkg_error("package %s is not installed", args.v[k]); rc = 1; continue; }
        if (dpkg_configure(&db, p, 0)) rc = 1;
      }
      if (dpkg_triggers_run(&db)) rc = 1;
    }
  } else if (!strcmp(action, "triggers-only")) rc = dpkg_triggers_run(&db);
  else if (!strcmp(action, "remove") || !strcmp(action, "purge")) {
    if (all) { for (int k = 0; k < db.n; k++) if (db.p[k]->want == W_DEINSTALL || db.p[k]->want == W_PURGE) { char *n = xstrdup(db.p[k]->name); sl_add(&args, n); free(n); } }
    for (int k = 0; k < args.n; k++) { pkginfo *p = find_arg(&db, args.v[k]); char kk[300]; if (p) sl_add(&dpkg_batch, pi_key(p, kk, sizeof kk)); }
    for (int k = 0; k < args.n; k++) {
      pkginfo *p = find_arg(&db, args.v[k]);
      if (!p) { pkg_warn("ignoring request to remove %s which isn't installed", args.v[k]); continue; }
      if (dpkg_remove(&db, p, action[0] == 'p')) rc = 1;
    }
    if (dpkg_triggers_run(&db)) rc = 1;
  }
  if (db.dirty) db_save(&db);
  db_free(&db);
  sl_free(&args);
  pkg_unsupported_report();
  return rc;
}
