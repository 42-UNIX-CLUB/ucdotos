// SPDX-License-Identifier: GPL-2.0-or-later
/* The fetch layer: the only code that knows how bytes arrive.
 *   file:PATH, file://PATH, copy:PATH   copied here
 *   http:, https:                        an external program does it
 * The programs for http and https come from apt.conf:
 *   Acquire::http::Command { "curl -fsSL -o %o %u"; "wget -q -O %o %u"; };
 * They are tried in order; one that is not installed is skipped. %o is the
 * output file, %u the URL. The download goes to DEST.partial and is renamed
 * when complete. Returns 0, -1 on failure, -2 when the server says the file
 * does not exist (so the caller can try another compression). */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *const default_cmds[] = {"curl -fsSL -o %o %u", "wget -q -O %o %u", 0};

static int fetch_file(const char *src, const char *dest) {
  struct stat st;
  if (stat(src, &st) < 0) return errno == ENOENT ? -2 : -1;
  char *part = xasprintf("%s.partial", dest);
  int r = file_copy(src, part, 0644);
  if (r == 0) r = rename(part, dest);
  if (r < 0) { int e = errno; unlink(part); pkg_error("cannot copy %s to %s: %s", src, part, pkg_file_error(e, part)); }
  free(part);
  return r < 0 ? -1 : 0;
}

static int fetch_cmd(const char *scheme, const char *url, const char *dest) {
  strlist cmds = {0};
  char *key = xasprintf("Acquire::%s::Command", scheme);
  aptconf_list(key, &cmds);
  const char *one = aptconf_get(key, 0);
  free(key);
  if (!cmds.n && one) sl_add(&cmds, one);
  if (!cmds.n) for (int i = 0; default_cmds[i]; i++) sl_add(&cmds, default_cmds[i]);
  char *part = xasprintf("%s.partial", dest);
  int rc = -1, tried = 0;
  for (int c = 0; c < cmds.n && rc == -1; c++) {
    strlist argv = {0};
    char *copy = xstrdup(cmds.v[c]), *save = 0;
    for (char *t = strtok_r(copy, " \t", &save); t; t = strtok_r(0, " \t", &save))
      sl_add(&argv, !strcmp(t, "%o") ? part : !strcmp(t, "%u") ? url : t);
    free(copy);
    if (!argv.n) continue;
    sl_add(&argv, "");
    free(argv.v[argv.n - 1]); argv.v[argv.n - 1] = 0;
    unlink(part);
    int st = run_command(argv.v, 0, 0);
    argv.n--;
    if (st == -1 || st == 127) { sl_free(&argv); continue; }  /* program not installed: try the next */
    tried = 1;
    struct stat sb;
    if (st == 0 && stat(part, &sb) == 0) rc = rename(part, dest) == 0 ? 0 : -1;
    else if ((!strcmp(argv.v[0], "curl") && st == 22) || (!strcmp(argv.v[0], "wget") && st == 8)) rc = -2;
    else { pkg_error("%s failed for %s (exit status %d)", argv.v[0], url, st); rc = -3; }
    sl_free(&argv);
  }
  if (rc == -3) rc = -1;
  if (!tried) pkg_error("no download program is installed for %s (tried: %s%s)", url, cmds.v[0], cmds.n > 1 ? " and others" : "");
  unlink(part);
  free(part);
  sl_free(&cmds);
  return rc;
}

int pkg_fetch(const char *url, const char *dest, pkg_progress *p) {
  if (p && p->fn) p->fn(p->ctx, url, 0, 0);
  int r;
  if (!strncmp(url, "file://", 7)) r = fetch_file(url + 7, dest);
  else if (!strncmp(url, "file:", 5)) r = fetch_file(url + 5, dest);
  else if (!strncmp(url, "copy:", 5)) r = fetch_file(url + (strncmp(url, "copy://", 7) ? 5 : 7), dest);
  else if (!strncmp(url, "http://", 7)) r = fetch_cmd("http", url, dest);
  else if (!strncmp(url, "https://", 8)) r = fetch_cmd("https", url, dest);
  else { pkg_error("unsupported URL scheme in %s", url); r = -1; }
  if (p && p->fn && r == 0) { struct stat st; if (stat(dest, &st) == 0) p->fn(p->ctx, url, st.st_size, st.st_size); }
  return r;
}
