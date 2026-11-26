// SPDX-License-Identifier: GPL-2.0-or-later
/* deb-systemd-helper: what Debian's maintainer scripts call to enable, disable, mask
 * and remember the state of unit files (deb-systemd-helper(1p)). Written from the
 * manual page and from what the generated maintainer scripts expect, for UC.OS:
 *
 *   enable        make the links of [Install] and record them in the state file
 *   disable       remove the links; the state file stays (the admin's choice is kept)
 *   reenable      disable, then enable
 *   purge         remove the links, the state file and any mask this tool made
 *   mask          link /etc/systemd/system/UNIT to /dev/null, unless a file is there
 *   unmask        remove a mask this tool made
 *   is-enabled    0 if enabled
 *   was-enabled   0 if there is no state yet (a new install), or if every link the
 *                 state file lists still exists
 *   debian-installed  0 if the state file of at least one unit exists
 *   update-state  rewrite the state file with the links enable would make, make none
 *
 * State: ROOT/var/lib/systemd/deb-systemd-helper-enabled/UNIT.dsh-also (one link path
 * per line) and ROOT/var/lib/systemd/deb-systemd-helper-masked/UNIT, in the root the
 * unit file was found in. Options: --quiet, --user (refused). DPKG_ROOT: work on that
 * tree instead of /. _DEB_SYSTEMD_HELPER_DEBUG=1: say what is done on stderr. */
#include "../../libsvc/svc.h"
#include "../../libsvc/os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static struct svc_roots roots;
static int quiet, debug;

static void state_path(int ri, const char *unit, const char *kind, char *out, size_t ol) {
  snprintf(out, ol, "%s/var/lib/systemd/deb-systemd-helper-%s/%s%s", roots.root[ri], kind, unit, !strcmp(kind, "enabled") ? ".dsh-also" : "");
}
static int unit_root(const char *unit) {
  char p[512];
  int ri = 0;
  if (svc_find(&roots, unit, p, sizeof p, &ri, 0, 0) < 0) return 0;
  return ri;
}
static int write_state(int ri, const char *unit, struct svc_change *ch, int n) {
  char sp[600];
  state_path(ri, unit, "enabled", sp, sizeof sp);
  char dir[600];
  snprintf(dir, sizeof dir, "%s", sp);
  *strrchr(dir, '/') = 0;
  svc_mkdir_p(dir);
  int fd = open(sp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -errno;
  for (int i = 0; i < n; i++) {
    const char *p = ch[i].path + strlen(roots.root[ri]); /* as the root sees it */
    if (write(fd, p, strlen(p)) < 0 || write(fd, "\n", 1) < 0) {}
  }
  close(fd);
  if (debug) fprintf(stderr, "deb-systemd-helper: wrote %s (%d links)\n", sp, n);
  return 0;
}

static int cmd(const char *verb, const char *unit) {
  char sp[600], err[300];
  struct svc_change ch[64];
  int ri = unit_root(unit);
  if (!strcmp(verb, "enable") || !strcmp(verb, "reenable")) {
    if (!strcmp(verb, "reenable")) svc_disable(&roots, unit, ch, 64);
    int n = svc_enable(&roots, unit, 0, ch, 64, err, sizeof err);
    if (n < 0) { if (!quiet) fprintf(stderr, "deb-systemd-helper: %s\n", err); return 1; }
    for (int i = 0; i < n && i < 64 && debug; i++) fprintf(stderr, "deb-systemd-helper: link %s -> %s\n", ch[i].path, ch[i].target);
    write_state(ri, unit, ch, n < 64 ? n : 64);
    return 0;
  }
  if (!strcmp(verb, "update-state")) {
    int n = svc_enable(&roots, unit, 1, ch, 64, err, sizeof err);
    if (n < 0) return 1;
    write_state(ri, unit, ch, n < 64 ? n : 64);
    return 0;
  }
  if (!strcmp(verb, "disable")) { svc_disable(&roots, unit, ch, 64); return 0; }
  if (!strcmp(verb, "purge")) {
    svc_disable(&roots, unit, ch, 64);
    state_path(ri, unit, "enabled", sp, sizeof sp);
    unlink(sp);
    state_path(ri, unit, "masked", sp, sizeof sp);
    if (unlink(sp) == 0) svc_mask(&roots, unit, 1);
    return 0;
  }
  if (!strcmp(verb, "mask")) {
    char p[600], t[300];
    snprintf(p, sizeof p, "%s/etc/systemd/system/%s", roots.root[ri], unit);
    if (svc_readlink(p, t, sizeof t) >= 0) return 0; /* something is there already: leave it */
    if (svc_mask(&roots, unit, 0) < 0) return 1;
    state_path(ri, unit, "masked", sp, sizeof sp);
    char dir[600];
    snprintf(dir, sizeof dir, "%s", sp);
    *strrchr(dir, '/') = 0;
    svc_mkdir_p(dir);
    int fd = open(sp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
    return 0;
  }
  if (!strcmp(verb, "unmask")) {
    state_path(ri, unit, "masked", sp, sizeof sp);
    struct stat st;
    if (stat(sp, &st) < 0) return 0; /* not our mask */
    svc_mask(&roots, unit, 1);
    unlink(sp);
    return 0;
  }
  if (!strcmp(verb, "is-enabled")) {
    const char *s = svc_is_enabled(&roots, unit);
    if (!quiet) printf("%s\n", s ? s : "disabled");
    return s && !strcmp(s, "enabled") ? 0 : 1;
  }
  if (!strcmp(verb, "was-enabled")) {
    state_path(ri, unit, "enabled", sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return 0; /* no state: a new install, which enable will handle */
    char line[512];
    int all = 1, any = 0;
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\n")] = 0;
      if (!line[0]) continue;
      any = 1;
      char p[700], t[300];
      snprintf(p, sizeof p, "%s%s", roots.root[ri], line);
      if (svc_readlink(p, t, sizeof t) < 0) all = 0;
    }
    fclose(f);
    if (!quiet) printf("%s\n", all && any ? "enabled" : "disabled");
    return all && any ? 0 : 1;
  }
  if (!strcmp(verb, "debian-installed")) {
    state_path(ri, unit, "enabled", sp, sizeof sp);
    struct stat st;
    return stat(sp, &st) == 0 ? 0 : 1;
  }
  fprintf(stderr, "deb-systemd-helper: unknown action %s\n", verb);
  return 2;
}

int main(int argc, char **argv) {
  const char *verb = 0;
  const char *units[64];
  int nu = 0;
  debug = getenv("_DEB_SYSTEMD_HELPER_DEBUG") && !strcmp(getenv("_DEB_SYSTEMD_HELPER_DEBUG"), "1");
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--quiet")) quiet = 1;
    else if (!strcmp(argv[i], "--user")) { fprintf(stderr, "deb-systemd-helper: --user is not supported on UC.OS (one user, one manager)\n"); return 0; }
    else if (argv[i][0] == '-') {}
    else if (!verb) verb = argv[i];
    else if (nu < 64) units[nu++] = argv[i];
  }
  if (!verb || !nu) { fprintf(stderr, "usage: deb-systemd-helper enable|disable|purge|mask|unmask|is-enabled|was-enabled|debian-installed|update-state|reenable UNIT...\n"); return 2; }
  svc_roots_default(&roots, getenv("SVC_GUEST_ROOT") ? getenv("SVC_GUEST_ROOT") : "/debian");
  const char *dr = getenv("DPKG_ROOT");
  if (dr && *dr) { snprintf(roots.root[0], sizeof roots.root[0], "%s", dr); roots.n = 1; }
  if (getenv("SVC_ROOT")) snprintf(roots.root[0], sizeof roots.root[0], "%s", getenv("SVC_ROOT"));
  int rc = 0;
  int any_installed = 0;
  for (int i = 0; i < nu; i++) {
    int r = cmd(verb, units[i]);
    if (!strcmp(verb, "debian-installed")) { if (r == 0) any_installed = 1; continue; }
    if (!strcmp(verb, "is-enabled") || !strcmp(verb, "was-enabled")) { if (r) rc = 1; continue; }
    if (r) rc = r;
  }
  if (!strcmp(verb, "debian-installed")) return any_installed ? 0 : 1;
  return rc;
}
