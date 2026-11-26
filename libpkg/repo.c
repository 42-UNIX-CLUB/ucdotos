// SPDX-License-Identifier: GPL-2.0-or-later
/* Repositories, as described on wiki.debian.org/DebianRepository/Format and in
 * sources.list(5):
 *   sources.list:  deb [arch=amd64 trusted=yes] URI SUITE COMPONENT...
 *   *.sources:     deb822 paragraphs with Types, URIs, Suites, Components, ...
 * apt update fetches dists/SUITE/InRelease (or Release), checks it, then for
 * each component and architecture the Packages index it lists, preferring
 * .xz, then .gz, then the plain file. Every index is checked against the size
 * and SHA-256 sum in Release. The OpenPGP signature on InRelease is NOT checked:
 * this code cannot verify signatures yet and says so on every update. Files go
 * to Dir::State::lists with apt's names (the URI with "/" turned into "_"). */
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

/* ---------- sources ---------- */
static void src_add(apt_sources *out, apt_source *s) {
  out->v = xrealloc(out->v, (out->n + 1) * sizeof *out->v);
  out->v[out->n++] = *s;
}
static char *norm_uri(const char *u) {
  size_t n = strlen(u);
  return n && u[n - 1] == '/' ? xstrdup(u) : xasprintf("%s/", u);
}
static void split_list(const char *v, const char *seps, strlist *out) {
  char *c = xstrdup(v), *save = 0;
  for (char *t = strtok_r(c, seps, &save); t; t = strtok_r(0, seps, &save)) sl_add(out, t);
  free(c);
}
int sources_parse_line(const char *line, const char *origin, apt_sources *out) {
  char *l = xstrdup(line);
  char *hash = strchr(l, '#');
  if (hash) *hash = 0;
  char *p = l;
  while (*p == ' ' || *p == '\t') p++;
  if (!*p || *p == '\n') { free(l); return 0; }
  char *type = p;
  p += strcspn(p, " \t");
  if (*p) *p++ = 0;
  if (strcmp(type, "deb") && strcmp(type, "deb-src")) { pkg_warn("%s: unknown type '%s'", origin, type); free(l); return -1; }
  int is_src = !strcmp(type, "deb-src");
  apt_source s = {0};
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '[') {
    char *e = strchr(p, ']');
    if (!e) { pkg_warn("%s: missing ']'", origin); free(l); return -1; }
    *e = 0;
    strlist opts = {0};
    split_list(p + 1, " \t", &opts);
    for (int i = 0; i < opts.n; i++) {
      char *eq = strchr(opts.v[i], '=');
      if (!eq) continue;
      *eq = 0;
      const char *k = opts.v[i], *v = eq + 1;
      if (!strcmp(k, "arch")) split_list(v, ",", &s.arches);
      else if (!strcmp(k, "trusted")) s.trusted = !strcmp(v, "yes");
      else if (!strcmp(k, "signed-by")) s.signed_by = xstrdup(v);
    }
    sl_free(&opts);
    p = e + 1;
  }
  strlist w = {0};
  split_list(p, " \t\r\n", &w);
  if (w.n < 2) { pkg_warn("%s: a source needs a URI and a suite", origin); sl_free(&w); free(l); return -1; }
  s.uri = norm_uri(w.v[0]);
  s.suite = xstrdup(w.v[1]);
  for (int i = 2; i < w.n; i++) sl_add(&s.comps, w.v[i]);
  size_t sl = strlen(s.suite);
  if (!(sl && s.suite[sl - 1] == '/') && !s.comps.n) { pkg_warn("%s: suite %s needs at least one component", origin, s.suite); }
  s.origin = xstrdup(origin);
  sl_free(&w);
  free(l);
  if (is_src) { /* source packages are not used here */
    free(s.uri); free(s.suite); sl_free(&s.comps); sl_free(&s.arches); free(s.signed_by); free(s.origin);
    return 0;
  }
  src_add(out, &s);
  return 0;
}
static int deb822_one(ctl_para *p, void *arg) {
  void **a = arg;
  apt_sources *out = a[0];
  const char *origin = a[1];
  const char *en = ctl_get(p, "Enabled");
  if (en && !strcasecmp(en, "no")) return 0;
  const char *types = ctl_get(p, "Types"), *uris = ctl_get(p, "URIs"), *suites = ctl_get(p, "Suites");
  if (!types || !uris || !suites) { pkg_warn("%s:%d: a source needs Types, URIs and Suites", origin, p->line); return 0; }
  strlist T = {0}, U = {0}, S = {0}, C = {0}, A = {0};
  split_list(types, " \t\n", &T); split_list(uris, " \t\n", &U); split_list(suites, " \t\n", &S);
  if (ctl_get(p, "Components")) split_list(ctl_get(p, "Components"), " \t\n", &C);
  if (ctl_get(p, "Architectures")) split_list(ctl_get(p, "Architectures"), " \t\n", &A);
  const char *tr = ctl_get(p, "Trusted"), *sb = ctl_get(p, "Signed-By");
  if (sl_find(&T, "deb") >= 0)
    for (int u = 0; u < U.n; u++)
      for (int s = 0; s < S.n; s++) {
        apt_source x = {0};
        x.uri = norm_uri(U.v[u]);
        x.suite = xstrdup(S.v[s]);
        for (int i = 0; i < C.n; i++) sl_add(&x.comps, C.v[i]);
        for (int i = 0; i < A.n; i++) sl_add(&x.arches, A.v[i]);
        x.trusted = tr && !strcasecmp(tr, "yes");
        x.signed_by = sb ? xstrdup(sb) : 0;
        x.origin = xasprintf("%s:%d", origin, p->line);
        src_add(out, &x);
      }
  sl_free(&T); sl_free(&U); sl_free(&S); sl_free(&C); sl_free(&A);
  return 0;
}
int sources_parse_deb822(const char *text, const char *origin, apt_sources *out) {
  ctl_reader *r = ctl_open_mem(text, strlen(text));
  ctl_para p = {0};
  void *arg[2] = {out, (void *)origin};
  int k;
  while ((k = ctl_next(r, &p)) == 1) deb822_one(&p, arg);
  if (k < 0) pkg_warn("%s: %s", origin, ctl_errmsg(r));
  ctl_free(&p);
  ctl_close(r);
  return k < 0 ? -1 : 0;
}
static void load_list_file(const char *path, apt_sources *out) {
  char *data;
  if (file_read(path, &data, 0) < 0) return;
  int ln = 0;
  for (char *line = data, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    ln++;
    char *o = xasprintf("%s:%d", path, ln);
    sources_parse_line(line, o, out);
    free(o);
  }
  free(data);
}
int sources_load(apt_sources *out) {
  memset(out, 0, sizeof *out);
  char *main = apt_dir("Dir::Etc::sourcelist", "sources.list");
  load_list_file(main, out);
  free(main);
  char *parts = apt_dir("Dir::Etc::sourceparts", "sources.list.d");
  DIR *d = opendir(parts);
  if (d) {
    strlist names = {0};
    struct dirent *e;
    while ((e = readdir(d))) sl_add(&names, e->d_name);
    closedir(d);
    sl_sort(&names);
    for (int i = 0; i < names.n; i++) {
      const char *n = names.v[i];
      size_t l = strlen(n);
      char *p = path_join(parts, n);
      if (l > 5 && !strcmp(n + l - 5, ".list")) load_list_file(p, out);
      else if (l > 8 && !strcmp(n + l - 8, ".sources")) { char *data; if (file_read(p, &data, 0) == 0) { sources_parse_deb822(data, p, out); free(data); } }
      free(p);
    }
    sl_free(&names);
  }
  free(parts);
  return 0;
}
void sources_free(apt_sources *s) {
  for (int i = 0; i < s->n; i++) {
    apt_source *x = &s->v[i];
    free(x->uri); free(x->suite); sl_free(&x->comps); sl_free(&x->arches); free(x->signed_by); free(x->origin);
  }
  free(s->v);
  memset(s, 0, sizeof *s);
}

/* apt's file names for lists/: the URI without its scheme (and user:pass@),
 * some characters %-escaped, "/" replaced by "_". */
char *apt_uri_to_filename(const char *uri) {
  const char *p = uri;
  const char *sch = strstr(p, "://");
  if (sch) p = sch + 3;
  else if (!strncmp(p, "file:", 5) || !strncmp(p, "copy:", 5)) p += 5;
  const char *at = strchr(p, '@'), *slash = strchr(p, '/');
  if (at && (!slash || at < slash)) p = at + 1;
  sbuf b = {0};
  for (; *p; p++) {
    unsigned char c = *p;
    if (strchr("\\|{}[]<>\"^~_=!@#$%^&*", c) || c <= 0x20 || c >= 0x7f) sb_printf(&b, "%%%02x", c);
    else sb_putc(&b, c == '/' ? '_' : c);
  }
  return b.s ? b.s : xstrdup("");
}

/* ---------- Release ---------- */
static char *strip_clearsign(const char *data, size_t n, int *was_signed) {
  *was_signed = 0;
  if (strncmp(data, "-----BEGIN PGP SIGNED MESSAGE-----", 34)) return xstrndup(data, n);
  *was_signed = 1;
  const char *p = strstr(data, "\n\n");
  if (!p) return xstrdup("");
  p += 2;
  const char *end = strstr(p, "\n-----BEGIN PGP SIGNATURE-----");
  if (!end) end = data + n;
  sbuf b = {0};
  while (p < end) {
    const char *nl = memchr(p, '\n', end - p);
    size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (l >= 2 && p[0] == '-' && p[1] == ' ') sb_add(&b, p + 2, l - 2); else sb_add(&b, p, l);
    sb_putc(&b, '\n');
    p += l + 1;
  }
  return b.s ? b.s : xstrdup("");
}
int release_parse(const char *data, size_t n, rel_file *out, char **err) {
  memset(out, 0, sizeof *out);
  *err = 0;
  char *body = strip_clearsign(data, n, &out->signed_clearsign);
  ctl_reader *r = ctl_open_mem(body, strlen(body));
  int k = ctl_next(r, &out->f);
  if (k != 1) { *err = xasprintf("bad Release file: %s", k < 0 ? ctl_errmsg(r) : "empty"); ctl_close(r); free(body); return -1; }
  ctl_close(r);
  free(body);
  const char *sums = ctl_get(&out->f, "SHA256");
  if (!sums) { *err = xstrdup("the Release file has no SHA256 sums (MD5 and SHA1 alone are not trusted)"); return -1; }
  char *c = xstrdup(sums);
  for (char *line = strtok(c, "\n"); line; line = strtok(0, "\n")) {
    char h[80], path[1024];
    unsigned long long size;
    if (sscanf(line, " %79s %llu %1023s", h, &size, path) != 3 || strlen(h) != 64) continue;
    out->e = xrealloc(out->e, (out->n + 1) * sizeof *out->e);
    rel_entry *e = &out->e[out->n++];
    e->path = xstrdup(path);
    e->size = size;
    memcpy(e->sha256, h, 65);
  }
  free(c);
  return 0;
}
void release_free(rel_file *r) {
  for (int i = 0; i < r->n; i++) free(r->e[i].path);
  free(r->e);
  ctl_free(&r->f);
  memset(r, 0, sizeof *r);
}
const rel_entry *release_find(const rel_file *r, const char *path) {
  for (int i = 0; i < r->n; i++) if (!strcmp(r->e[i].path, path)) return &r->e[i];
  return 0;
}
/* "Sat, 21 Nov 2026 09:15:44 UTC" (RFC 2822 as Debian writes it) */
static long long parse_date(const char *s) {
  static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const char *comma = strchr(s, ',');
  if (comma) s = comma + 1;
  int d, y, H, M, S;
  char m[8], zone[16] = "UTC";
  if (sscanf(s, " %d %3s %d %d:%d:%d %15s", &d, m, &y, &H, &M, &S, zone) < 6) return -1;
  int mi = -1;
  for (int i = 0; i < 12; i++) if (!strcmp(mon[i], m)) mi = i;
  if (mi < 0) return -1;
  struct tm tm = {0};
  tm.tm_mday = d; tm.tm_mon = mi; tm.tm_year = y - 1900; tm.tm_hour = H; tm.tm_min = M; tm.tm_sec = S;
  long long t = timegm(&tm);
  if (zone[0] == '+' || zone[0] == '-') {
    int z = atoi(zone + 1);
    long long off = (z / 100) * 3600 + (z % 100) * 60;
    t += zone[0] == '+' ? -off : off;
  }
  return t;
}

/* ---------- update ---------- */
static int sha256_file(const char *path, char out[65], uint64_t *size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  arc_sha256 c;
  arc_sha256_init(&c);
  char *buf = xmalloc(65536);
  long r;
  uint64_t n = 0;
  while ((r = read(fd, buf, 65536)) > 0) { arc_sha256_update(&c, buf, r); n += r; }
  free(buf);
  close(fd);
  if (r < 0) return -1;
  unsigned char d[32];
  arc_sha256_final(&c, d);
  hex_encode(d, 32, out);
  if (size) *size = n;
  return 0;
}
/* Decompress src into dest (atomically); check the result against want if given. */
static int decompress_to(const char *src, const char *dest, const rel_entry *want, char **why) {
  fd_reader fr = {0};
  fr.fd = open(src, O_RDONLY);
  if (fr.fd < 0) { *why = xasprintf("cannot open %s", src); return -1; }
  arc_stream *s = arc_open(ARC_AUTO, fd_reader_src, &fr);
  char *tmp = xasprintf("%s.new", dest);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0 || !s) {
    *why = out < 0 ? xasprintf("cannot create %s: %s", tmp, pkg_file_error(errno, tmp)) : xstrdup("out of memory");
    if (out >= 0) close(out);
    if (s) arc_close(s);
    close(fr.fd); free(tmp);
    return -1;
  }
  arc_sha256 c;
  arc_sha256_init(&c);
  char *buf = xmalloc(65536);
  long r;
  uint64_t n = 0;
  int rc = 0;
  while ((r = arc_read(s, buf, 65536)) > 0) {
    arc_sha256_update(&c, buf, r);
    n += r;
    if (write(out, buf, r) != r) { rc = -1; break; }
  }
  if (r < 0) { *why = xasprintf("cannot decompress %s: %s", src, arc_error(s)); rc = -1; }
  free(buf);
  fsync_fd(out, tmp);
  close(out);
  arc_close(s);
  close(fr.fd);
  if (rc == 0 && want) {
    unsigned char d[32]; char h[65];
    arc_sha256_final(&c, d); hex_encode(d, 32, h);
    if (n != want->size || strcmp(h, want->sha256)) { *why = xasprintf("the uncompressed index does not match the Release file (size %llu, want %llu)", (unsigned long long)n, (unsigned long long)want->size); rc = -1; }
  }
  if (rc == 0 && rename(tmp, dest) < 0) rc = -1;
  if (rc) unlink(tmp);
  free(tmp);
  return rc;
}

static void sig_warning(const apt_source *s) {
  fflush(stdout);
  fprintf(stderr, "W: The signature of %s %s was NOT checked. This system cannot verify OpenPGP signatures yet,\n"
                  "   so anyone who can change what you download can change these packages. Only the SHA-256\n"
                  "   sums from the Release file down were checked.%s\n",
          s->uri, s->suite, s->trusted ? " (The source is marked trusted=yes.)" : "");
}

/* Fetch one index listed in Release: try each compression, check size and sum. */
static int fetch_index(const char *base, const char *relpath, const rel_file *rel, const char *listdir, int by_hash, int keep_compressed, int *count) {
  /* Acquire::CompressionTypes::Order lists the preferred ones; the plain file comes last */
  strlist order = {0};
  aptconf_list("Acquire::CompressionTypes::Order", &order);
  if (!order.n) { sl_add(&order, "xz"); sl_add(&order, "gz"); sl_add(&order, "zst"); }
  sl_add(&order, "");
  int rc = -2;
  for (int i = 0; i < order.n && rc == -2; i++) {
      char ext[16];
      snprintf(ext, sizeof ext, "%s%s", order.v[i][0] ? "." : "", order.v[i]);
      char *rp = xasprintf("%s%s", relpath, ext);
      const rel_entry *e = release_find(rel, rp);
      if (!e) { free(rp); continue; }
      char *url = xasprintf("%s%s", base, rp);
      char *fname = apt_uri_to_filename(url);
      char *part = xasprintf("%s/partial/%s", listdir, fname);
      int fr = -2;
      if (by_hash) {
        char *dir = path_dirname(rp);
        char *hu = xasprintf("%s%s/by-hash/SHA256/%s", base, dir, e->sha256);
        fr = pkg_fetch(hu, part, 0);
        free(hu); free(dir);
      }
      if (fr != 0) fr = pkg_fetch(url, part, 0);
      if (fr == 0) {
        char h[65]; uint64_t sz;
        if (sha256_file(part, h, &sz) < 0 || sz != e->size || strcmp(h, e->sha256)) {
          pkg_error("%s: size or SHA-256 sum does not match the Release file; refusing it", url);
          unlink(part);
          rc = -1;
        } else {
          char *uncomp = xasprintf("%s%s", base, relpath);
          char *ufname = apt_uri_to_filename(uncomp);
          char *dest = xasprintf("%s/%s%s", listdir, ufname, keep_compressed ? ext : "");
          char *why = 0;
          if (keep_compressed || !*ext) {
            rc = rename(part, dest) == 0 ? 0 : -1;
            if (rc == 0) fsync_path(listdir);
          } else {
            rc = decompress_to(part, dest, release_find(rel, relpath), &why);
            unlink(part);
            if (rc < 0) { pkg_warn("%s: %s", url, why ? why : "failed"); free(why); rc = -2; }
          }
          if (rc == 0) { (*count)++; pkg_msg("Get: %s\n", url); }
          free(uncomp); free(ufname); free(dest);
        }
      } else if (fr == -1) rc = -1;
      free(rp); free(url); free(fname); free(part);
  }
  sl_free(&order);
  return rc;
}

static int arch_list(const apt_source *s, strlist *out) {
  if (s->arches.n) { for (int i = 0; i < s->arches.n; i++) sl_add(out, s->arches.v[i]); return 0; }
  sl_add(out, pkg_cfg.arch);
  for (int i = 0; i < pkg_cfg.foreign.n; i++) sl_add(out, pkg_cfg.foreign.v[i]);
  return 0;
}

int apt_update(void) {
  apt_sources srcs;
  sources_load(&srcs);
  if (!srcs.n) {
    char *sl = apt_dir("Dir::Etc::sourcelist", "sources.list");
    pkg_warn("no sources are configured (%s and sources.list.d/ are empty)", sl);
    free(sl);
    sources_free(&srcs);
    return 0;
  }
  char *listdir = apt_dir("Dir::State::lists", "lists/");
  char *partial = path_join(listdir, "partial");
  if (mkdir_p(partial, 0755) < 0) { pkg_error("cannot create %s: %s", partial, strerror(errno)); free(listdir); free(partial); sources_free(&srcs); return 100; }
  int errors = 0, nidx = 0;
  int check_valid = aptconf_bool("Acquire::Check-Valid-Until", 1);
  int keep_comp = aptconf_bool("Acquire::GzipIndexes", 0);
  int by_hash_ok = strcasecmp(aptconf_get("Acquire::By-Hash", "yes"), "no") != 0;
  for (int i = 0; i < srcs.n; i++) {
    apt_source *s = &srcs.v[i];
    int dup = 0;
    for (int j = 0; j < i; j++) if (!strcmp(srcs.v[j].uri, s->uri) && !strcmp(srcs.v[j].suite, s->suite)) dup = 1;
    size_t sl = strlen(s->suite);
    int flat = sl && s->suite[sl - 1] == '/';
    char *distbase = flat ? xasprintf("%s%s", s->uri, strcmp(s->suite, "./") ? s->suite : "") : xasprintf("%sdists/%s/", s->uri, s->suite);
    char *inrel_url = xasprintf("%sInRelease", distbase), *rel_url = xasprintf("%sRelease", distbase);
    char *inrel_name = apt_uri_to_filename(inrel_url), *rel_name = apt_uri_to_filename(rel_url);
    char *inrel_part = xasprintf("%s/%s", partial, inrel_name), *rel_part = xasprintf("%s/%s", partial, rel_name);
    char *got = 0, *final = 0;
    if (pkg_fetch(inrel_url, inrel_part, 0) == 0) { got = inrel_part; final = xasprintf("%s/%s", listdir, inrel_name); pkg_msg("Hit: %s\n", inrel_url); }
    else if (pkg_fetch(rel_url, rel_part, 0) == 0) { got = rel_part; final = xasprintf("%s/%s", listdir, rel_name); pkg_msg("Hit: %s\n", rel_url); }
    if (!got) {
      pkg_error("cannot fetch %sInRelease or %sRelease", distbase, distbase);
      errors++;
      goto next;
    }
    {
      char *data; size_t n;
      file_read(got, &data, &n);
      rel_file rel;
      char *err;
      if (release_parse(data, n, &rel, &err) < 0) { pkg_error("%s: %s", got == inrel_part ? inrel_url : rel_url, err); free(err); free(data); errors++; unlink(got); goto next; }
      free(data);
      if (!dup) sig_warning(s);
      const char *vu = ctl_get(&rel.f, "Valid-Until");
      if (vu && check_valid) {
        long long t = parse_date(vu);
        if (t > 0 && t < (long long)time(0)) {
          pkg_error("the Release file of %s %s expired on %s; refusing it (Acquire::Check-Valid-Until \"false\" overrides)", s->uri, s->suite, vu);
          release_free(&rel); errors++; unlink(got); goto next;
        }
      }
      const char *bh = ctl_get(&rel.f, "Acquire-By-Hash");
      int by_hash = by_hash_ok && bh && !strcasecmp(bh, "yes");
      const char *nsa = ctl_get(&rel.f, "No-Support-for-Architecture-all");
      int all_in_arch = nsa && strstr(nsa, "Packages");
      strlist arches = {0};
      arch_list(s, &arches);
      if (!all_in_arch && sl_find(&arches, "all") < 0) sl_add(&arches, "all");
      int ncomp = flat ? 1 : s->comps.n;
      for (int c = 0; c < ncomp; c++)
        for (int a = 0; a < arches.n; a++) {
          char *relpath = flat ? xstrdup("Packages") : xasprintf("%s/binary-%s/Packages", s->comps.v[c], arches.v[a]);
          int r = fetch_index(distbase, relpath, &rel, listdir, by_hash, keep_comp, &nidx);
          if (r == -2 && strcmp(arches.v[a], "all")) {
            /* not listed: an architecture the archive does not carry */
            const char *archs = ctl_get(&rel.f, "Architectures");
            if (!archs || strstr(archs, arches.v[a])) { pkg_warn("%s%s is not available", distbase, relpath); }
          } else if (r == -1) errors++;
          free(relpath);
          if (flat) break;
        }
      sl_free(&arches);
      release_free(&rel);
      if (rename(got, final) < 0) { pkg_error("cannot store %s: %s", final, pkg_file_error(errno, final)); errors++; }
      /* only one of InRelease and Release is kept, as apt does */
      char *other = xasprintf("%s/%s", listdir, got == inrel_part ? rel_name : inrel_name);
      unlink(other);
      free(other);
    }
  next:
    free(distbase); free(inrel_url); free(rel_url); free(inrel_name); free(rel_name); free(inrel_part); free(rel_part); free(final);
  }
  pkg_msg("Fetched %d indexes.\n", nidx);
  free(listdir); free(partial);
  sources_free(&srcs);
  if (errors) { pkg_warn("some index files failed to download; they have been ignored, or old ones used instead"); return 100; }
  return 0;
}
