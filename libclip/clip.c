// SPDX-License-Identifier: GPL-2.0-or-later
/* libclip: talk to clipd. See clip.h. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "clip.h"

#define WAIT_MS 2000 /* longest wait for clipd at any one step; it never takes this long */

extern char **environ;

static int try_connect(void) {
  struct { unsigned short fam; char path[108]; } addr = {1, CLIP_SOCKET};
  int fd = __syscall3(SYS_socket, 1, 1 | O_NONBLOCK | O_CLOEXEC, 0);
  if (fd < 0) return fd;
  int r = __syscall3(SYS_connect, fd, &addr, sizeof addr);
  if (r < 0) { close(fd); return r; }
  return fd;
}

/* Connect, starting clipd first if nobody answers. */
static int clip_connect(void) {
  int fd = try_connect();
  if (fd >= 0) return fd;
  char *argv[] = {"/bin/clipd", 0};
  int none[3] = {-1, -1, -1};
  if (spawn("/bin/clipd", argv, environ, none) < 0) return -ENOENT;
  for (int i = 0; i < 200; i++) { /* it binds the socket within a few milliseconds */
    usleep(5000);
    if ((fd = try_connect()) >= 0) return fd;
  }
  return fd;
}

/* Move n bytes over fd in the given direction, waiting at most WAIT_MS for each step. */
static int xfer(int fd, void *buf, size_t n, int out) {
  unsigned char *p = buf;
  while (n) {
    long r = out ? write(fd, p, n) : read(fd, p, n);
    if (r > 0) { p += r; n -= (size_t)r; continue; }
    if (r == 0) return -EPIPE; /* the other end closed early */
    if (errno != EAGAIN) return -errno;
    struct pollfd pf = {fd, out ? POLLOUT : POLLIN, 0};
    if (poll(&pf, 1, WAIT_MS) <= 0) return -EIO; /* clipd did not answer in time */
  }
  return 0;
}

/* One request, one answer. data and dlen: what to send. ret and rlen: what came back (ret may be NULL). */
static int call(unsigned op, unsigned arg, const void *data, size_t dlen, char **ret, size_t *rlen) {
  if (dlen > CLIP_MAX_ENTRY) return -ENOSPC;
  int fd = clip_connect();
  if (fd < 0) return fd;
  struct clip_req q = {op, arg, (unsigned)dlen};
  struct clip_ans a;
  int r = xfer(fd, &q, sizeof q, 1);
  if (!r && dlen) r = xfer(fd, (void *)data, dlen, 1);
  if (!r) r = xfer(fd, &a, sizeof a, 0);
  if (!r && a.len > CLIP_MAX_ENTRY + 65536) r = -EIO; /* not an answer clipd would give */
  char *buf = NULL;
  if (!r && a.len) {
    if (!(buf = malloc(a.len + 1))) r = -ENOMEM;
    else r = xfer(fd, buf, a.len, 0);
  }
  close(fd);
  if (!r) r = a.status;
  if (r || !ret) { free(buf); return r; }
  if (!buf && !(buf = malloc(1))) return -ENOMEM;
  buf[a.len] = 0;
  *ret = buf;
  if (rlen) *rlen = a.len;
  return 0;
}

int clip_put(const void *data, size_t len) { return call(CLIP_PUT, 0, data, len, NULL, NULL); }
int clip_get(int index, char **data, size_t *len) { return call(CLIP_GET, (unsigned)index, NULL, 0, data, len); }
int clip_list(char **text, size_t *len) { return call(CLIP_LIST, 0, NULL, 0, text, len); }
int clip_select(int index) { return call(CLIP_SELECT, (unsigned)index, NULL, 0, NULL, NULL); }
int clip_remove(int index) { return call(CLIP_REMOVE, (unsigned)index, NULL, 0, NULL, NULL); }
int clip_clear(void) { return call(CLIP_CLEAR, 0, NULL, 0, NULL, NULL); }
