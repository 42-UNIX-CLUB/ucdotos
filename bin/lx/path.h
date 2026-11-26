// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest paths. The guest's "/" is a directory of the host file system chosen by
 * configuration (lx_cfg.root). With root "/" paths pass through unchanged. Otherwise
 * every path a guest gives is resolved here, like a chroot done in user space: ".." stops
 * at the root, and symbolic links whose targets are absolute are resolved inside the
 * root. /dev and /proc are taken from the host. */
#ifndef LX_PATH_H
#define LX_PATH_H
#include <stddef.h>
#include <stdint.h>

#define LX_PATH_MAX 4096

/* Backend for path resolution, so the module can be tested without the engine. */
struct path_ops {
  /* lstat-like: 1 symlink, 0 other existing object, -errno */
  int (*kind)(const char *hostpath);
  /* readlink into buf (NUL-terminated); length or -errno */
  int64_t (*readlink)(const char *hostpath, char *buf, size_t cap);
  /* guest path of a directory descriptor (absolute), or 0 */
  const char *(*fd_path)(int fd);
};

int path_init(const char *root, const struct path_ops *ops);
int path_is_rooted(void);
const char *path_root(void);
/* Guest cwd as the guest sees it (absolute). */
const char *path_cwd(void);
void path_set_cwd(const char *guest_abs);

/* Resolve a guest path to an absolute guest path (no symlinks left unless !follow for the
 * last component, no "." or ".."). dirfd: LX_AT_FDCWD or a directory descriptor. */
int64_t path_resolve(int dirfd, const char *g, int follow, char *out, size_t cap);
/* Guest path -> host path. In pass-through mode the result may be relative and *hostdirfd
 * keeps dirfd; in rooted mode the result is absolute and *hostdirfd is AT_FDCWD. */
int64_t path_xlate(int dirfd, const char *g, int follow, char *out, size_t cap, int *hostdirfd);
/* Convenience for the loader: always returns a path usable with AT_FDCWD. */
int64_t path_to_host(int dirfd, const char *g, int follow, char *out, size_t cap);
/* Host absolute path -> guest path (strip the root), for getcwd and readlink. */
int64_t path_from_host(const char *host, char *out, size_t cap);

#endif
