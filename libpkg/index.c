// SPDX-License-Identifier: GPL-2.0-or-later
/* A compact in-memory view of Packages indexes and the status file.
 * Indexes are parsed as a stream, one paragraph at a time. Per version only
 * the fields dependency resolution needs are kept, copied into an arena; small
 * repeated strings (architectures, sections) are shared. The rest of a stanza
 * (Filename, SHA256, Description, ...) stays on disk and is re-read through
 * the stanza's byte offset when needed. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

int prio_value(const char *s) {
  if (!s) return 0;
  if (!strcmp(s, "required")) return 5;
  if (!strcmp(s, "important")) return 4;
  if (!strcmp(s, "standard")) return 3;
  if (!strcmp(s, "optional")) return 2;
  if (!strcmp(s, "extra")) return 1;
  return 0;
}
const char *prio_name(int p) {
  static const char *const n[] = {"unknown", "extra", "optional", "standard", "important", "required"};
  return p >= 0 && p <= 5 ? n[p] : "unknown";
}
int arch_ok_native(const char *a) {
  return !a || !strcmp(a, "all") || !strcmp(a, pkg_cfg.arch) || sl_find(&pkg_cfg.foreign, a) >= 0;
}

static const char *intern(pkgindex *ix, const char *s) {
  if (!s) return 0;
  void **slot = sm_slot(&ix->intern, s);
  if (!*slot) *slot = ar_strndup(&ix->a, s, strlen(s));
  return *slot;
}
static const char *keep(pkgindex *ix, const char *s) {
  if (!s || !*s) return 0;
  return ar_strndup(&ix->a, s, strlen(s));
}

pkgname *index_name(pkgindex *ix, const char *name, int create) {
  if (!ix->tabcap) { ix->tabcap = 1 << 16; ix->tab = xcalloc(ix->tabcap, sizeof *ix->tab); }
  uint32_t h = hash_str(name, strlen(name)) & (ix->tabcap - 1);
  for (pkgname *p = ix->tab[h]; p; p = p->next_hash) if (!strcmp(p->name, name)) return p;
  if (!create) return 0;
  if (ix->nnames * 2 > ix->tabcap) {
    uint32_t nc = ix->tabcap * 2;
    pkgname **nt = xcalloc(nc, sizeof *nt);
    for (uint32_t i = 0; i < ix->tabcap; i++)
      for (pkgname *p = ix->tab[i], *n; p; p = n) {
        n = p->next_hash;
        uint32_t j = hash_str(p->name, strlen(p->name)) & (nc - 1);
        p->next_hash = nt[j]; nt[j] = p;
      }
    free(ix->tab);
    ix->tab = nt; ix->tabcap = nc;
    h = hash_str(name, strlen(name)) & (nc - 1);
  }
  pkgname *p = ar_alloc(&ix->a, sizeof *p);
  memset(p, 0, sizeof *p);
  p->name = ar_strndup(&ix->a, name, strlen(name));
  p->next_hash = ix->tab[h];
  ix->tab[h] = p;
  ix->nnames++;
  return p;
}

/* Insert keeping versions highest first; returns the existing entry for an exact duplicate. */
static pkgver *insert_version(pkgname *pn, pkgver *v) {
  pkgver **pp = &pn->vers;
  while (*pp) {
    int c = ver_cmp(v->ver, (*pp)->ver);
    if (c == 0 && !strcmp(v->arch ? v->arch : "", (*pp)->arch ? (*pp)->arch : "")) return *pp;
    if (c > 0) break;
    pp = &(*pp)->next;
  }
  v->next = *pp;
  *pp = v;
  return 0;
}

typedef struct { pkgindex *ix; uint16_t fid; } addctx;
static pkgver *from_para(pkgindex *ix, const ctl_para *p, uint16_t fid, uint64_t off) {
  const char *name = 0, *ver = 0, *arch = 0;
  pkgver tmp;
  memset(&tmp, 0, sizeof tmp);
  for (int i = 0; i < p->n; i++) {
    const char *n = ctl_name(p, i), *v = ctl_value(p, i);
    switch (n[0] | 0x20) {
    case 'p':
      if (!strcasecmp(n, "Package")) name = v;
      else if (!strcasecmp(n, "Pre-Depends")) tmp.predepends = v;
      else if (!strcasecmp(n, "Provides")) tmp.provides = v;
      else if (!strcasecmp(n, "Priority")) tmp.prio = prio_value(v);
      break;
    case 'v': if (!strcasecmp(n, "Version")) ver = v; break;
    case 'a': if (!strcasecmp(n, "Architecture")) arch = v; break;
    case 'd': if (!strcasecmp(n, "Depends")) tmp.depends = v; break;
    case 'r':
      if (!strcasecmp(n, "Recommends")) tmp.recommends = v;
      else if (!strcasecmp(n, "Replaces")) tmp.replaces = v;
      break;
    case 's':
      if (!strcasecmp(n, "Suggests")) tmp.suggests = v;
      else if (!strcasecmp(n, "Size")) tmp.size = strtoull(v, 0, 10);
      else if (!strcasecmp(n, "Section")) tmp.section = v;
      break;
    case 'c': if (!strcasecmp(n, "Conflicts")) tmp.conflicts = v; break;
    case 'b': if (!strcasecmp(n, "Breaks")) tmp.breaks = v; break;
    case 'e':
      if (!strcasecmp(n, "Essential")) tmp.essential = !strcasecmp(v, "yes");
      else if (!strcasecmp(n, "Enhances")) tmp.enhances = v;
      break;
    case 'i':
      if (!strcasecmp(n, "Installed-Size")) tmp.isize = strtoul(v, 0, 10);
      else if (!strcasecmp(n, "Important")) tmp.important = !strcasecmp(v, "yes");
      break;
    case 'm': if (!strcasecmp(n, "Multi-Arch")) tmp.multiarch = v; break;
    }
  }
  if (!name || !ver) return 0;
  pkgname *pn = index_name(ix, name, 1);
  pkgver *v = ar_alloc(&ix->a, sizeof *v);
  *v = tmp;
  v->pn = pn;
  v->ver = keep(ix, ver);
  v->arch = intern(ix, arch ? arch : "all");
  v->section = intern(ix, tmp.section);
  v->multiarch = intern(ix, tmp.multiarch);
  v->depends = keep(ix, tmp.depends);
  v->predepends = keep(ix, tmp.predepends);
  v->recommends = keep(ix, tmp.recommends);
  v->suggests = keep(ix, tmp.suggests);
  v->conflicts = keep(ix, tmp.conflicts);
  v->breaks = keep(ix, tmp.breaks);
  v->replaces = keep(ix, tmp.replaces);
  v->provides = keep(ix, tmp.provides);
  v->enhances = keep(ix, tmp.enhances);
  v->file_id = fid;
  v->offset = off;
  v->pin = fid ? 500 : 100;
  pkgver *dup = insert_version(pn, v);
  if (dup) {
    /* the same version from two places: keep the first, prefer a repository copy over the status file */
    if (dup->file_id == 0 && fid) { dup->file_id = fid; dup->offset = off; dup->pin = 500; dup->size = v->size; }
    return dup;
  }
  ix->nvers++;
  return v;
}

int index_add_file(pkgindex *ix, const char *path, const char *base, const char *label) {
  fd_reader fr = {0};
  fr.fd = open(path, O_RDONLY);
  if (fr.fd < 0) return -1;
  uint16_t fid = ix->files.n;
  sl_add(&ix->files, path);
  sl_add(&ix->file_base, base ? base : "");
  sl_add(&ix->file_label, label ? label : "");
  arc_stream *s = arc_open(ARC_AUTO, fd_reader_src, &fr);
  ctl_reader *r = ctl_open(arc_stream_src, s);
  ctl_para p = {0};
  int k, n = 0;
  while ((k = ctl_next(r, &p)) == 1) { from_para(ix, &p, fid, ctl_offset(r)); n++; }
  if (k < 0) pkg_error("%s: %s", path, ctl_errmsg(r));
  ix->nstanzas += n;
  ctl_free(&p);
  ctl_close(r);
  arc_close(s);
  close(fr.fd);
  return k < 0 ? -1 : n;
}

void index_add_status(pkgindex *ix, pkgdb *db) {
  ix->db = db;
  if (!ix->files.n) { sl_add(&ix->files, "status"); sl_add(&ix->file_base, ""); sl_add(&ix->file_label, pkg_cfg.admindir); }
  for (int i = 0; i < db->n; i++) {
    pkginfo *pi = db->p[i];
    pkgname *pn = index_name(ix, pi->name, 1);
    if (!pn->inst || pi_present(pi)) pn->inst = pi;
    if (!pi_present(pi)) continue;
    ctl_para tmp = {0};
    ctl_copy(&tmp, &pi->f);
    ctl_set(&tmp, "Package", pi->name);
    if (pi->arch) ctl_set(&tmp, "Architecture", pi->arch);
    pkgver *v = from_para(ix, &tmp, 0, (uint64_t)i);
    if (v) v->installed = 1;
    ctl_free(&tmp);
  }
}

static void add_provider(pkgname *pn, pkgver *v) {
  for (uint32_t i = 0; i < pn->nprov; i++) if (pn->prov[i] == v) return;
  if (pn->nprov == pn->capprov) { pn->capprov = pn->capprov ? pn->capprov * 2 : 4; pn->prov = xrealloc(pn->prov, pn->capprov * sizeof *pn->prov); }
  pn->prov[pn->nprov++] = v;
}
void index_finish(pkgindex *ix) {
  for (uint32_t h = 0; h < ix->tabcap; h++)
    for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash)
      for (pkgver *v = pn->vers; v; v = v->next) {
        if (!v->provides) continue;
        dep_list l;
        if (dep_parse(v->provides, DEP_PROVIDES, &l, 0) < 0) continue;
        for (int i = 0; i < l.n; i++) add_provider(index_name(ix, l.o[i].a[0].name, 1), v);
        dep_free(&l);
      }
}

static char *list_name(const char *listdir, const char *url) {
  char *f = apt_uri_to_filename(url);
  char *p = path_join(listdir, f);
  free(f);
  return p;
}
static void set_untrusted(pkgindex *ix, int untrusted) {
  int fid = ix->files.n - 1;
  if (fid >= ix->nfile_untrusted) {
    int n = fid + 16;
    ix->file_untrusted = xrealloc(ix->file_untrusted, n);
    memset(ix->file_untrusted + ix->nfile_untrusted, 0, n - ix->nfile_untrusted);
    ix->nfile_untrusted = n;
  }
  ix->file_untrusted[fid] = (unsigned char)untrusted;
}
int index_untrusted(const pkgindex *ix, const pkgver *v) {
  return v->file_id > 0 && v->file_id < ix->nfile_untrusted && ix->file_untrusted[v->file_id];
}
int index_load(pkgindex *ix, pkgdb *db) {
  memset(ix, 0, sizeof *ix);
  index_add_status(ix, db);
  apt_sources srcs;
  sources_load(&srcs);
  char *listdir = apt_dir("Dir::State::lists", "lists/");
  strmap seen = {0};
  for (int i = 0; i < srcs.n; i++) {
    apt_source *s = &srcs.v[i];
    size_t sl = strlen(s->suite);
    int flat = sl && s->suite[sl - 1] == '/';
    char *distbase = flat ? xasprintf("%s%s", s->uri, strcmp(s->suite, "./") ? s->suite : "") : xasprintf("%sdists/%s/", s->uri, s->suite);
    strlist arches = {0};
    if (s->arches.n) for (int a = 0; a < s->arches.n; a++) sl_add(&arches, s->arches.v[a]);
    else { sl_add(&arches, pkg_cfg.arch); for (int a = 0; a < pkg_cfg.foreign.n; a++) sl_add(&arches, pkg_cfg.foreign.v[a]); }
    sl_add(&arches, "all");
    /* authenticated: apt update keeps InRelease or Release.gpg only after the signature checked */
    char *u1 = xasprintf("%sInRelease", distbase), *u2 = xasprintf("%sRelease.gpg", distbase);
    char *inrel = list_name(listdir, u1), *relgpg = list_name(listdir, u2);
    int untrusted = !s->trusted && !path_exists(inrel) && !path_exists(relgpg);
    free(u1); free(u2); free(inrel); free(relgpg);
    int ncomp = flat ? 1 : s->comps.n;
    for (int c = 0; c < ncomp; c++)
      for (int a = 0; a < arches.n; a++) {
        char *url = flat ? xasprintf("%sPackages", distbase) : xasprintf("%s%s/binary-%s/Packages", distbase, s->comps.v[c], arches.v[a]);
        char *path = list_name(listdir, url);
        static const char *const ext[] = {"", ".xz", ".gz", ".zst", 0};
        for (int e = 0; ext[e]; e++) {
          char *p = xasprintf("%s%s", path, ext[e]);
          if (!sm_get(&seen, p) && path_exists(p)) {
            *sm_slot(&seen, p) = (void *)1;
            char *label = flat ? xasprintf("%s %s", s->uri, s->suite) : xasprintf("%s %s/%s %s", s->uri, s->suite, s->comps.v[c], arches.v[a]);
            index_add_file(ix, p, s->uri, label);
            set_untrusted(ix, untrusted);
            free(label);
            free(p);
            break;
          }
          free(p);
        }
        free(url); free(path);
        if (flat) break;
      }
    sl_free(&arches);
    free(distbase);
  }
  sm_free(&seen);
  free(listdir);
  sources_free(&srcs);
  index_finish(ix);
  return 0;
}

pkgver *index_installed(pkgname *pn) {
  for (pkgver *v = pn->vers; v; v = v->next) if (v->installed) return v;
  return 0;
}
pkgver *index_candidate(pkgindex *ix, pkgname *pn) {
  (void)ix;
  pkgver *inst = index_installed(pn), *best = 0;
  const char *want_arch = inst && inst->arch && strcmp(inst->arch, "all") ? inst->arch : 0;
  for (pkgver *v = pn->vers; v; v = v->next) {
    if (!arch_ok_native(v->arch)) continue;
    if (want_arch && strcmp(v->arch, want_arch) && strcmp(v->arch, "all")) continue;
    if (!want_arch && strcmp(v->arch, "all") && strcmp(v->arch, pkg_cfg.arch) && best) continue;
    if (!best || v->pin > best->pin || (v->pin == best->pin && ver_cmp(v->ver, best->ver) > 0)) best = v;
  }
  /* apt does not downgrade on its own: an installed version above the candidate stays */
  if (inst && best && ver_cmp(inst->ver, best->ver) > 0) return inst;
  return best ? best : inst;
}

int index_stanza(pkgindex *ix, const pkgver *v, ctl_para *out) {
  ctl_reset(out);
  if (v->file_id == 0) {
    if (!ix->db || v->offset >= (uint64_t)ix->db->n) return -1;
    pkginfo *pi = ix->db->p[v->offset];
    ctl_set(out, "Package", pi->name);
    for (int i = 0; i < pi->f.n; i++) ctl_set(out, ctl_name(&pi->f, i), ctl_value(&pi->f, i));
    return 0;
  }
  const char *path = ix->files.v[v->file_id];
  fd_reader fr = {0};
  fr.fd = open(path, O_RDONLY);
  if (fr.fd < 0) return -1;
  size_t pl = strlen(path);
  int plain = !(pl > 3 && (!strcmp(path + pl - 3, ".xz") || !strcmp(path + pl - 3, ".gz") || !strcmp(path + pl - 4, ".zst")));
  int rc = -1;
  if (plain) {
    if (lseek(fr.fd, (off_t)v->offset, SEEK_SET) == (off_t)v->offset) {
      ctl_reader *r = ctl_open(fd_reader_src, &fr);
      if (ctl_next(r, out) == 1) rc = 0;
      ctl_close(r);
    }
  } else {
    /* compressed index: read forward to the offset */
    arc_stream *s = arc_open(ARC_AUTO, fd_reader_src, &fr);
    ctl_reader *r = ctl_open(arc_stream_src, s);
    while (ctl_next(r, out) == 1) if (ctl_offset(r) == v->offset) { rc = 0; break; }
    ctl_close(r);
    arc_close(s);
  }
  close(fr.fd);
  return rc;
}

void index_free(pkgindex *ix) {
  for (uint32_t h = 0; h < ix->tabcap; h++) for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) free(pn->prov);
  free(ix->tab);
  ar_free(&ix->a);
  sl_free(&ix->files); sl_free(&ix->file_base); sl_free(&ix->file_label);
  free(ix->file_untrusted);
  sm_free(&ix->intern);
  memset(ix, 0, sizeof *ix);
}
