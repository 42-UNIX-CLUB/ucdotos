// SPDX-License-Identifier: GPL-2.0-or-later
/* Maintainer scripts (Debian Policy chapter 6). A script runs with the
 * arguments Policy gives and the environment dpkg sets (DPKG_MAINTSCRIPT_*,
 * DPKG_ROOT, DPKG_ADMINDIR). Where it runs depends on the root:
 *  - root "/": the script runs here, through its #! interpreter.
 *  - another root: dpkg would chroot into it. Here that takes a program that
 *    can run the root's own binaries (the "script-runner" option, meant for the
 *    binary engine). Without one, --force-script-chrootless runs the script
 *    with this system's shell and DPKG_ROOT set, as dpkg does with that option.
 *  - otherwise the script cannot run: script_run returns -1 and says why, and
 *    the caller decides (fail, or carry on with --force-unrunnable-scripts). */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef __wasm__
#include <spawn.h>
#endif

extern char **environ;

int run_command(char *const argv[], const char *const env[], const char *cwd) {
  /* environment: ours, with env[] entries replacing same-named ones */
  int n = 0, m = 0;
  while (environ && environ[n]) n++;
  while (env && env[m]) m++;
  char **ev = xcalloc(n + m + 1, sizeof *ev);
  int k = 0;
  for (int i = 0; i < n; i++) {
    const char *eq = strchr(environ[i], '=');
    size_t nl = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
    int replaced = 0;
    for (int j = 0; j < m; j++) if (!strncmp(env[j], environ[i], nl) && env[j][nl] == '=') replaced = 1;
    if (!replaced) ev[k++] = environ[i];
  }
  for (int j = 0; j < m; j++) ev[k++] = (char *)env[j];
  ev[k] = 0;
  fflush(stdout); fflush(stderr);
  /* a bare program name is looked up in PATH (ours, or the one in env[]) */
  char *found = 0;
  if (!strchr(argv[0], '/')) {
    const char *path = 0;
    for (int j = 0; j < k; j++) if (!strncmp(ev[j], "PATH=", 5)) path = ev[j] + 5;
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *s = path; *s && !found;) {
      size_t l = strcspn(s, ":");
      char *cand = xasprintf("%.*s/%s", (int)l, s, argv[0]);
      if (access(cand, X_OK) == 0) found = cand; else free(cand);
      s += l + (s[l] == ':');
    }
    if (!found) { free(ev); errno = ENOENT; return -1; }
  }
  const char *prog = found ? found : argv[0];
  pid_t pid;
#ifdef __wasm__
  (void)cwd;
  pid = spawn(prog, argv, ev, 0);
  if (pid < 0) { free(ev); free(found); return -1; }
#else
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
  int e = posix_spawn(&pid, prog, &fa, 0, argv, ev);
  posix_spawn_file_actions_destroy(&fa);
  if (e) { free(ev); free(found); errno = e; return -1; }
#endif
  free(ev);
  free(found);
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 128 + WTERMSIG(st);
}

/* What kind of program is at path: 0 missing, 1 script, 2 runnable binary here, 3 foreign binary. */
static int prog_kind(const char *path, char *interp, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  char buf[256];
  long n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n < 0) return 0;
  buf[n] = 0;
  if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
    char *s = buf + 2;
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strcspn(s, " \t\n");
    snprintf(interp, cap, "%.*s", (int)l, s);
    return 1;
  }
#ifdef __wasm__
  if (n >= 4 && !memcmp(buf, "\0asm", 4)) return 2;
  return 3;   /* ELF and anything else needs the binary engine */
#else
  return access(path, X_OK) == 0 ? 2 : 3;
#endif
}

int script_run(const pkginfo *p, const char *path, const char *name, const char *const args[], char **why) {
  *why = 0;
  char *own = 0;
  if (!path) path = own = pi_infoname(p, name);
  if (!path_exists(path)) { free(own); return 0; }
  if (pkg_cfg.no_act) { free(own); return 0; }
  char interp[256] = "";
  int kind = prog_kind(path, interp, sizeof interp);
  int is_root = !strcmp(pkg_cfg.root, "/");
  strlist argv = {0};
  const char *mode = 0;
  if (is_root) {
    if (kind == 1) {
      char ik[256];
      int k = prog_kind(interp, ik, sizeof ik);
      if (k == 2 || (k == 1 && !strcmp(interp, "/bin/sh"))) { sl_add(&argv, interp); sl_add(&argv, path); }
      else if (k == 0) *why = xasprintf("its interpreter %s is not installed", interp);
      else if (pkg_cfg.script_runner) mode = "runner";
      else *why = xasprintf("its interpreter %s is a program this system cannot run yet", interp);
    } else if (kind == 2) sl_add(&argv, path);
    else if (pkg_cfg.script_runner) mode = "runner";
    else *why = xstrdup("it is a program this system cannot run yet");
  } else if (pkg_cfg.script_runner) mode = "runner";
  else if (pkg_cfg.force & F_SCRIPT_CHROOTLESS) {
    if (kind == 1 && (!strcmp(interp, "/bin/sh") || !strcmp(interp, "/bin/dash"))) { sl_add(&argv, pkg_cfg.script_shell); sl_add(&argv, path); }
    else if (kind == 1 && prog_kind(interp, (char[256]){0}, 256) == 2) { sl_add(&argv, interp); sl_add(&argv, path); }
    else *why = xasprintf("it needs %s, which this system cannot run", kind == 1 ? interp : "a foreign binary");
  } else
    *why = xasprintf("it must run inside %s, which needs the binary engine (or --force-script-chrootless)", pkg_cfg.root);
  if (mode) {
    /* runner words, with %r replaced by the root; then the script as seen from inside the root */
    char *rw = xstrdup(pkg_cfg.script_runner), *save = 0;
    for (char *t = strtok_r(rw, " ", &save); t; t = strtok_r(0, " ", &save)) sl_add(&argv, !strcmp(t, "%r") ? pkg_cfg.root : t);
    free(rw);
    size_t rl = strlen(pkg_cfg.root);
    const char *inside = (!is_root && !strncmp(path, pkg_cfg.root, rl)) ? path + rl : path;
    if (kind == 1) sl_add(&argv, interp);
    sl_add(&argv, inside);
  }
  if (*why) { sl_free(&argv); free(own); return -1; }
  for (int i = 0; args && args[i]; i++) sl_add(&argv, args[i]);
  sl_add(&argv, "");
  free(argv.v[argv.n - 1]);
  argv.v[argv.n - 1] = 0;

  char ka[300];
  pi_key(p, ka, sizeof ka);
  const char *arch = p->arch ? p->arch : pkg_cfg.arch;
  char *e_pkg = xasprintf("DPKG_MAINTSCRIPT_PACKAGE=%s", p->name);
  char *e_arch = xasprintf("DPKG_MAINTSCRIPT_ARCH=%s", arch);
  char *e_name = xasprintf("DPKG_MAINTSCRIPT_NAME=%s", name);
  char *e_root = xasprintf("DPKG_ROOT=%s", (is_root || mode) ? "" : pkg_cfg.root);
  char *e_admin = xasprintf("DPKG_ADMINDIR=%s", mode ? "/var/lib/dpkg" : pkg_cfg.admindir);
  const char *env[] = {e_pkg, "DPKG_MAINTSCRIPT_PACKAGE_REFCOUNT=1", e_arch, e_name, "DPKG_MAINTSCRIPT_DEBUG=0",
                       "DPKG_RUNNING_VERSION=1.22.0-ucos", e_root, e_admin,
                       "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0};
  if (pkg_verbose) {
    sbuf b = {0};
    for (int i = 0; argv.v[i]; i++) sb_printf(&b, "%s%s", i ? " " : "", argv.v[i]);
    pkg_msg("running: %s\n", b.s);
    sb_free(&b);
  }
  int rc = run_command(argv.v, env, is_root ? "/" : pkg_cfg.root);
  if (rc < 0) { *why = xasprintf("cannot start %s: %s", argv.v[0], strerror(errno)); }
  free(e_pkg); free(e_arch); free(e_name); free(e_root); free(e_admin);
  argv.n--;  /* the NULL slot */
  sl_free(&argv);
  free(own);
  return rc;
}
