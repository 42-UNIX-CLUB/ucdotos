// SPDX-License-Identifier: GPL-2.0-or-later
/* libpkg: the package manager's logic. Debian formats (deb(5), deb-control(5),
 * deb-version(7), the dpkg database, apt's repository format), written from
 * their documentation. The programs dpkg, dpkg-deb, dpkg-query and apt are thin
 * front ends over this library. */
#ifndef PKG_H
#define PKG_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../libarc/arc.h"

/* ================= util.c ================= */
void *xmalloc(size_t n);
void *xcalloc(size_t a, size_t b);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void xqsort(void *b, size_t n, size_t sz, int (*cmp)(const void *, const void *));  /* qsort that accepts n == 0 */
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef struct { char *s; size_t n, cap; } sbuf;  /* growable string, always NUL-terminated */
void sb_add(sbuf *b, const void *p, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_putc(sbuf *b, int c);
void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_free(sbuf *b);

typedef struct { char **v; int n, cap; } strlist;
void sl_add(strlist *l, const char *s);            /* copies s */
void sl_free(strlist *l);
int sl_find(const strlist *l, const char *s);       /* index or -1 */
void sl_sort(strlist *l);

/* A string-keyed hash map; keys are copied. */
typedef struct { char **k; void **v; uint32_t cap, n; } strmap;
void *sm_get(const strmap *m, const char *key);
void **sm_slot(strmap *m, const char *key);         /* creates the key (value NULL) if missing */
void sm_free(strmap *m);
uint32_t hash_str(const char *s, size_t n);

/* An arena: many small allocations freed all at once. Keeps the libc heap's free
 * list short, which matters with a first-fit allocator. */
typedef struct arena_blk arena_blk;
typedef struct { arena_blk *head; size_t used_total; } arena;
void *ar_alloc(arena *a, size_t n);
char *ar_strndup(arena *a, const char *s, size_t n);
void ar_free(arena *a);

/* messages */
extern const char *pkg_progname;
extern int pkg_verbose, pkg_quiet;
void pkg_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pkg_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pkg_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A system call answered "not supported" (ENOSYS and friends). Warns once per
 * operation, counts, and the caller carries on. */
void pkg_unsupported(const char *what, const char *path);
int pkg_unsupported_count(void);
void pkg_unsupported_report(void);

/* files */
int file_read(const char *path, char **data, size_t *n);   /* 0 ok; data NUL-terminated */
int file_write_atomic(const char *path, const void *data, size_t n, unsigned mode);
int file_copy(const char *from, const char *to, unsigned mode);
const char *pkg_file_error(int err, const char *path);  /* strerror, plus the length and the limit for ENAMETOOLONG */
int mkdir_p(const char *path, unsigned mode);
int path_exists(const char *path);   /* lstat succeeds */
int path_is_dir(const char *path);
char *path_join(const char *a, const char *b);   /* a + "/" + b without doubled slashes */
char *path_dirname(const char *p);
void fsync_path(const char *path);
void fsync_fd(int fd, const char *what);
int hex_digest_file(const char *path, int md5, char *out /* 65 bytes */);
void hex_encode(const unsigned char *in, int n, char *out);
/* Reading through libarc: an arc_src over a file descriptor with an optional limit. */
typedef struct { int fd; uint64_t left; int limited; } fd_reader;
long fd_reader_src(void *ctx, void *buf, unsigned long n);
long arc_stream_src(void *ctx, void *buf, unsigned long n);  /* ctx is an arc_stream */

/* ================= control.c: RFC 822 style paragraphs ================= */
/* Field names and values live in one buffer per paragraph, so parsing a large
 * index makes no per-field allocations. Values of multi-line fields keep their
 * continuation lines, each after a "\n", with their leading space. */
typedef struct { uint32_t name, value; } ctl_field;
typedef struct { char *buf; size_t len, cap; ctl_field *f; int n, fcap; int line; } ctl_para;
const char *ctl_get(const ctl_para *p, const char *name);   /* case-insensitive, NULL if absent */
void ctl_set(ctl_para *p, const char *name, const char *value);  /* replaces or appends */
void ctl_del(ctl_para *p, const char *name);
void ctl_reset(ctl_para *p);
void ctl_free(ctl_para *p);
void ctl_copy(ctl_para *dst, const ctl_para *src);
const char *ctl_name(const ctl_para *p, int i);
const char *ctl_value(const ctl_para *p, int i);
void ctl_write(sbuf *out, const ctl_para *p);  /* "Name: value\n" lines, no blank line */

typedef struct ctl_reader ctl_reader;
ctl_reader *ctl_open(arc_src src, void *ctx);
ctl_reader *ctl_open_mem(const char *data, size_t n);
int ctl_next(ctl_reader *r, ctl_para *p);      /* 1 paragraph, 0 end, -1 error (ctl_errmsg) */
const char *ctl_errmsg(ctl_reader *r);
uint64_t ctl_offset(ctl_reader *r);            /* byte offset where the last paragraph started */
void ctl_close(ctl_reader *r);
/* Parse a whole file: calls fn for each paragraph. Returns 0, or -1 with a message printed. */
int ctl_parse_file(const char *path, int (*fn)(ctl_para *p, void *arg), void *arg);
int ctl_field_name_ok(const char *s, size_t n);

/* ================= version.c: deb-version(7) ================= */
int ver_cmp(const char *a, const char *b);     /* <0, 0, >0 */
const char *ver_check(const char *v);          /* NULL if valid, else why not */
enum { REL_NONE, REL_LT, REL_LE, REL_EQ, REL_GE, REL_GT };
int ver_satisfies(const char *have, int op, const char *want);
const char *rel_str(int op);

/* ================= deps.c: relationship fields ================= */
typedef struct {
  char *name;
  char *archq;      /* ":any", ":native", ":ARCH" qualifier without the colon, or NULL */
  int op;           /* REL_* */
  char *ver;        /* NULL if op == REL_NONE */
  strlist arches;   /* [arch !arch] restriction list (source fields) */
  int arch_neg;
  char *profiles;   /* <...> build profile formula kept as text */
} dep_atom;
typedef struct { dep_atom *a; int n; } dep_or;     /* alternatives joined by | */
typedef struct { dep_or *o; int n; } dep_list;     /* comma-separated */
enum { DEP_ALT = 1, DEP_RESTRICT = 2, DEP_PROVIDES = 4 };
int dep_parse(const char *text, int flags, dep_list *out, char **err);
void dep_free(dep_list *l);
void dep_format(sbuf *out, const dep_list *l);
void dep_atom_format(sbuf *out, const dep_atom *a);
/* Drop atoms whose [arch] list excludes `arch` (and empty groups). */
void dep_reduce_arch(dep_list *l, const char *arch);
int arch_matches(const char *pattern, const char *arch);  /* "any", "linux-any", "any-amd64", exact */

/* ================= conf.c: configuration ================= */
enum {
  F_DEPENDS = 1 << 0, F_CONFLICTS = 1 << 1, F_BREAKS = 1 << 2, F_OVERWRITE = 1 << 3,
  F_OVERWRITE_DIR = 1 << 4, F_CONFOLD = 1 << 5, F_CONFNEW = 1 << 6, F_CONFDEF = 1 << 7,
  F_CONFMISS = 1 << 8, F_REMOVE_ESSENTIAL = 1 << 9, F_REMOVE_REINSTREQ = 1 << 10,
  F_DOWNGRADE = 1 << 11, F_ARCHITECTURE = 1 << 12, F_BAD_VERSION = 1 << 13,
  F_SCRIPT_CHROOTLESS = 1 << 14, F_UNSAFE_IO = 1 << 15, F_UNRUNNABLE_SCRIPTS = 1 << 16,
  F_DEPENDS_VERSION = 1 << 17, F_NOT_ROOT = 1 << 18, F_BAD_PATH = 1 << 19,
  F_CONFFILE_ASK = 1 << 20, F_HOLD = 1 << 21
};
typedef struct {
  char *root;          /* --root / --instdir: where package files go */
  char *admindir;      /* --admindir: the database */
  char *arch;          /* the native architecture: the ONE place it is decided */
  strlist foreign;     /* dpkg --add-architecture */
  unsigned force;
  int no_act;          /* --no-act / --dry-run / --simulate */
  int no_triggers;
  char *script_runner; /* command prefix that can run the root's own programs (the binary engine) */
  char *script_shell;  /* shell used for scripts run "chrootless" */
  char *confdir;       /* /etc/dpkg */
  char *logfile;
} pkg_config;
extern pkg_config pkg_cfg;
/* Defaults, then the config files, then the environment (DPKG_ROOT, DPKG_ADMINDIR). */
void pkg_config_init(void);
/* Applies one dpkg option, "--force-x" or "root=dir" (from dpkg.cfg). 1 used, 0 unknown, -1 bad. */
int pkg_config_option(const char *opt, const char *val);
int pkg_force_parse(const char *list, int on);
void pkg_config_finish(void);  /* derives admindir, reads the arch file */
const char *pkg_force_help(void);
char *root_path(const char *pkgpath);  /* root + path, malloc'd */
int arch_is_known(const char *a);      /* native, foreign, or all */

/* ================= deb.c: .deb files ================= */
typedef struct { char name[64]; unsigned char *data; size_t size; unsigned mode; } deb_cfile;
typedef struct deb {
  int fd;
  fd_reader rd;
  char fmtver[16];
  uint64_t file_size, control_size, data_size;
  char control_member[64], data_member[64];
  deb_cfile *cf; int ncf;
  ctl_para control;
  arc_stream *data;
} deb;
deb *deb_open(const char *path, char **err);
const deb_cfile *deb_cfile_get(const deb *d, const char *name);
int deb_data_begin(deb *d, int raw, char **err);  /* raw: decompressed tar bytes via arc_read */
void deb_close(deb *d);
/* "./usr/bin/x" -> "/usr/bin/x"; returns 0 if the path is unsafe (".." or absolute escape). */
int deb_norm_path(const char *in, char *out, size_t cap);

/* ================= db.c: the dpkg database ================= */
enum { W_UNKNOWN, W_INSTALL, W_HOLD, W_DEINSTALL, W_PURGE };
enum { E_OK, E_REINSTREQ };
enum { S_NOT_INSTALLED, S_CONFIG_FILES, S_HALF_INSTALLED, S_UNPACKED, S_HALF_CONFIGURED,
       S_TRIGGERS_AWAITED, S_TRIGGERS_PENDING, S_INSTALLED };
extern const char *const want_names[], *const eflag_names[], *const status_names[];
typedef struct pkginfo {
  char *name, *arch;
  int want, eflag, status;
  ctl_para f;        /* every field except Package and Status */
  int mark;          /* scratch for callers */
} pkginfo;
typedef struct {
  pkginfo **p; int n, cap;
  strmap byname;     /* "name" -> first pkginfo with that name (see db_find) */
  int lockfd;
  int dirty;
} pkgdb;
int db_load(pkgdb *db, int lock);
int db_save(pkgdb *db);
void db_free(pkgdb *db);
pkginfo *db_find(pkgdb *db, const char *name, const char *arch);  /* arch NULL: any (single instance) */
pkginfo *db_get(pkgdb *db, const char *name, const char *arch);   /* creates as not-installed */
void db_remove_entry(pkgdb *db, pkginfo *p);
const char *pi_version(const pkginfo *p);
int pi_present(const pkginfo *p);      /* files on disk: anything but not-installed/config-files */
int pi_configured(const pkginfo *p);   /* installed, or triggers-*: satisfies Depends */
char *pi_infoname(const pkginfo *p, const char *ext);  /* admindir/info/NAME[:ARCH].ext */
const char *pi_key(const pkginfo *p, char *buf, size_t cap);  /* "name" or "name:arch" */
int db_read_list(const pkginfo *p, strlist *files);
int db_write_list(const pkginfo *p, const strlist *files);
/* Every path owned by any present package: path -> pkginfo* (first owner). */
void db_file_owners(pkgdb *db, strmap *out, pkginfo *skip);
/* Conffiles field <-> list of (path, md5, obsolete) */
typedef struct { char *path; char hash[40]; int obsolete; } conffile;
int conffiles_parse(const char *field, conffile **out);
void conffiles_free(conffile *c, int n);
void db_write_pkg(sbuf *b, const pkginfo *p);
int pkg_glob(const char *pat, const char *s);
void db_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ================= dpkg.c: install, remove, configure ================= */
int dpkg_unpack(pkgdb *db, const char *debpath, int configure_after);
int dpkg_configure(pkgdb *db, pkginfo *p, int in_batch);
int dpkg_configure_pending(pkgdb *db);  /* --configure -a */
int dpkg_remove(pkgdb *db, pkginfo *p, int purge);
int dpkg_triggers_run(pkgdb *db);
int dpkg_check_depends(pkgdb *db, pkginfo *p, const char *field, int configured_only, sbuf *why);
int deps_satisfied_by_db(pkgdb *db, const dep_or *o, const char *pkgarch, int configured_only, pkginfo *skip);
extern strlist dpkg_batch;  /* package keys being processed together (cycles) */

/* ================= script.c: maintainer scripts ================= */
/* Runs admindir/info/PKG.NAME (or an explicit path) with args. Returns its exit
 * status, 0 if it does not exist, or -1 if it cannot run here; *why says why. */
int script_run(const pkginfo *p, const char *path, const char *name, const char *const args[], char **why);
int run_command(char *const argv[], const char *const env[], const char *cwd);

/* ================= fetch.c: the one place that knows how bytes arrive ================= */
typedef struct { void (*fn)(void *ctx, const char *url, uint64_t done, uint64_t total); void *ctx; } pkg_progress;
/* 0 ok; -1 failed (message printed). Methods by scheme: file:, copy:, http:, https:. */
int pkg_fetch(const char *url, const char *dest, pkg_progress *p);

/* ================= aptconf.c: apt.conf style configuration ================= */
void aptconf_set(const char *key, const char *val);
const char *aptconf_get(const char *key, const char *dflt);
int aptconf_bool(const char *key, int dflt);
int aptconf_parse_file(const char *path);
int aptconf_parse_text(const char *text, const char *origin);
void aptconf_list(const char *prefix, strlist *out);   /* values of KEY::, KEY:: ... lists */
void aptconf_dump(sbuf *out);
char *apt_dir(const char *key, const char *dflt);    /* resolves Dir::... the apt way */

/* ================= repo.c: sources, Release, Packages ================= */
typedef struct {
  char *uri;               /* with trailing slash */
  char *suite;             /* "trixie" or an exact path "./" when it ends in / */
  strlist comps;
  strlist arches;          /* [arch=...] or empty: native + foreign */
  int trusted;             /* [trusted=yes] */
  char *signed_by;
  char *origin;            /* file and line, for messages */
} apt_source;
typedef struct { apt_source *v; int n; } apt_sources;
int sources_load(apt_sources *out);
int sources_parse_line(const char *line, const char *origin, apt_sources *out);
int sources_parse_deb822(const char *text, const char *origin, apt_sources *out);
void sources_free(apt_sources *s);
char *apt_uri_to_filename(const char *uri);  /* apt's lists/ naming: http://a/b/ -> a_b_ */
int apt_update(void);
/* Release file entries */
typedef struct { char *path; uint64_t size; char sha256[65]; } rel_entry;
typedef struct { ctl_para f; rel_entry *e; int n; int signed_clearsign; } rel_file;
int release_parse(const char *data, size_t n, rel_file *out, char **err);
void release_free(rel_file *r);
const rel_entry *release_find(const rel_file *r, const char *path);

/* ================= index.c: compact package index ================= */
typedef struct pkgver pkgver;
typedef struct pkgname pkgname;
/* One version of one package from one index (or the status file). Only what
 * the solver needs stays in memory; the rest of the stanza is re-read from
 * its file by offset when someone asks (apt show, downloads). */
struct pkgver {
  pkgname *pn;
  const char *ver, *arch, *section, *multiarch;
  const char *depends, *predepends, *recommends, *suggests, *conflicts, *breaks, *replaces, *provides, *enhances;
  uint64_t size;             /* .deb size in bytes */
  uint64_t offset;           /* stanza offset in its file */
  uint32_t isize;            /* Installed-Size in KiB */
  uint16_t file_id;          /* index into pkgindex.files; 0 is the dpkg status file */
  uint8_t prio, essential, important, installed;  /* installed: this exact version is in the status file */
  int16_t pin;               /* apt policy priority: 500 from a repository, 100 installed only */
  pkgver *next;              /* other versions of the same name, highest first */
  int solver;                /* solver scratch: variable number */
};
struct pkgname {
  const char *name;
  pkgver *vers;
  pkgver **prov; uint32_t nprov, capprov;  /* versions whose Provides names this */
  pkginfo *inst;            /* dpkg status entry, if any */
  pkgname *next_hash;
  int mark;
};
typedef struct {
  arena a;
  pkgname **tab; uint32_t tabcap, nnames;
  uint32_t nvers, nstanzas;
  strlist files;             /* index files by file_id (0: the status file) */
  strlist file_base;         /* repository base URI for downloads */
  strlist file_label;        /* "URI SUITE/COMP ARCH" for apt policy */
  strmap intern;
  pkgdb *db;
} pkgindex;
int index_load(pkgindex *ix, pkgdb *db);   /* status file + every list the sources name */
int index_add_file(pkgindex *ix, const char *path, const char *base, const char *label);
void index_add_status(pkgindex *ix, pkgdb *db);
pkgname *index_name(pkgindex *ix, const char *name, int create);
void index_finish(pkgindex *ix);           /* builds the provider lists */
pkgver *index_candidate(pkgindex *ix, pkgname *pn);
pkgver *index_installed(pkgname *pn);
void index_free(pkgindex *ix);
int index_stanza(pkgindex *ix, const pkgver *v, ctl_para *out);  /* the full paragraph */
int prio_value(const char *s);
const char *prio_name(int p);
int arch_ok_native(const char *a);         /* native, all, or a foreign architecture */

/* ================= solve.c: dependency resolution ================= */
enum { JOB_INSTALL = 1, JOB_REMOVE, JOB_PURGE, JOB_UPGRADE_ALL, JOB_DIST_UPGRADE, JOB_FIX, JOB_KEEP };
typedef struct { int kind; pkgname *pn; pkgver *ver; int op; const char *verstr; } solve_job;
typedef struct {
  int recommends;            /* install Recommends */
  int allow_remove;          /* may remove installed packages to resolve conflicts */
  int allow_essential_remove;
  int upgrade_all, dist_upgrade, fix_broken, autoremove;
} solve_opts;
typedef struct { pkgver *v; pkgver *old; int action; int auto_inst; } solve_step;  /* action: 1 install/upgrade, 2 remove, 3 purge */
typedef struct {
  solve_step *steps; int n;
  sbuf why;                  /* explanation when it fails */
  double ms;                 /* time spent */
  uint64_t dl_bytes, inst_kib, freed_kib;
  int n_install, n_upgrade, n_remove, n_downgrade;
  int vars, clauses, conflicts, decisions;
} solve_result;
int solve(pkgindex *ix, pkgdb *db, const solve_job *jobs, int njobs, const solve_opts *o, solve_result *r);
void solve_result_free(solve_result *r);
/* Order for dpkg: unpack/configure steps with Pre-Depends first. */
typedef struct { pkgver *v; int op; } order_step;   /* op: 'u' unpack, 'c' configure, 'r' remove, 'p' purge */
int solve_order(pkgindex *ix, solve_result *r, order_step **out, int *n, sbuf *notes);
/* Which candidate satisfies a dependency atom: shared by solver and apt depends. */
int atom_matches(const dep_atom *a, const pkgver *v, const char *dependent_arch, int via_provides_ok);
int dep_candidates(pkgindex *ix, const dep_atom *a, const char *dependent_arch, pkgver ***out);

/* ================= apt.c: auto-installed marks ================= */
int extstates_load(strmap *auto_marks);
int extstates_save(const strmap *auto_marks);


/* ================= front ends (cli_*.c) ================= */
int dpkg_main(int argc, char **argv, int as_query);
int dpkg_deb_main(int argc, char **argv);
int apt_main(int argc, char **argv, const char *personality);
#endif
