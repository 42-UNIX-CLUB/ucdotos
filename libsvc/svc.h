// SPDX-License-Identifier: GPL-2.0-or-later
/* libsvc: the parts of the service manager that are plain C and shared by the manager,
 * systemctl and the Debian helpers. It reads systemd unit files (the format described in
 * systemd.syntax(7), systemd.unit(5), systemd.service(5)); it does not run anything.
 * It builds with clang for UC.OS and with the host cc (tests: libsvc/tests/). */
#ifndef SVC_H
#define SVC_H
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
/* Linux values for the few error numbers the UC.OS headers do not define yet */
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef EBUSY
#define EBUSY 16
#endif

/* ---------- unit files ---------- */
struct svc_kv { char *sec, *key, *val; const char *src; int line; };
struct svc_file {
  struct svc_kv *kv; int n, cap;
  char **srcs; int nsrc;            /* file names, owned; svc_kv.src points into them */
};
typedef void (*svc_warn_fn)(void *ctx, const char *src, int line, const char *msg);
/* Append the entries of one file (a fragment or a drop-in). Returns the number of
 * problems found (each reported through warn); entries are still added. */
int svc_parse(struct svc_file *f, const char *text, size_t len, const char *src, svc_warn_fn warn, void *ctx);
int svc_parse_path(struct svc_file *f, const char *path, svc_warn_fn warn, void *ctx); /* -1: cannot read */
void svc_file_free(struct svc_file *f);
/* The value in force: the last assignment. NULL if the key never appears. */
const char *svc_get(const struct svc_file *f, const char *sec, const char *key);
/* Every value of a list key, honouring resets (an empty assignment clears what came
 * before). Returns the count; fills out[] up to max. */
int svc_getall(const struct svc_file *f, const char *sec, const char *key, const char **out, int max);
int svc_has_section(const struct svc_file *f, const char *sec);

/* ---------- values ---------- */
int svc_bool(const char *s, int dflt);               /* yes/no/true/false/on/off/1/0 */
#define SVC_EMASKED 132 /* returned when a unit is masked (Linux ERFKILL, unused here otherwise) */
#define SVC_INFINITY ((int64_t)INT64_MAX)
int svc_timespan(const char *s, int64_t *ms, int64_t default_unit_ms); /* 0 ok, -1 bad */
/* Split a value into words (whitespace, quotes and C escapes, as systemd's word
 * splitting). Returns the count, -1 on bad quoting. Words are malloc'ed. */
int svc_words(const char *s, char ***out);
void svc_free_words(char **w, int n);

/* ---------- command lines (ExecStart= and friends) ---------- */
#define SVC_EX_IGNORE 1  /* "-": a failure is not an error */
#define SVC_EX_ARGV0  2  /* "@": the second word is argv[0] */
#define SVC_EX_NOENV  4  /* ":": no variable expansion */
#define SVC_EX_PRIV   8  /* "+" "!" "!!": privileges; meaningless here, accepted */
struct svc_cmd { int flags; char *path; char **argv; int argc; };
typedef const char *(*svc_env_fn)(void *ctx, const char *name);
/* Parse one ExecXxx= value into commands (a word ";" separates commands). Variables
 * are expanded with env (may be NULL). Returns the count, -1 on a syntax error. */
int svc_cmdline(const char *val, svc_env_fn env, void *ctx, struct svc_cmd **out, char *err, size_t errlen);
void svc_cmds_free(struct svc_cmd *c, int n);

/* ---------- calendar events (OnCalendar=) ---------- */
struct svc_cal {
  uint8_t wday;          /* bit 0 = Monday .. bit 6 = Sunday */
  uint16_t mon;          /* bit 1..12 */
  uint32_t mday;         /* bit 1..31 */
  uint32_t hour;         /* bit 0..23 */
  uint64_t min, sec;     /* bit 0..59 */
  int nyear; struct { int a, b, step; } year[4]; /* nyear 0 = any year */
};
int svc_cal_parse(const char *s, struct svc_cal *c);           /* 0 ok, -1 bad */
int64_t svc_cal_next(const struct svc_cal *c, int64_t after_s); /* first match > after (UTC seconds), -1 none */
void svc_civil(int64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd); /* wd 0 = Monday */

/* ---------- names ---------- */
enum { SVC_T_SERVICE, SVC_T_SOCKET, SVC_T_TARGET, SVC_T_TIMER, SVC_T_PATH, SVC_T_SLICE,
       SVC_T_MOUNT, SVC_T_AUTOMOUNT, SVC_T_DEVICE, SVC_T_SWAP, SVC_T_SCOPE, SVC_T_UNKNOWN };
int svc_name_type(const char *name);
const char *svc_type_name(int t);
int svc_name_valid(const char *name);
/* "foo@bar.service" -> template "foo@.service", instance "bar". Returns 1 for an
 * instance, 2 for a bare template, 0 otherwise. */
int svc_name_split(const char *name, char *tmpl, size_t tl, char *inst, size_t il);
void svc_unescape(const char *in, char *out, size_t outlen);   /* "-" -> "/", "\xNN" */
/* Expand specifiers (%n %N %p %P %i %I %f %j %J %t %S %C %L %E %h %u %U %H %v %%).
 * Returns a malloc'ed string; *bad counts unknown specifiers. */
struct svc_spec { const char *name; const char *home, *user, *host, *release; int uid; };
char *svc_specifiers(const char *in, const struct svc_spec *sp, int *bad);

/* ---------- links ----------
 * Real systemd enables a unit by making symlinks. Until the kernel has symlinks, a
 * "link file" stands in: a regular file whose whole content is one absolute path and
 * a newline. svc_readlink reads either; svc_makelink makes a symlink when the kernel
 * can, else a link file. */
int svc_readlink(const char *path, char *buf, size_t len); /* 1 link, 0 not a link, -1 missing */
int svc_makelink(const char *target, const char *path);    /* 0 ok, -errno */
int svc_have_symlinks(void);
int svc_mkdir_p(const char *path);

/* ---------- finding units ----------
 * A root is a directory tree that holds units: "" is UC.OS's own; a Debian root
 * (default /debian) holds guest units, whose paths are guest paths. */
#define SVC_MAXROOTS 2
struct svc_roots { int n; char root[SVC_MAXROOTS][128]; char extra[128]; };
extern const char *const svc_unit_dirs[]; /* relative to a root, NULL-terminated, highest priority first */
/* The k-th unit directory of root ri as a host path, or 0 past the end. "extra" (a
 * directory searched first, like $SYSTEMD_UNIT_PATH) belongs to root 0. */
const char *svc_unit_dir(const struct svc_roots *r, int ri, int k, char *buf, size_t bl);
/* Find the fragment for a unit name. On success fills path (host path), root index,
 * and the name of the unit the fragment really defines (differs for an alias or a
 * template instance). Returns 0, -ENOENT, or 1 if the unit is masked. */
int svc_find(const struct svc_roots *r, const char *name, char *path, size_t pl, int *root, char *real, size_t rl);
/* Load a unit: fragment, then drop-ins (NAME.d/ *.conf in every directory, and for an
 * instance the template's TEMPLATE.d/ as well), in systemd's order. */
int svc_load(const struct svc_roots *r, const char *name, struct svc_file *f, char *path, size_t pl, int *root,
             svc_warn_fn warn, void *ctx);
/* Names listed in NAME.wants/ (kind "wants") or NAME.requires/ in every unit directory. */
int svc_dirdeps(const struct svc_roots *r, const char *name, const char *kind, char ***out);
/* Turn a path from a guest unit into a host path ("/usr/sbin/x" -> "/debian/usr/sbin/x").
 * Root 0 is UC.OS's own: its paths are used as they are (a prefix on root 0 only moves
 * where unit files are looked for, which the native tests use). */
void svc_host_path(const struct svc_roots *r, int root, const char *p, char *out, size_t ol);
/* Read the Debian root from /etc/svc.conf style text or default; fills r. */
void svc_roots_default(struct svc_roots *r, const char *debian_root);

/* ---------- enabling (shared by systemctl and deb-systemd-helper) ---------- */
struct svc_change { char path[256]; char target[256]; int removed; };
/* Apply [Install] of a unit: make (enable) or compute (dry) the links. Returns the
 * number of changes written to ch (up to max), or -errno. */
int svc_enable(const struct svc_roots *r, const char *name, int dry, struct svc_change *ch, int max, char *err, size_t el);
int svc_disable(const struct svc_roots *r, const char *name, struct svc_change *ch, int max);
/* "enabled" "disabled" "static" "masked" "alias" "indirect" "generated" or NULL (not found) */
const char *svc_is_enabled(const struct svc_roots *r, const char *name);
int svc_mask(const struct svc_roots *r, const char *name, int unmask);
#endif
