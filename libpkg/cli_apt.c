// SPDX-License-Identifier: GPL-2.0-or-later
/* apt, apt-get and apt-cache. One program with three personalities; the
 * commands and options follow apt(8), apt-get(8) and apt-cache(8). */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#ifndef __wasm__
#include <sys/resource.h>
#endif

static const char *who = "apt";
static int opt_yes, opt_sim, opt_quiet, opt_fix, opt_dlonly, opt_reinstall, opt_purge, opt_autoremove;
static int opt_installed, opt_upgradable, opt_allver, opt_stats;

/* ---------- helpers ---------- */
static void si_size(uint64_t bytes, char *out, size_t cap) {
  if (bytes < 1000) snprintf(out, cap, "%llu B", (unsigned long long)bytes);
  else if (bytes < 1000000) snprintf(out, cap, "%.1f kB", bytes / 1000.0);
  else if (bytes < 1000000000) snprintf(out, cap, "%.1f MB", bytes / 1e6);
  else snprintf(out, cap, "%.1f GB", bytes / 1e9);
}
static void progress(const char *what) { if (!opt_quiet) printf("%s... Done\n", what); }
static void wrap_list(const char *title, strlist *names) {
  if (!names->n) return;
  printf("%s\n", title);
  sl_sort(names);
  int col = 0;
  for (int i = 0; i < names->n; i++) {
    int l = strlen(names->v[i]);
    if (col && col + l + 1 > 78) { printf("\n"); col = 0; }
    if (!col) { printf("  "); col = 2; }
    else { printf(" "); col++; }
    printf("%s", names->v[i]);
    col += l;
  }
  printf("\n");
}
static int confirm(void) {
  if (opt_yes) return 1;
  printf("Do you want to continue? [Y/n] ");
  fflush(stdout);
  char line[64];
  if (!fgets(line, sizeof line, stdin)) { printf("\nAbort.\n"); return 0; }
  if (line[0] == '\n' || line[0] == 'y' || line[0] == 'Y') return 1;
  printf("Abort.\n");
  return 0;
}
static long peak_kib(void) {
#ifndef __wasm__
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
#else
  return (long)(__builtin_wasm_memory_size(0) * 64);
#endif
}

/* ---------- extended_states: which packages were installed automatically ---------- */
static char *ext_path(void) { char *d = apt_dir("Dir::State", "var/lib/apt"); char *p = path_join(d, "extended_states"); free(d); return p; }
static int ext_one(ctl_para *p, void *arg) {
  strmap *m = arg;
  const char *n = ctl_get(p, "Package"), *a = ctl_get(p, "Auto-Installed");
  if (n && a && !strcmp(a, "1")) *sm_slot(m, n) = (void *)1;
  return 0;
}
int extstates_load(strmap *m) {
  memset(m, 0, sizeof *m);
  char *p = ext_path();
  ctl_parse_file(p, ext_one, m);
  free(p);
  return 0;
}
int extstates_save(const strmap *m) {
  if (pkg_cfg.no_act) return 0;
  strlist names = {0};
  for (uint32_t i = 0; i < m->cap; i++) if (m->k && m->k[i] && m->v[i]) sl_add(&names, m->k[i]);
  sl_sort(&names);
  sbuf b = {0};
  for (int i = 0; i < names.n; i++) sb_printf(&b, "Package: %s\nArchitecture: %s\nAuto-Installed: 1\n\n", names.v[i], pkg_cfg.arch);
  char *p = ext_path(), *d = path_dirname(p);
  mkdir_p(d, 0755);
  int r = file_write_atomic(p, b.s ? b.s : "", b.n, 0644);
  free(p); free(d); sb_free(&b); sl_free(&names);
  return r;
}

/* ---------- autoremove: auto-installed packages nothing manual needs ---------- */
static void mark_reach(pkgindex *ix, pkgver *v, strmap *reached) {
  if (!v) return;
  void **slot = sm_slot(reached, v->pn->name);
  if (*slot) return;
  *slot = (void *)1;
  const char *f[4] = {v->predepends, v->depends, aptconf_bool("APT::AutoRemove::RecommendsImportant", 1) ? v->recommends : 0,
                      aptconf_bool("APT::AutoRemove::SuggestsImportant", 1) ? v->suggests : 0};
  for (int k = 0; k < 4; k++) {
    dep_list l;
    if (!f[k] || dep_parse(f[k], DEP_ALT, &l, 0) < 0) continue;
    for (int g = 0; g < l.n; g++)
      for (int a = 0; a < l.o[g].n; a++) {
        pkgver **c;
        int n = dep_candidates(ix, &l.o[g].a[a], v->arch, &c);
        for (int j = 0; j < n; j++) if (c[j]->installed) mark_reach(ix, c[j], reached);
        free(c);
      }
    dep_free(&l);
  }
}
static void autoremovable(pkgindex *ix, strmap *autom, strlist *out) {
  strmap reached = {0};
  for (uint32_t h = 0; h < ix->tabcap; h++)
    for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) {
      pkgver *v = index_installed(pn);
      if (!v) continue;
      if (!sm_get(autom, pn->name) || v->essential || v->important || v->prio >= 5) mark_reach(ix, v, &reached);
    }
  for (uint32_t h = 0; h < ix->tabcap; h++)
    for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash)
      if (index_installed(pn) && sm_get(autom, pn->name) && !sm_get(&reached, pn->name)) sl_add(out, pn->name);
  sm_free(&reached);
}

/* ---------- show, policy, search ---------- */
static void print_stanza(pkgindex *ix, pkgver *v, int apt_style) {
  ctl_para p = {0};
  if (index_stanza(ix, v, &p) < 0) { printf("Package: %s\nVersion: %s\n\n", v->pn->name, v->ver); return; }
  if (apt_style) {
    /* apt show leaves out the index bookkeeping and adds where it comes from */
    static const char *const hide[] = {"Filename", "MD5sum", "SHA1", "SHA256", "SHA512", "Description-md5", "Status", "Conffiles", 0};
    for (int i = 0; hide[i]; i++) ctl_del(&p, hide[i]);
    char sz[32];
    si_size(v->size, sz, sizeof sz);
    if (v->size) ctl_set(&p, "Download-Size", sz);
    ctl_set(&p, "APT-Sources", ix->file_label.v[v->file_id]);
    if (v->installed) ctl_set(&p, "APT-Manual-Installed", "yes");
  }
  sbuf b = {0};
  ctl_write(&b, &p);
  fputs(b.s ? b.s : "", stdout);
  putchar('\n');
  sb_free(&b);
  ctl_free(&p);
}
static int cmd_show(pkgindex *ix, char **args, int n, int apt_style) {
  int rc = 0;
  for (int i = 0; i < n; i++) {
    pkgname *pn = index_name(ix, args[i], 0);
    if (!pn || (!pn->vers && !pn->nprov)) { pkg_error("no packages found matching %s", args[i]); rc = 100; continue; }
    if (!pn->vers) {
      printf("Package: %s\nState: not a real package (virtual)\nProvided by:", pn->name);
      for (uint32_t k = 0; k < pn->nprov; k++) printf(" %s", pn->prov[k]->pn->name);
      printf("\n\n");
      continue;
    }
    if (apt_style && !opt_allver) { pkgver *c = index_candidate(ix, pn); print_stanza(ix, c ? c : pn->vers, 1); continue; }
    for (pkgver *v = pn->vers; v; v = v->next) print_stanza(ix, v, apt_style);
  }
  return rc;
}
static int cmd_policy(pkgindex *ix, char **args, int n) {
  if (!n) {
    printf("Package files:\n 100 %s/status\n     release a=now\n", pkg_cfg.admindir);
    for (int f = 1; f < ix->files.n; f++) printf(" 500 %s\n", ix->file_label.v[f]);
    printf("Pinned packages:\n");
    return 0;
  }
  for (int i = 0; i < n; i++) {
    pkgname *pn = index_name(ix, args[i], 0);
    if (!pn || !pn->vers) { pkg_warn("unable to locate package %s", args[i]); continue; }
    pkgver *inst = index_installed(pn), *cand = index_candidate(ix, pn);
    printf("%s:\n  Installed: %s\n  Candidate: %s\n  Version table:\n", pn->name, inst ? inst->ver : "(none)", cand ? cand->ver : "(none)");
    for (pkgver *v = pn->vers; v; v = v->next) {
      printf(" %s %s %d\n", v == inst ? "***" : "   ", v->ver, v->pin);
      if (v->file_id) printf("        %d %s\n", v->pin, ix->file_label.v[v->file_id]);
      if (v->installed) printf("        100 %s/status\n", pkg_cfg.admindir);
    }
  }
  return 0;
}
static int ci_contains(const char *hay, const char *needle) { return strcasestr(hay, needle) != 0; }
static int cmd_search(pkgindex *ix, char **pats, int n, int names_only) {
  /* reads the index files again, one stanza at a time, so descriptions need not stay in memory */
  strlist hits = {0};
  ctl_para p = {0};
  for (int f = 1; f < ix->files.n; f++) {
    fd_reader fr = {0};
    fr.fd = open(ix->files.v[f], O_RDONLY);
    if (fr.fd < 0) continue;
    arc_stream *s = arc_open(ARC_AUTO, fd_reader_src, &fr);
    ctl_reader *r = ctl_open(arc_stream_src, s);
    while (ctl_next(r, &p) == 1) {
      const char *name = ctl_get(&p, "Package"), *d = ctl_get(&p, "Description"), *ver = ctl_get(&p, "Version");
      if (!name) continue;
      int all = 1;
      for (int i = 0; i < n && all; i++) {
        int glob = strpbrk(pats[i], "*?[") != 0;
        int m = glob ? pkg_glob(pats[i], name) : ci_contains(name, pats[i]);
        if (!m && !names_only && d) m = ci_contains(d, pats[i]);
        all = m;
      }
      if (!all) continue;
      char line[512];
      size_t dl = d ? strcspn(d, "\n") : 0;
      if (!strcmp(who, "apt-cache")) snprintf(line, sizeof line, "%s - %.*s", name, (int)dl, d ? d : "");
      else {
        pkgname *pn = index_name(ix, name, 0);
        pkgver *inst = pn ? index_installed(pn) : 0;
        snprintf(line, sizeof line, "%s/%s %s %s%s\n  %.*s\n", name, ix->file_label.v[f], ver ? ver : "", ctl_get(&p, "Architecture") ? ctl_get(&p, "Architecture") : "",
                 inst && ver && !strcmp(inst->ver, ver) ? " [installed]" : "", (int)dl, d ? d : "");
      }
      if (sl_find(&hits, line) < 0) sl_add(&hits, line);
    }
    ctl_close(r); arc_close(s); close(fr.fd);
  }
  ctl_free(&p);
  sl_sort(&hits);
  if (strcmp(who, "apt-cache")) printf("Sorting... Done\nFull Text Search... Done\n");
  for (int i = 0; i < hits.n; i++) printf("%s\n", hits.v[i]);
  sl_free(&hits);
  return 0;
}
static int name_cmp(const void *a, const void *b) { return strcmp((*(pkgname *const *)a)->name, (*(pkgname *const *)b)->name); }
static pkgname **sorted_names(pkgindex *ix, int *n) {
  pkgname **v = xmalloc((ix->nnames + 1) * sizeof *v);
  int k = 0;
  for (uint32_t h = 0; h < ix->tabcap; h++) for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) if (pn->vers) v[k++] = pn;
  xqsort(v, k, sizeof *v, name_cmp);
  *n = k;
  return v;
}
static int cmd_list(pkgindex *ix, char **pats, int np) {
  if (!opt_quiet) printf("Listing... Done\n");
  int n;
  pkgname **v = sorted_names(ix, &n);
  for (int i = 0; i < n; i++) {
    pkgname *pn = v[i];
    if (np) { int m = 0; for (int k = 0; k < np; k++) if (pkg_glob(pats[k], pn->name)) m = 1; if (!m) continue; }
    pkgver *inst = index_installed(pn), *cand = index_candidate(ix, pn);
    if (opt_installed && !inst) continue;
    if (opt_upgradable && !(inst && cand && ver_cmp(cand->ver, inst->ver) > 0)) continue;
    for (pkgver *w = pn->vers; w; w = w->next) {
      if (!opt_allver && w != cand && w != inst) continue;
      const char *lab = ix->file_label.v[w->file_id];
      const char *sp = strchr(lab, ' ');
      char state[96] = "";
      if (w->installed) {
        strmap *dummy = 0; (void)dummy;
        snprintf(state, sizeof state, " [installed%s]", cand && cand != w && ver_cmp(cand->ver, w->ver) > 0 ? ",upgradable to: " : "");
        if (cand && cand != w && ver_cmp(cand->ver, w->ver) > 0) { size_t l = strlen(state); snprintf(state + l - 1, sizeof state - l + 1, "%s]", cand->ver); }
      } else if (inst && w == cand && ver_cmp(w->ver, inst->ver) > 0) continue;  /* shown on the installed line */
      printf("%s/%s %s %s%s\n", pn->name, w->file_id ? (sp ? sp + 1 : lab) : "now", w->ver, w->arch, state);
      if (!opt_allver) break;
    }
  }
  free(v);
  return 0;
}
static void print_deps_of(pkgindex *ix, pkgver *v) {
  static const char *const names[] = {"PreDepends", "Depends", "Recommends", "Suggests", "Conflicts", "Breaks", "Replaces", "Enhances"};
  const char *f[] = {v->predepends, v->depends, v->recommends, v->suggests, v->conflicts, v->breaks, v->replaces, v->enhances};
  printf("%s\n", v->pn->name);
  for (int k = 0; k < 8; k++) {
    dep_list l;
    if (!f[k] || dep_parse(f[k], k < 4 ? DEP_ALT : 0, &l, 0) < 0) continue;
    for (int g = 0; g < l.n; g++)
      for (int a = 0; a < l.o[g].n; a++) {
        sbuf b = {0};
        dep_atom_format(&b, &l.o[g].a[a]);
        printf(" %s%s: %s\n", a + 1 < l.o[g].n ? "|" : "", names[k], b.s);
        sb_free(&b);
        pkgver **c;
        int n = dep_candidates(ix, &l.o[g].a[a], v->arch, &c);
        for (int j = 0; j < n; j++) if (c[j]->pn != index_name(ix, l.o[g].a[a].name, 0)) printf("    %s\n", c[j]->pn->name);
        free(c);
      }
    dep_free(&l);
  }
}
static int cmd_depends(pkgindex *ix, char **args, int n) {
  for (int i = 0; i < n; i++) {
    pkgname *pn = index_name(ix, args[i], 0);
    if (!pn || !pn->vers) { pkg_error("no packages found matching %s", args[i]); return 100; }
    pkgver *c = index_candidate(ix, pn);
    print_deps_of(ix, c ? c : pn->vers);
  }
  return 0;
}
static int cmd_rdepends(pkgindex *ix, char **args, int n) {
  int count;
  pkgname **all = sorted_names(ix, &count);
  for (int i = 0; i < n; i++) {
    pkgname *target = index_name(ix, args[i], 0);
    if (!target) { pkg_error("no packages found matching %s", args[i]); free(all); return 100; }
    printf("%s\nReverse Depends:\n", target->name);
    for (int k = 0; k < count; k++) {
      pkgver *v = index_candidate(ix, all[k]);
      if (!v) continue;
      const char *f[3] = {v->predepends, v->depends, v->recommends};
      int found = 0;
      for (int x = 0; x < 3 && !found; x++) {
        dep_list l;
        if (!f[x] || dep_parse(f[x], DEP_ALT, &l, 0) < 0) continue;
        for (int g = 0; g < l.n && !found; g++)
          for (int a = 0; a < l.o[g].n && !found; a++) {
            if (!strcmp(l.o[g].a[a].name, target->name)) found = 1;
            for (uint32_t pv = 0; pv < target->nprov && !found; pv++) if (!strcmp(l.o[g].a[a].name, target->name)) found = 1;
          }
        dep_free(&l);
      }
      if (found) printf("  %s\n", v->pn->name);
    }
  }
  free(all);
  return 0;
}

/* ---------- downloads ---------- */
static char *archive_path(const pkgver *v) {
  char *dir = apt_dir("Dir::Cache::archives", "archives/");
  sbuf b = {0};
  for (const char *s = v->ver; *s; s++) { if (*s == ':') sb_puts(&b, "%3a"); else sb_putc(&b, *s); }
  char *name = xasprintf("%s_%s_%s.deb", v->pn->name, b.s, v->arch);
  char *p = path_join(dir, name);
  free(dir); free(name); sb_free(&b);
  return p;
}
static int download(pkgindex *ix, pkgver *v, const char *dest_override, int num) {
  if (!v->file_id) { pkg_error("%s %s is not available from any source; it cannot be downloaded", v->pn->name, v->ver); return -1; }
  ctl_para p = {0};
  if (index_stanza(ix, v, &p) < 0) { pkg_error("cannot read the index entry of %s", v->pn->name); return -1; }
  const char *fn = ctl_get(&p, "Filename"), *sha = ctl_get(&p, "SHA256"), *sz = ctl_get(&p, "Size");
  if (!fn) { pkg_error("%s %s has no Filename in its index", v->pn->name, v->ver); ctl_free(&p); return -1; }
  char *dest = dest_override ? xstrdup(dest_override) : archive_path(v);
  char *d = path_dirname(dest);
  mkdir_p(d, 0755);
  free(d);
  char h[65] = "";
  int have = sha && hex_digest_file(dest, 0, h) == 0 && !strcmp(h, sha);
  char *url = xasprintf("%s%s", ix->file_base.v[v->file_id], fn);
  int rc = 0;
  if (!have) {
    char szs[32];
    si_size(v->size, szs, sizeof szs);
    if (!opt_quiet) printf("Get:%d %s %s %s %s [%s]\n", num, ix->file_label.v[v->file_id], v->pn->name, v->arch, v->ver, szs);
    if (pkg_fetch(url, dest, 0) != 0) { pkg_error("failed to fetch %s", url); rc = -1; }
    else if (!sha || hex_digest_file(dest, 0, h) < 0 || strcmp(h, sha) || (sz && (uint64_t)strtoull(sz, 0, 10) != v->size)) {
      pkg_error("hash sum mismatch for %s: the file is not the one the index describes; it was deleted", url);
      unlink(dest);
      rc = -1;
    }
  }
  free(url); free(dest);
  ctl_free(&p);
  return rc;
}

/* ---------- install / remove / upgrade ---------- */
static int parse_job(pkgindex *ix, const char *arg, int kind, solve_job *j, char **err) {
  memset(j, 0, sizeof *j);
  j->kind = kind;
  char name[256];
  snprintf(name, sizeof name, "%s", arg);
  int len = strlen(name);
  /* apt's suffixes: pkg- removes and pkg+ installs inside the same command */
  if (len > 1 && kind == JOB_INSTALL && name[len - 1] == '-' && !index_name(ix, name, 0)) { name[--len] = 0; j->kind = JOB_REMOVE; }
  else if (len > 1 && name[len - 1] == '+' && !index_name(ix, name, 0)) { name[--len] = 0; j->kind = JOB_INSTALL; }
  char *eq = strchr(name, '=');
  if (eq) { *eq = 0; j->verstr = xstrdup(eq + 1); j->op = REL_EQ; }
  char *colon = strchr(name, ':');
  if (colon) *colon = 0;   /* architecture qualifier: only native and all are resolved here */
  pkgname *pn = index_name(ix, name, 0);
  if (!pn || (!pn->vers && !pn->nprov)) { *err = xasprintf("unable to locate package %s", name); return -1; }
  if (!pn->vers && j->kind == JOB_INSTALL) {
    if (pn->nprov == 1) { pkg_msg("Note, selecting '%s' instead of '%s'\n", pn->prov[0]->pn->name, name); pn = pn->prov[0]->pn; }
    else {
      sbuf b = {0};
      for (uint32_t k = 0; k < pn->nprov; k++) sb_printf(&b, "  %s %s\n", pn->prov[k]->pn->name, pn->prov[k]->ver);
      *err = xasprintf("package %s is a virtual package provided by:\n%sYou should explicitly select one to install.", name, b.s);
      sb_free(&b);
      return -1;
    }
  }
  j->pn = pn;
  return 0;
}

static int run_plan(pkgdb *db, pkgindex *ix, solve_result *r, strmap *autom, int reinst_n, pkgver **reinst) {
  order_step *ord;
  int nord;
  sbuf notes = {0};
  solve_order(ix, r, &ord, &nord, &notes);
  if (notes.n) fputs(notes.s, stderr);
  sb_free(&notes);
  /* add reinstalls: unpack + configure at the end */
  ord = xrealloc(ord, (nord + 2 * reinst_n + 1) * sizeof *ord);
  for (int i = 0; i < reinst_n; i++) { ord[nord].v = reinst[i]; ord[nord++].op = 'u'; ord[nord].v = reinst[i]; ord[nord++].op = 'c'; }
  if (opt_sim) {
    for (int i = 0; i < nord; i++) {
      pkgver *v = ord[i].v;
      pkgver *old = index_installed(v->pn);
      const char *lab = ix->file_label.v[v->file_id];
      if (ord[i].op == 'u') printf("Inst %s%s%s%s (%s %s [%s])\n", v->pn->name, old && old != v ? " [" : "", old && old != v ? old->ver : "", old && old != v ? "]" : "", v->ver, lab, v->arch);
      else if (ord[i].op == 'c') printf("Conf %s (%s %s [%s])\n", v->pn->name, v->ver, lab, v->arch);
      else printf("%s %s [%s]\n", ord[i].op == 'p' ? "Purg" : "Remv", v->pn->name, v->ver);
    }
    free(ord);
    return 0;
  }
  /* downloads first: nothing changes on disk if one fails */
  int num = 0, failed = 0;
  for (int i = 0; i < nord; i++) if (ord[i].op == 'u') if (download(ix, ord[i].v, 0, ++num) < 0) failed = 1;
  if (failed) { pkg_error("some archives could not be downloaded; nothing was changed"); free(ord); return 100; }
  if (opt_dlonly) { printf("Download complete and in download only mode\n"); free(ord); return 0; }
  /* everything this run unpacks may satisfy dependencies of the others while they are
   * configured together (dpkg's batch); Multi-Arch: same packages are keyed name:arch */
  for (int i = 0; i < nord; i++) if (ord[i].op == 'u') {
    char k[300];
    snprintf(k, sizeof k, "%s", ord[i].v->pn->name);
    if (sl_find(&dpkg_batch, k) < 0) sl_add(&dpkg_batch, k);
    snprintf(k, sizeof k, "%s:%s", ord[i].v->pn->name, strcmp(ord[i].v->arch, "all") ? ord[i].v->arch : pkg_cfg.arch);
    if (sl_find(&dpkg_batch, k) < 0) sl_add(&dpkg_batch, k);
  }
  int rc = 0;
  /* DPkg::Options from apt.conf go to dpkg, e.g. "--force-confnew" */
  strlist dopts = {0};
  aptconf_list("DPkg::Options", &dopts);
  for (int i = 0; i < dopts.n; i++) { char *v = strchr(dopts.v[i], '='); if (v) *v++ = 0; pkg_config_option(dopts.v[i], v); }
  sl_free(&dopts);
  for (int i = 0; i < nord && !rc; i++) {
    pkgver *v = ord[i].v;
    const char *arch = strcmp(v->arch, "all") ? v->arch : pkg_cfg.arch;
    if (ord[i].op == 'r' || ord[i].op == 'p') {
      pkginfo *p = db_find(db, v->pn->name, arch);
      if (p && dpkg_remove(db, p, ord[i].op == 'p')) rc = 100;
      *sm_slot(autom, v->pn->name) = 0;
    } else if (ord[i].op == 'u') {
      char *deb = archive_path(v);
      if (dpkg_unpack(db, deb, 0)) rc = 100;
      free(deb);
    } else if (ord[i].op == 'c') {
      pkginfo *p = db_find(db, v->pn->name, arch);
      if (p && (p->status == S_UNPACKED || p->status == S_HALF_CONFIGURED) && dpkg_configure(db, p, 1)) rc = 100;
    }
  }
  if (dpkg_triggers_run(db)) rc = 100;
  for (int i = 0; i < r->n; i++) if (r->steps[i].action == 1) {
    if (r->steps[i].auto_inst && !r->steps[i].old) *sm_slot(autom, r->steps[i].v->pn->name) = (void *)1;
    else if (!r->steps[i].auto_inst) *sm_slot(autom, r->steps[i].v->pn->name) = 0;
  }
  extstates_save(autom);
  if (rc) pkg_error("sub-process dpkg returned an error code; 'apt -f install' or 'dpkg --configure -a' may fix it");
  free(ord);
  return rc;
}

static int cmd_change(pkgdb *db, pkgindex *ix, const char *cmd, char **args, int n, solve_opts *o) {
  int kind = !strcmp(cmd, "remove") ? JOB_REMOVE : !strcmp(cmd, "purge") ? JOB_PURGE : JOB_INSTALL;
  if (opt_purge && kind == JOB_REMOVE) kind = JOB_PURGE;
  solve_job *jobs = xcalloc(n + 1, sizeof *jobs);
  int nj = 0, rc = 0;
  pkgver **reinst = xcalloc(n + db->n + 1, sizeof *reinst);
  int nre = 0;
  strmap autom;
  extstates_load(&autom);
  if (!strcmp(cmd, "upgrade")) o->upgrade_all = 1;
  if (!strcmp(cmd, "full-upgrade") || !strcmp(cmd, "dist-upgrade")) { o->upgrade_all = o->dist_upgrade = 1; o->allow_remove = 1; }
  if (!strcmp(cmd, "autoremove") || !strcmp(cmd, "autopurge")) {
    strlist rm = {0};
    autoremovable(ix, &autom, &rm);
    for (int i = 0; i < rm.n; i++) { jobs = xrealloc(jobs, (nj + 1) * sizeof *jobs); memset(&jobs[nj], 0, sizeof *jobs); jobs[nj].kind = !strcmp(cmd, "autopurge") || opt_purge ? JOB_PURGE : JOB_REMOVE; jobs[nj].pn = index_name(ix, rm.v[i], 0); nj++; }
    sl_free(&rm);
  }
  for (int i = 0; i < n; i++) {
    char *err;
    if (parse_job(ix, args[i], kind, &jobs[nj], &err) < 0) { pkg_error("%s", err); free(err); rc = 100; continue; }
    pkgver *inst = index_installed(jobs[nj].pn);
    if (jobs[nj].kind == JOB_INSTALL && inst && !jobs[nj].verstr && (opt_reinstall || !strcmp(cmd, "reinstall"))) {
      pkgver *c = index_candidate(ix, jobs[nj].pn);
      if (c && c->installed) reinst[nre++] = c;
    }
    if (jobs[nj].kind == JOB_INSTALL && inst && !jobs[nj].verstr && !opt_reinstall && strcmp(cmd, "reinstall")) {
      pkgver *c = index_candidate(ix, jobs[nj].pn);
      if (c == inst) { printf("%s is already the newest version (%s).\n", inst->pn->name, inst->ver); *sm_slot(&autom, inst->pn->name) = 0; }
    }
    if ((jobs[nj].kind == JOB_REMOVE || jobs[nj].kind == JOB_PURGE) && !inst) {
      if (jobs[nj].pn->inst && jobs[nj].pn->inst->status == S_CONFIG_FILES && jobs[nj].kind == JOB_PURGE) {
        /* only configuration files are left: purge them directly */
        if (!opt_sim) dpkg_remove(db, jobs[nj].pn->inst, 1);
      } else printf("Package '%s' is not installed, so not removed\n", jobs[nj].pn->name);
      continue;
    }
    nj++;
  }
  if (rc) { free(jobs); free(reinst); sm_free(&autom); return rc; }
  if (opt_fix) {
    /* packages dpkg left half-installed must be unpacked again */
    for (int i = 0; i < db->n; i++)
      if (db->p[i]->eflag == E_REINSTREQ || db->p[i]->status == S_HALF_INSTALLED) {
        pkgname *pn = index_name(ix, db->p[i]->name, 0);
        pkgver *v = pn ? index_installed(pn) : 0;
        if (v && v->file_id) reinst[nre++] = v;
      }
  }
  if (!opt_quiet) progress("Building dependency tree");
  solve_result r;
  int sr = solve(ix, db, jobs, nj, o, &r);
  if (opt_stats) fprintf(stderr, "solver: %d variables, %d clauses, %d decisions, %d conflicts, %.1f ms, peak memory %ld KiB\n", r.vars, r.clauses, r.decisions, r.conflicts, r.ms, peak_kib());
  if (sr < 0) {
    printf("Some packages could not be installed. The request cannot be satisfied because:\n%s", r.why.s ? r.why.s : "  (no reason recorded)\n");
    pkg_error("unable to correct problems, you have held broken packages.");
    solve_result_free(&r); free(jobs); free(reinst); sm_free(&autom);
    return 100;
  }
  /* what the user sees */
  strlist extra = {0}, newp = {0}, upg = {0}, down = {0}, rem = {0}, rec_missing = {0}, sugg = {0}, kept = {0}, reins = {0};
  for (int i = 0; i < r.n; i++) {
    solve_step *s = &r.steps[i];
    if (s->action == 1) {
      if (!s->old) sl_add(&newp, s->v->pn->name);
      else if (ver_cmp(s->v->ver, s->old->ver) < 0) sl_add(&down, s->v->pn->name);
      else sl_add(&upg, s->v->pn->name);
      if (s->auto_inst && !s->old) sl_add(&extra, s->v->pn->name);
      dep_list l;
      if (s->v->suggests && dep_parse(s->v->suggests, DEP_ALT, &l, 0) == 0) { for (int g = 0; g < l.n; g++) if (sl_find(&sugg, l.o[g].a[0].name) < 0) sl_add(&sugg, l.o[g].a[0].name); dep_free(&l); }
      if (!o->recommends && s->v->recommends && dep_parse(s->v->recommends, DEP_ALT, &l, 0) == 0) { for (int g = 0; g < l.n; g++) if (sl_find(&rec_missing, l.o[g].a[0].name) < 0) sl_add(&rec_missing, l.o[g].a[0].name); dep_free(&l); }
    } else sl_add(&rem, s->v->pn->name);
  }
  for (int i = 0; i < nre; i++) sl_add(&reins, reinst[i]->pn->name);
  /* upgradable packages this run leaves alone */
  for (uint32_t h = 0; h < ix->tabcap; h++) for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) {
    pkgver *inst = index_installed(pn), *cand = index_candidate(ix, pn);
    if (inst && cand && ver_cmp(cand->ver, inst->ver) > 0 && sl_find(&upg, pn->name) < 0 && sl_find(&rem, pn->name) < 0) sl_add(&kept, pn->name);
  }
  for (int i = 0; i < sugg.n;) { pkgname *pn = index_name(ix, sugg.v[i], 0); if (pn && (index_installed(pn) || sl_find(&newp, pn->name) >= 0)) { free(sugg.v[i]); sugg.v[i] = sugg.v[--sugg.n]; } else i++; }
  for (int i = 0; i < rec_missing.n;) { pkgname *pn = index_name(ix, rec_missing.v[i], 0); if (pn && (index_installed(pn) || sl_find(&newp, pn->name) >= 0)) { free(rec_missing.v[i]); rec_missing.v[i] = rec_missing.v[--rec_missing.n]; } else i++; }
  if (strcmp(cmd, "autoremove") && strcmp(cmd, "autopurge")) {
    strlist ar = {0};
    autoremovable(ix, &autom, &ar);
    for (int i = 0; i < ar.n;) { if (sl_find(&rem, ar.v[i]) >= 0) { free(ar.v[i]); ar.v[i] = ar.v[--ar.n]; } else i++; }
    if (ar.n) { wrap_list("The following packages were automatically installed and are no longer required:", &ar); printf("Use '%s autoremove' to remove them.\n", who); }
    sl_free(&ar);
  }
  if (newp.n > 0 && extra.n) wrap_list("The following additional packages will be installed:", &extra);
  if (sugg.n) wrap_list("Suggested packages:", &sugg);
  if (rec_missing.n) wrap_list("Recommended packages:", &rec_missing);
  wrap_list("The following packages will be REMOVED:", &rem);
  wrap_list("The following NEW packages will be installed:", &newp);
  wrap_list("The following packages will be upgraded:", &upg);
  wrap_list("The following packages will be DOWNGRADED:", &down);
  if (o->upgrade_all) wrap_list("The following packages have been kept back:", &kept);
  printf("%d upgraded, %d newly installed, %d to remove and %d not upgraded.\n", upg.n, newp.n, rem.n, kept.n);
  if (reins.n) printf("%d reinstalled.\n", reins.n);
  int changes = r.n + nre;
  if (changes && !opt_sim) {
    char a[32], b[32];
    uint64_t dl = r.dl_bytes;
    for (int i = 0; i < nre; i++) dl += reinst[i]->size;
    si_size(dl, a, sizeof a);
    printf("Need to get %s of archives.\n", a);
    long long delta = ((long long)r.inst_kib - (long long)r.freed_kib) * 1024;
    si_size(delta < 0 ? -delta : delta, b, sizeof b);
    printf("After this operation, %s %s.\n", b, delta >= 0 ? "of additional disk space will be used" : "disk space will be freed");
  } else if (changes && opt_stats) {
    char a[32], b[32];
    si_size(r.dl_bytes, a, sizeof a);
    si_size((uint64_t)r.inst_kib * 1024, b, sizeof b);
    fprintf(stderr, "plan: %d packages, download %s (%llu bytes), installed size %s (%llu KiB)\n", r.n, a, (unsigned long long)r.dl_bytes, b, (unsigned long long)r.inst_kib);
  }
  /* apt-secure(8): packages from a repository whose Release file was not authenticated */
  strlist unauth = {0};
  for (int i = 0; i < r.n; i++) if (r.steps[i].action == 1 && index_untrusted(ix, r.steps[i].v)) sl_add(&unauth, r.steps[i].v->pn->name);
  for (int i = 0; i < nre; i++) if (index_untrusted(ix, reinst[i])) sl_add(&unauth, reinst[i]->pn->name);
  int unauth_ok = 1;
  if (unauth.n) {
    wrap_list("WARNING: The following packages cannot be authenticated!", &unauth);
    if (!aptconf_bool("APT::Get::AllowUnauthenticated", 0) && !opt_sim) {
      if (opt_yes) { fflush(stdout); fprintf(stderr, "E: There were unauthenticated packages and -y was used without --allow-unauthenticated\n"); unauth_ok = 0; }
      else {
        printf("Install these packages without verification? [y/N] ");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof line, stdin) || (line[0] != 'y' && line[0] != 'Y')) { fflush(stdout); fprintf(stderr, "E: Some packages could not be authenticated\n"); unauth_ok = 0; }
      }
    }
  }
  sl_free(&unauth);
  if (!unauth_ok) {
    sl_free(&extra); sl_free(&newp); sl_free(&upg); sl_free(&down); sl_free(&rem); sl_free(&rec_missing); sl_free(&sugg); sl_free(&kept); sl_free(&reins);
    solve_result_free(&r); free(jobs); free(reinst); sm_free(&autom);
    return 100;
  }
  /* like apt-get: no question when the only change is exactly what was asked for */
  int only_asked = !rem.n && !down.n && !upg.n && !extra.n && !nre;
  int go = changes && (opt_sim || only_asked || confirm());
  if (changes && go) rc = run_plan(db, ix, &r, &autom, nre, reinst);
  else if (changes && !go) rc = 1;
  if (opt_fix && !opt_sim && rc == 0) { if (dpkg_configure_pending(db)) rc = 100; }
  sl_free(&extra); sl_free(&newp); sl_free(&upg); sl_free(&down); sl_free(&rem); sl_free(&rec_missing); sl_free(&sugg); sl_free(&kept); sl_free(&reins);
  solve_result_free(&r);
  free(jobs); free(reinst);
  sm_free(&autom);
  return rc;
}

static void usage(void) {
  printf("Usage: %s [options] command\n\n", who);
  if (!strcmp(who, "apt-cache"))
    printf("Commands: show search policy depends rdepends pkgnames stats showpkg madison\n");
  else
    printf("Commands:\n"
           "  update                 fetch the package indexes\n"
           "  install PKG...         install (PKG=VERSION for a version; PKG- removes)\n"
           "  reinstall PKG...       install the same version again\n"
           "  remove PKG...          remove, keep configuration files\n"
           "  purge PKG...           remove everything\n"
           "  autoremove             remove packages nothing needs any more\n"
           "  upgrade                upgrade without removing anything\n"
           "  full-upgrade           upgrade, removing packages if needed (dist-upgrade)\n"
           "  search WORD...  show PKG...  list [PATTERN]  policy [PKG]\n"
           "  depends PKG  rdepends PKG  download PKG...  check  clean\n");
  printf("\nOptions: -y (assume yes)  -s (simulate)  -f (fix broken)  -d (download only)\n"
         "  -q  --no-install-recommends  --install-recommends  --reinstall  --purge\n"
         "  --allow-insecure-repositories  --allow-unauthenticated (see apt-secure(8))\n"
         "  -o Key=Value (apt.conf, e.g. -o Dir=/scratch/root -o APT::Architecture=arm64)  -c FILE\n");
}

int apt_main(int argc, char **argv, const char *personality) {
  who = personality;
  pkg_progname = personality;
  pkg_config_init();
  /* configuration: APT_CONFIG, then Dir (from -o, else the dpkg root), then apt.conf and apt.conf.d */
  strlist cmdline_o = {0}, args = {0};
  const char *cfgfile = getenv("APT_CONFIG");
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if ((!strcmp(a, "-o") || !strcmp(a, "--option")) && i + 1 < argc) { sl_add(&cmdline_o, argv[++i]); continue; }
    if (!strncmp(a, "-o", 2) && a[2]) { sl_add(&cmdline_o, a + 2); continue; }
    if (!strncmp(a, "--option=", 9)) { sl_add(&cmdline_o, a + 9); continue; }
    if ((!strcmp(a, "-c") || !strcmp(a, "--config-file")) && i + 1 < argc) { cfgfile = argv[++i]; continue; }
    if (!strcmp(a, "-y") || !strcmp(a, "--yes") || !strcmp(a, "--assume-yes")) { opt_yes = 1; continue; }
    if (!strcmp(a, "-s") || !strcmp(a, "--simulate") || !strcmp(a, "--just-print") || !strcmp(a, "--dry-run") || !strcmp(a, "--recon") || !strcmp(a, "--no-act")) { opt_sim = 1; continue; }
    if (!strcmp(a, "--no-install-recommends")) { sl_add(&cmdline_o, "APT::Install-Recommends=false"); continue; }
    if (!strcmp(a, "--install-recommends")) { sl_add(&cmdline_o, "APT::Install-Recommends=true"); continue; }
    if (!strcmp(a, "-f") || !strcmp(a, "--fix-broken")) { opt_fix = 1; continue; }
    if (!strcmp(a, "-d") || !strcmp(a, "--download-only")) { opt_dlonly = 1; continue; }
    if (!strcmp(a, "--reinstall")) { opt_reinstall = 1; continue; }
    if (!strcmp(a, "--purge")) { opt_purge = 1; continue; }
    if (!strcmp(a, "--auto-remove") || !strcmp(a, "--autoremove")) { opt_autoremove = 1; continue; }
    if (!strcmp(a, "--installed")) { opt_installed = 1; continue; }
    if (!strcmp(a, "--upgradable")) { opt_upgradable = 1; continue; }
    if (!strcmp(a, "-a") || !strcmp(a, "--all-versions")) { opt_allver = 1; continue; }
    if (!strcmp(a, "--allow-insecure-repositories")) { sl_add(&cmdline_o, "Acquire::AllowInsecureRepositories=true"); continue; }
    if (!strcmp(a, "--allow-downgrade-to-insecure-repositories")) { sl_add(&cmdline_o, "Acquire::AllowDowngradeToInsecureRepositories=true"); continue; }
    if (!strcmp(a, "--allow-unauthenticated")) { sl_add(&cmdline_o, "APT::Get::AllowUnauthenticated=true"); continue; }
    if (!strcmp(a, "--names-only")) { sl_add(&cmdline_o, "APT::Cache::NamesOnly=true"); continue; }
    if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) { opt_quiet++; continue; }
    if (!strcmp(a, "-qq")) { opt_quiet += 2; continue; }
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
    if (!strcmp(a, "-v") || !strcmp(a, "--version")) { printf("%s 2.9.0-ucos (libpkg 0.1)\n", who); return 0; }
    if (a[0] == '-' && a[1]) { pkg_error("command line option '%s' is not understood", a); return 100; }
    sl_add(&args, a);
  }
  if (cfgfile && aptconf_parse_file(cfgfile) == -2) { pkg_error("cannot read %s", cfgfile); return 100; }
  for (int i = 0; i < cmdline_o.n; i++) { char *eq = strchr(cmdline_o.v[i], '='); if (eq) { *eq = 0; aptconf_set(cmdline_o.v[i], eq + 1); *eq = '='; } }
  const char *dir = aptconf_get("Dir", 0);
  if (dir) { pkg_config_option("root", dir); }
  else if (pkg_cfg.root) aptconf_set("Dir", pkg_cfg.root);
  pkg_config_finish();
  if (!aptconf_get("Dir", 0)) aptconf_set("Dir", pkg_cfg.root);
  char *mainf = apt_dir("Dir::Etc::main", "apt.conf"), *parts = apt_dir("Dir::Etc::parts", "apt.conf.d");
  DIR *d = opendir(parts);
  if (d) {
    strlist names = {0};
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && !strchr(e->d_name, '~')) sl_add(&names, e->d_name);
    closedir(d);
    sl_sort(&names);
    for (int i = 0; i < names.n; i++) { char *p = path_join(parts, names.v[i]); aptconf_parse_file(p); free(p); }
    sl_free(&names);
  }
  aptconf_parse_file(mainf);
  free(mainf); free(parts);
  for (int i = 0; i < cmdline_o.n; i++) { char *eq = strchr(cmdline_o.v[i], '='); if (eq) { *eq = 0; aptconf_set(cmdline_o.v[i], eq + 1); } }
  sl_free(&cmdline_o);
  const char *aa = aptconf_get("APT::Architecture", 0);
  if (aa) { free(pkg_cfg.arch); pkg_cfg.arch = xstrdup(aa); }
  opt_stats = aptconf_bool("APT::Solver::Stats", 0);
  if (opt_sim) pkg_cfg.no_act = 1;
  if (!args.n) { usage(); return 100; }
  const char *cmd = args.v[0];
  char **rest = args.v + 1;
  int nrest = args.n - 1;
  if (!strcmp(cmd, "dist-upgrade") && !strcmp(who, "apt")) cmd = "full-upgrade";

  if (!strcmp(cmd, "update")) {
    int rc = apt_update();
    if (!strcmp(who, "apt") && rc == 0) {
      pkgdb db; pkgindex ix;
      if (db_load(&db, 0) == 0) {
        index_load(&ix, &db);
        int up = 0;
        for (uint32_t h = 0; h < ix.tabcap; h++) for (pkgname *pn = ix.tab[h]; pn; pn = pn->next_hash) {
          pkgver *i = index_installed(pn), *c = index_candidate(&ix, pn);
          if (i && c && ver_cmp(c->ver, i->ver) > 0) up++;
        }
        if (up) printf("%d package%s can be upgraded. Run 'apt list --upgradable' to see %s.\n", up, up > 1 ? "s" : "", up > 1 ? "them" : "it");
        else printf("All packages are up to date.\n");
        index_free(&ix); db_free(&db);
      }
    }
    return rc;
  }
  if (!strcmp(cmd, "clean") || !strcmp(cmd, "autoclean")) {
    char *dir2 = apt_dir("Dir::Cache::archives", "archives/");
    DIR *dd = opendir(dir2);
    if (dd) { struct dirent *e; while ((e = readdir(dd))) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".deb")) { char *p = path_join(dir2, e->d_name); if (!opt_sim) unlink(p); free(p); } } closedir(dd); }
    free(dir2);
    return 0;
  }
  int writing = !strcmp(cmd, "install") || !strcmp(cmd, "remove") || !strcmp(cmd, "purge") || !strcmp(cmd, "upgrade") || !strcmp(cmd, "full-upgrade") ||
                !strcmp(cmd, "dist-upgrade") || !strcmp(cmd, "autoremove") || !strcmp(cmd, "autopurge") || !strcmp(cmd, "reinstall");
  pkgdb db;
  if (db_load(&db, writing && !opt_sim) < 0) return 100;
  pkgindex ix;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (!opt_quiet && strcmp(who, "apt-cache")) progress("Reading package lists");
  index_load(&ix, &db);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (opt_stats) fprintf(stderr, "index: %u stanzas, %u versions, %u names, %zu KiB in the arena, loaded in %.0f ms, peak memory %ld KiB\n",
                         ix.nstanzas, ix.nvers, ix.nnames, ix.a.used_total / 1024, (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6, peak_kib());
  int rc = 0;
  solve_opts o = {0};
  o.recommends = aptconf_bool("APT::Install-Recommends", 1);
  if (writing || (opt_fix && !strcmp(cmd, "install"))) {
    if (opt_autoremove && !strcmp(cmd, "remove")) { rc = cmd_change(&db, &ix, cmd, rest, nrest, &o); if (!rc) rc = cmd_change(&db, &ix, "autoremove", 0, 0, &o); }
    else rc = cmd_change(&db, &ix, cmd, rest, nrest, &o);
  } else if (!strcmp(cmd, "show") || !strcmp(cmd, "showpkg")) rc = cmd_show(&ix, rest, nrest, !strcmp(who, "apt"));
  else if (!strcmp(cmd, "search")) rc = cmd_search(&ix, rest, nrest, aptconf_bool("APT::Cache::NamesOnly", 0));
  else if (!strcmp(cmd, "policy") || !strcmp(cmd, "madison")) rc = cmd_policy(&ix, rest, nrest);
  else if (!strcmp(cmd, "list")) rc = cmd_list(&ix, rest, nrest);
  else if (!strcmp(cmd, "depends")) rc = cmd_depends(&ix, rest, nrest);
  else if (!strcmp(cmd, "rdepends")) rc = cmd_rdepends(&ix, rest, nrest);
  else if (!strcmp(cmd, "pkgnames")) { int n; pkgname **v = sorted_names(&ix, &n); for (int i = 0; i < n; i++) if (!nrest || !strncmp(v[i]->name, rest[0], strlen(rest[0]))) printf("%s\n", v[i]->name); free(v); }
  else if (!strcmp(cmd, "stats")) {
    uint32_t virt = 0;
    for (uint32_t h = 0; h < ix.tabcap; h++) for (pkgname *pn = ix.tab[h]; pn; pn = pn->next_hash) if (!pn->vers && pn->nprov) virt++;
    printf("Total package names: %u\n  Normal packages: %u\n  Pure virtual packages: %u\nTotal distinct versions: %u\nIndex files: %d\nMemory for the index: %zu KiB\n",
           ix.nnames, ix.nnames - virt, virt, ix.nvers, ix.files.n - 1, ix.a.used_total / 1024);
  } else if (!strcmp(cmd, "download")) {
    for (int i = 0; i < nrest; i++) {
      solve_job j; char *err;
      if (parse_job(&ix, rest[i], JOB_INSTALL, &j, &err) < 0) { pkg_error("%s", err); free(err); rc = 100; continue; }
      pkgver *v = index_candidate(&ix, j.pn);
      if (j.verstr) for (pkgver *w = j.pn->vers; w; w = w->next) if (!strcmp(w->ver, j.verstr)) v = w;
      if (!v) { rc = 100; continue; }
      if (index_untrusted(&ix, v) && !aptconf_bool("APT::Get::AllowUnauthenticated", 0)) {
        fflush(stdout);
        fprintf(stderr, "E: %s comes from a repository that is not authenticated; --allow-unauthenticated downloads it anyway\n", v->pn->name);
        rc = 100; continue;
      }
      char *p = archive_path(v), *base = strrchr(p, '/');
      if (download(&ix, v, base + 1, i + 1) < 0) rc = 100;
      free(p);
    }
  } else if (!strcmp(cmd, "check")) {
    solve_result r;
    if (solve(&ix, &db, 0, 0, &o, &r) < 0) { printf("%s", r.why.s ? r.why.s : ""); rc = 100; }
    solve_result_free(&r);
  } else { pkg_error("invalid operation %s", cmd); rc = 100; }
  index_free(&ix);
  db_free(&db);
  sl_free(&args);
  pkg_unsupported_report();
  return rc;
}
