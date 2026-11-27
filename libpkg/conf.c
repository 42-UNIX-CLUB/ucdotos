// SPDX-License-Identifier: GPL-2.0-or-later
/* dpkg configuration: built-in defaults, then CONFDIR/dpkg.cfg.d/ * and
 * CONFDIR/dpkg.cfg (one long option per line, without the dashes), then the
 * environment (DPKG_ROOT, DPKG_ADMINDIR), then the command line.
 *
 * The native architecture is decided here and nowhere else: the compile-time
 * default PKG_NATIVE_ARCH, which the "native-architecture" option in dpkg.cfg
 * can change. Everything else asks pkg_cfg.arch. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#ifndef PKG_NATIVE_ARCH
#define PKG_NATIVE_ARCH "riscv64"   /* the guest architecture of the Linux binary engine (bin/lx): Debian 13 riscv64 */
#endif
#ifndef PKG_CONFDIR
#define PKG_CONFDIR "/etc/dpkg"
#endif

pkg_config pkg_cfg;
static int root_given;
strlist pkg_path_filters;  /* "+glob" include, "-glob" exclude, in order */

static const struct { const char *name; unsigned bit; const char *help; } forces[] = {
  {"depends", F_DEPENDS, "turn dependency problems into warnings"},
  {"depends-version", F_DEPENDS_VERSION, "turn version dependency problems into warnings"},
  {"conflicts", F_CONFLICTS, "allow installing conflicting packages"},
  {"breaks", F_BREAKS, "install even if it would break another package"},
  {"overwrite", F_OVERWRITE, "overwrite a file owned by another package"},
  {"overwrite-dir", F_OVERWRITE_DIR, "overwrite a directory with a file"},
  {"confold", F_CONFOLD, "keep the old version of a changed conffile"},
  {"confnew", F_CONFNEW, "install the new version of a changed conffile"},
  {"confdef", F_CONFDEF, "use the default choice for changed conffiles"},
  {"confmiss", F_CONFMISS, "install conffiles the user deleted"},
  {"confask", F_CONFFILE_ASK, "offer to replace conffiles that were not changed"},
  {"remove-essential", F_REMOVE_ESSENTIAL, "remove an essential package"},
  {"remove-reinstreq", F_REMOVE_REINSTREQ, "remove a package that needs reinstalling"},
  {"downgrade", F_DOWNGRADE, "replace a package with a lower version (on by default)"},
  {"architecture", F_ARCHITECTURE, "install a package for an unknown architecture"},
  {"bad-version", F_BAD_VERSION, "accept badly formed versions"},
  {"bad-path", F_BAD_PATH, "accept a PATH without the usual programs"},
  {"not-root", F_NOT_ROOT, "do not try to change owners"},
  {"hold", F_HOLD, "process packages that are on hold"},
  {"script-chrootless", F_SCRIPT_CHROOTLESS, "run maintainer scripts with this system's shell, without a changed root"},
  {"unsafe-io", F_UNSAFE_IO, "do not fsync"},
  {"unrunnable-scripts", F_UNRUNNABLE_SCRIPTS, "treat a maintainer script that cannot run here as done (UC.OS)"},
};

const char *pkg_force_help(void) {
  static sbuf b;
  if (b.n) return b.s;
  for (size_t i = 0; i < sizeof forces / sizeof *forces; i++) sb_printf(&b, "  %-20s %s\n", forces[i].name, forces[i].help);
  sb_puts(&b, "  all                  all of the above (dangerous)\n");
  return b.s;
}
int pkg_force_parse(const char *list, int on) {
  char *copy = xstrdup(list), *save = 0;
  int ok = 1;
  for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(0, ",", &save)) {
    unsigned bit = 0;
    if (!strcmp(t, "all")) bit = ~(F_CONFOLD | F_CONFNEW | F_CONFDEF | F_CONFFILE_ASK | F_UNRUNNABLE_SCRIPTS | F_SCRIPT_CHROOTLESS);
    else if (!strcmp(t, "help")) { fputs(pkg_force_help(), stdout); exit(0); }
    else for (size_t i = 0; i < sizeof forces / sizeof *forces; i++) if (!strcmp(forces[i].name, t)) bit = forces[i].bit;
    if (!bit) { pkg_error("unknown force/refuse option '%s'", t); ok = 0; continue; }
    if (on) pkg_cfg.force |= bit; else pkg_cfg.force &= ~bit;
  }
  free(copy);
  return ok ? 1 : -1;
}

static void set(char **slot, const char *v) { free(*slot); *slot = xstrdup(v); }

int pkg_config_option(const char *opt, const char *val) {
  while (*opt == '-') opt++;
  if (!strncmp(opt, "force-", 6)) return pkg_force_parse(opt + 6, 1);
  if (!strncmp(opt, "refuse-", 7)) return pkg_force_parse(opt + 7, 0);
  if (!strncmp(opt, "no-force-", 9)) return pkg_force_parse(opt + 9, 0);
  if (!strcmp(opt, "root")) { if (!val) return -1; set(&pkg_cfg.root, val); free(pkg_cfg.admindir); pkg_cfg.admindir = 0; root_given = 1; return 1; }
  if (!strcmp(opt, "instdir")) { if (!val) return -1; set(&pkg_cfg.root, val); root_given = 1; return 1; }
  if (!strcmp(opt, "admindir")) { if (!val) return -1; set(&pkg_cfg.admindir, val); return 1; }
  if (!strcmp(opt, "native-architecture")) { if (!val) return -1; set(&pkg_cfg.arch, val); return 1; }
  if (!strcmp(opt, "script-runner")) { if (!val) return -1; set(&pkg_cfg.script_runner, val); return 1; }
  if (!strcmp(opt, "script-shell")) { if (!val) return -1; set(&pkg_cfg.script_shell, val); return 1; }
  if (!strcmp(opt, "log")) { if (!val) return -1; set(&pkg_cfg.logfile, val); return 1; }
  if (!strcmp(opt, "no-act") || !strcmp(opt, "dry-run") || !strcmp(opt, "simulate")) { pkg_cfg.no_act = 1; return 1; }
  if (!strcmp(opt, "no-triggers")) { pkg_cfg.no_triggers = 1; return 1; }
  if (!strcmp(opt, "path-exclude") || !strcmp(opt, "path-include")) {
    if (!val) return -1;
    char *s = xasprintf("%c%s", opt[5] == 'e' ? '-' : '+', val);
    sl_add(&pkg_path_filters, s);
    free(s);
    return 1;
  }
  /* accepted and ignored: they change nothing here */
  static const char *const ignored[] = {"no-debsig", "no-pager", "status-logger", "status-fd", "abort-after", "debug", "skip-same-version", 0};
  for (int i = 0; ignored[i]; i++) if (!strcmp(opt, ignored[i])) return 1;
  return 0;
}

static void read_cfg_file(const char *path) {
  char *data;
  if (file_read(path, &data, 0) < 0) return;
  int lineno = 0;
  for (char *line = data, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    lineno++;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') continue;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) line[--n] = 0;
    char *v = line + strcspn(line, " \t=");
    if (*v) { *v++ = 0; while (*v == ' ' || *v == '\t' || *v == '=') v++; }
    if (pkg_config_option(line, *v ? v : 0) == 0) pkg_warn("%s:%d: unknown option '%s'", path, lineno, line);
  }
  free(data);
}

void pkg_config_init(void) {
  memset(&pkg_cfg, 0, sizeof pkg_cfg);
  pkg_cfg.arch = xstrdup(PKG_NATIVE_ARCH);
  pkg_cfg.force = F_DOWNGRADE;
  pkg_cfg.script_shell = xstrdup("/bin/sh");
  const char *cd = getenv("UCOS_PKG_CONFDIR");  /* tests point this at a scratch directory */
  pkg_cfg.confdir = xstrdup(cd ? cd : PKG_CONFDIR);
  char *dir = path_join(pkg_cfg.confdir, "dpkg.cfg.d");
  DIR *d = opendir(dir);
  if (d) {
    strlist names = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
      const char *n = e->d_name;
      if (n[0] == '.' || strspn(n, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(n)) continue;
      sl_add(&names, n);
    }
    closedir(d);
    sl_sort(&names);
    for (int i = 0; i < names.n; i++) { char *p = path_join(dir, names.v[i]); read_cfg_file(p); free(p); }
    sl_free(&names);
  }
  free(dir);
  char *cfg = path_join(pkg_cfg.confdir, "dpkg.cfg");
  read_cfg_file(cfg);
  free(cfg);
  const char *e;
  if ((e = getenv("DPKG_ROOT")) && *e) { set(&pkg_cfg.root, e); root_given = 1; free(pkg_cfg.admindir); pkg_cfg.admindir = 0; }
  if ((e = getenv("DPKG_ADMINDIR")) && *e) set(&pkg_cfg.admindir, e);
}

void pkg_config_finish(void) {
  if (!pkg_cfg.root) {
#ifdef __wasm__
    pkg_cfg.root = xstrdup("/");
#else
    /* On a development machine never touch the host's own system by accident. */
    pkg_error("no --root given and DPKG_ROOT is not set; this host build refuses to use '/'");
    exit(2);
#endif
  }
  (void)root_given;
  if (!pkg_cfg.admindir) pkg_cfg.admindir = path_join(pkg_cfg.root, "var/lib/dpkg");
  char *af = path_join(pkg_cfg.admindir, "arch");
  char *data;
  if (file_read(af, &data, 0) == 0) {
    for (char *t = strtok(data, " \t\n"); t; t = strtok(0, " \t\n"))
      if (strcmp(t, pkg_cfg.arch) && sl_find(&pkg_cfg.foreign, t) < 0) sl_add(&pkg_cfg.foreign, t);
    free(data);
  }
  free(af);
}

char *root_path(const char *p) { return path_join(pkg_cfg.root, p); }
int arch_is_known(const char *a) {
  return !a || !strcmp(a, "all") || !strcmp(a, pkg_cfg.arch) || sl_find(&pkg_cfg.foreign, a) >= 0;
}
