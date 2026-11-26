// SPDX-License-Identifier: GPL-2.0-or-later
/* deb-systemd-invoke: Debian's wrapper around systemctl for maintainer scripts
 * (deb-systemd-invoke(1p)): asks /usr/sbin/policy-rc.d first, and does not start a
 * unit the admin disabled. For UC.OS:
 *   - with no manager running (no /run/systemd/system) it does nothing and succeeds;
 *   - policy-rc.d (native root, then the Debian root) is run as "policy-rc.d UNIT
 *     ACTION"; exit status 101 means the action is not allowed and the unit is skipped;
 *   - start skips units that are not enabled (is-enabled other than enabled, static,
 *     indirect, alias); restart skips units that are neither enabled nor active;
 *   - stop always goes through;
 *   - daemon-reload and daemon-reexec are passed on.
 * The behaviour beyond the manual page's description is this program's reading of
 * what the Debian tool does; docs/SERVICES.md lists it as such. */
#include "../../libsvc/svc.h"
#include "../../libsvc/os.h"
#include "../../libsvc/proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int run(char *const argv[]) {
  extern char **environ;
  int fdmap[3] = {-1, -1, -1};
  int pid = svc_spawn(argv[0], argv, environ, fdmap, 0, 0, -1);
  if (pid < 0) return 127;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (st & 0x7f);
}
static const char *find_systemctl(void) {
  static const char *const c[] = {"/bin/systemctl", "/usr/bin/systemctl", 0};
  struct stat st;
  for (int i = 0; c[i]; i++) if (stat(c[i], &st) == 0) return c[i];
  return "/bin/systemctl";
}
static int policy_allows(const char *unit, const char *action) {
  static const char *const where[] = {"/usr/sbin/policy-rc.d", "/debian/usr/sbin/policy-rc.d", 0};
  struct stat st;
  for (int i = 0; where[i]; i++) {
    if (stat(where[i], &st) < 0) continue;
    char head[4] = {0};
    FILE *f = fopen(where[i], "r");
    if (f) { if (fread(head, 1, 4, f)) {} fclose(f); }
    int r;
    if (!memcmp(head, "\0asm", 4)) r = run((char *[]){(char *)where[i], (char *)unit, (char *)action, 0});
    else if (i == 1 || !memcmp(head, "\177ELF", 4)) r = run((char *[]){"/bin/lx", "--root=/debian", "--", "/usr/sbin/policy-rc.d", (char *)unit, (char *)action, 0});
    else r = run((char *[]){"/bin/sh", (char *)where[i], (char *)unit, (char *)action, 0});
    return r != 101;
  }
  return 1;
}

int main(int argc, char **argv) {
  const char *verb = 0;
  char *units[64];
  int nu = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--user")) { fprintf(stderr, "deb-systemd-invoke: --user is not supported on UC.OS\n"); return 0; }
    if (argv[i][0] == '-') continue;
    if (!verb) verb = argv[i];
    else if (nu < 64) units[nu++] = argv[i];
  }
  if (!verb) { fprintf(stderr, "usage: deb-systemd-invoke [--user] start|stop|restart UNIT...\n       deb-systemd-invoke [--user] [--no-dbus] daemon-reload|daemon-reexec\n"); return 1; }
  struct stat st;
  if (stat(SVC_RUNNING, &st) < 0) return 0; /* no manager: nothing to do */
  const char *sc = find_systemctl();
  if (!strcmp(verb, "daemon-reload") || !strcmp(verb, "daemon-reexec")) return run((char *[]){(char *)sc, (char *)verb, 0});
  if (strcmp(verb, "start") && strcmp(verb, "stop") && strcmp(verb, "restart") && strcmp(verb, "try-restart") && strcmp(verb, "reload") &&
      strcmp(verb, "force-reload")) {
    fprintf(stderr, "deb-systemd-invoke: unsupported action %s\n", verb);
    return 1;
  }
  struct svc_roots roots;
  svc_roots_default(&roots, "/debian");
  char *argv2[70];
  int n = 0;
  argv2[n++] = (char *)sc;
  argv2[n++] = (char *)verb;
  for (int i = 0; i < nu; i++) {
    if (!policy_allows(units[i], verb)) { fprintf(stderr, "deb-systemd-invoke: %s %s not allowed by policy-rc.d\n", verb, units[i]); continue; }
    if (!strcmp(verb, "start") || !strcmp(verb, "restart")) {
      const char *s = svc_is_enabled(&roots, units[i]);
      int enabled = s && (!strcmp(s, "enabled") || !strcmp(s, "static") || !strcmp(s, "indirect") || !strcmp(s, "alias"));
      if (!enabled) {
        if (!strcmp(verb, "start")) continue;
        /* restart: only if it is running */
        if (run((char *[]){(char *)sc, "--quiet", "is-active", units[i], 0}) != 0) continue;
      }
    }
    argv2[n++] = units[i];
  }
  if (n == 2) return 0;
  argv2[n] = 0;
  return run(argv2);
}
