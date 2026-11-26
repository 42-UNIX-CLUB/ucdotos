// SPDX-License-Identifier: GPL-2.0-or-later
/* Tests of the path translation module (path.c) against a fake file system: a table of
 * host paths that are symbolic links. No engine and no real files needed. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../path.h"

static const char *links[][2] = {
  {"/R/bin", "usr/bin"},                 /* relative, like Debian's merged /usr */
  {"/R/lib", "/usr/lib"},                /* absolute: must stay inside the root */
  {"/R/usr/lib/ld.so", "x86/ld-2.so"},
  {"/R/etc/alt/sh", "/bin/dash"},        /* absolute, through another link */
  {"/R/loop1", "loop2"}, {"/R/loop2", "loop1"},
  {"/R/up", "../../../../etc"},          /* tries to climb out of the root */
  {"/R/dir/dot", "."},
};
static int kind(const char *hp) {
  for (unsigned i = 0; i < sizeof links / sizeof links[0]; i++) if (!strcmp(hp, links[i][0])) return 1;
  return 0;
}
static int64_t rl(const char *hp, char *buf, size_t cap) {
  for (unsigned i = 0; i < sizeof links / sizeof links[0]; i++)
    if (!strcmp(hp, links[i][0])) { snprintf(buf, cap, "%s", links[i][1]); return (int64_t)strlen(links[i][1]); }
  return -22;
}
static const char *fdp(int fd) { return fd == 7 ? "/usr/share" : 0; }
static const struct path_ops ops = {kind, rl, fdp};

static int pass, fail;
static void t_res(int dirfd, const char *g, int follow, const char *want) {
  char out[LX_PATH_MAX];
  int64_t r = path_resolve(dirfd, g, follow, out, sizeof out);
  const char *got = r < 0 ? (r == -40 ? "ELOOP" : r == -9 ? "EBADF" : r == -2 ? "ENOENT" : "ERR") : out;
  if (!strcmp(got, want)) pass++; else { fail++; printf("FAIL resolve(%d, %s, %d): got %s want %s\n", dirfd, g, follow, got, want); }
}
static void t_host(const char *g, const char *want) {
  char out[LX_PATH_MAX];
  int64_t r = path_to_host(-100, g, 1, out, sizeof out);
  const char *got = r < 0 ? "ERR" : out;
  if (!strcmp(got, want)) pass++; else { fail++; printf("FAIL to_host(%s): got %s want %s\n", g, got, want); }
}
static void t_from(const char *h, const char *want) {
  char out[LX_PATH_MAX];
  int64_t r = path_from_host(h, out, sizeof out);
  const char *got = r < 0 ? "ERR" : out;
  if (!strcmp(got, want)) pass++; else { fail++; printf("FAIL from_host(%s): got %s want %s\n", h, got, want); }
}

int main(void) {
  path_init("/R/", &ops);
  t_res(-100, "/", 1, "/");
  t_res(-100, "/usr/bin/ls", 1, "/usr/bin/ls");
  t_res(-100, "/bin/ls", 1, "/usr/bin/ls");                 /* relative link */
  t_res(-100, "/lib/x", 1, "/usr/lib/x");                   /* absolute link stays in root */
  t_res(-100, "/lib/ld.so", 1, "/usr/lib/x86/ld-2.so");     /* two links */
  t_res(-100, "/lib/ld.so", 0, "/usr/lib/ld.so");           /* last component not followed */
  t_res(-100, "/etc/alt/sh", 1, "/usr/bin/dash");           /* absolute link to a link */
  t_res(-100, "/../../etc", 1, "/etc");                     /* .. stops at the root */
  t_res(-100, "/up/passwd", 1, "/etc/passwd");              /* a link cannot climb out */
  t_res(-100, "/a/./b//c/..", 1, "/a/b");
  t_res(-100, "/loop1", 1, "ELOOP");
  t_res(-100, "/loop1", 0, "/loop1");
  t_res(-100, "/dir/dot/x", 1, "/dir/x");
  t_res(-100, "", 1, "ENOENT");
  path_set_cwd("/home/user");
  t_res(-100, "file", 1, "/home/user/file");
  t_res(-100, "../../../x", 1, "/x");
  t_res(7, "doc", 1, "/usr/share/doc");                     /* relative to a directory fd */
  t_res(8, "doc", 1, "EBADF");
  t_res(7, "/abs", 1, "/abs");                              /* absolute ignores dirfd */
  t_host("/bin/sh", "/R/usr/bin/sh");
  t_host("/dev/null", "/dev/null");                         /* passed through */
  t_host("/proc/self/fd", "/proc/self/fd");
  t_host("/", "/R/");
  t_from("/R/usr/bin", "/usr/bin");
  t_from("/R", "/");
  t_from("/Rother", "ERR");                                 /* not inside the root */
  t_from("/dev/tty", "/dev/tty");
  /* pass-through mode */
  path_init("/", &ops);
  t_host("relative/x", "relative/x");
  t_host("/bin/ls", "/bin/ls");
  printf("path: %d checks, %d passed, %d failed\n", pass + fail, pass, fail);
  return fail != 0;
}
