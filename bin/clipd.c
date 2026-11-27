// SPDX-License-Identifier: GPL-2.0-or-later
/* clipd: the clipboard. Keeps the last MAX_ENTRIES copied items (newest first) and
 * answers requests on $XDG_RUNTIME_DIR/clip.sock, or /run/clip.sock without it (format in libclip/clip.h). It is an ordinary
 * program: the window manager and the kernel know nothing about it. libclip starts it
 * the first time a program uses the clipboard; a second copy that finds one already
 * running exits at once.
 *
 * It is also linked to the computer's clipboard, through the kernel's /dev/hostclip: what
 * the user pastes into the browser page (Ctrl+V there) comes in as the newest entry, and
 * whatever becomes the newest entry by a copy (PUT) or a pick (SELECT) goes out to it.
 *
 * One poll() loop, non-blocking sockets: a client that stops halfway cannot hold up the
 * others. Limits: an entry is at most CLIP_MAX_ENTRY bytes, and all entries together at
 * most MAX_TOTAL bytes; the oldest go first. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "clip.h"

#define MAX_ENTRIES 20
#define MAX_TOTAL (4 << 20)
#define MAX_CLIENTS 16
#define PREVIEW 80 /* bytes of each entry shown by LIST */

typedef struct { char *data; unsigned len; } entry;
static entry hist[MAX_ENTRIES];
static int nhist;
static unsigned total;

typedef struct {
  int fd;
  struct clip_req q;
  unsigned got;        /* bytes of the request (header, then data) read so far */
  char *in;            /* PUT data */
  char *out;           /* the answer: header and data */
  unsigned outlen, sent;
} client;
static client cl[MAX_CLIENTS];

/* ---------- history ---------- */
static void drop(int i) {
  total -= hist[i].len;
  free(hist[i].data);
  memmove(hist + i, hist + i + 1, (size_t)(nhist - i - 1) * sizeof hist[0]);
  nhist--;
}
static void to_top(int i) {
  entry e = hist[i];
  memmove(hist + 1, hist, (size_t)i * sizeof hist[0]);
  hist[0] = e;
}
/* Takes ownership of data. */
static void add(char *data, unsigned len) {
  if (!len) { free(data); return; } /* nothing copied: keep what is there */
  if (nhist && hist[0].len == len && !memcmp(hist[0].data, data, len)) { free(data); return; } /* copied twice */
  while (nhist && (nhist == MAX_ENTRIES || total + len > MAX_TOTAL)) drop(nhist - 1);
  memmove(hist + 1, hist, (size_t)nhist * sizeof hist[0]);
  hist[0] = (entry){data, len};
  nhist++;
  total += len;
}

static void host_out(void);

/* ---------- answers ---------- */
static void answer(client *c, int status, const void *data, unsigned len) {
  c->out = malloc(sizeof(struct clip_ans) + len);
  if (!c->out) { len = 0; status = -ENOMEM; c->out = malloc(sizeof(struct clip_ans)); }
  if (!c->out) { c->outlen = 0; return; } /* nothing to say: the client sees the socket close */
  struct clip_ans a = {status, len};
  memcpy(c->out, &a, sizeof a);
  if (len) memcpy(c->out + sizeof a, data, len);
  c->outlen = sizeof a + len;
  c->sent = 0;
}
/* The preview of one entry: up to PREVIEW bytes, control characters as spaces, never
 * cutting a UTF-8 character in half. */
static int preview(const entry *e, char *o) {
  unsigned n = e->len < PREVIEW ? e->len : PREVIEW;
  if (n < e->len) while (n > 0 && ((unsigned char)e->data[n] & 0xc0) == 0x80) n--;
  for (unsigned i = 0; i < n; i++) { unsigned char ch = e->data[i]; o[i] = ch < 32 || ch == 127 ? ' ' : (char)ch; }
  return (int)n;
}
static void handle(client *c) {
  struct clip_req *q = &c->q;
  int i = (int)q->arg;
  switch (q->op) {
  case CLIP_PUT:
    add(c->in, q->len);
    c->in = NULL;
    if (q->len) host_out(); /* an empty copy changes nothing */
    answer(c, 0, NULL, 0);
    return;
  case CLIP_GET:
    if (i < 0 || i >= nhist) answer(c, -ENOENT, NULL, 0);
    else answer(c, 0, hist[i].data, hist[i].len);
    return;
  case CLIP_LIST: {
    char *t = malloc((size_t)nhist * (PREVIEW + 16) + 1);
    if (!t) { answer(c, -ENOMEM, NULL, 0); return; }
    int n = 0;
    for (int k = 0; k < nhist; k++) {
      n += snprintf(t + n, 16, "%u\t", hist[k].len);
      n += preview(&hist[k], t + n);
      t[n++] = '\n';
    }
    answer(c, 0, t, (unsigned)n);
    free(t);
    return;
  }
  case CLIP_SELECT:
    if (i < 0 || i >= nhist) { answer(c, -ENOENT, NULL, 0); return; }
    to_top(i);
    host_out();
    answer(c, 0, NULL, 0);
    return;
  case CLIP_REMOVE:
    if (i < 0 || i >= nhist) { answer(c, -ENOENT, NULL, 0); return; }
    drop(i);
    answer(c, 0, NULL, 0);
    return;
  case CLIP_CLEAR:
    while (nhist) drop(nhist - 1);
    answer(c, 0, NULL, 0);
    return;
  }
  answer(c, -EINVAL, NULL, 0);
}

/* ---------- the computer's clipboard ---------- */
#define FIONREAD 0x541B
static int host_fd = -1;
/* A paste is waiting: take it whole (see kernel/dev.c). */
static void host_readable(void) {
  for (int tries = 0; tries < 4; tries++) { /* a newer, bigger paste may land in between */
    unsigned n = 0;
    if (__syscall3(SYS_ioctl, host_fd, FIONREAD, &n) < 0 || !n) return;
    if (n > CLIP_MAX_ENTRY) n = CLIP_MAX_ENTRY; /* the kernel never holds more */
    char *buf = malloc(n);
    if (!buf) return;
    long got = read(host_fd, buf, n);
    if (got > 0) { add(buf, (unsigned)got); return; }
    free(buf);
    if (got < 0 && errno != EINVAL) return; /* EAGAIN: someone else took it */
  }
}

/* The newest entry, to the computer's clipboard (one write is one item). */
static void host_out(void) {
  if (host_fd >= 0 && nhist) write(host_fd, hist[0].data, hist[0].len);
}

/* ---------- clients ---------- */
static void close_client(client *c) {
  close(c->fd);
  free(c->in); free(c->out);
  memset(c, 0, sizeof *c);
  c->fd = -1;
}
/* Read what has arrived; once the request is whole, prepare the answer. */
static void readable(client *c) {
  for (;;) {
    unsigned hdr = sizeof c->q, want = c->got < hdr ? hdr - c->got : hdr + c->q.len - c->got;
    if (!want) break;
    char *dst = c->got < hdr ? (char *)&c->q + c->got : c->in + (c->got - hdr);
    long n = read(c->fd, dst, want);
    if (n < 0 && errno == EAGAIN) return;
    if (n <= 0) { close_client(c); return; }
    c->got += (unsigned)n;
    if (c->got == hdr) { /* the header is in: check it, make room for the data */
      if (c->q.op != CLIP_PUT) c->q.len = 0;
      if (c->q.len > CLIP_MAX_ENTRY) { answer(c, -ENOSPC, NULL, 0); return; }
      if (c->q.op == CLIP_PUT && !(c->in = malloc(c->q.len ? c->q.len : 1))) { answer(c, -ENOMEM, NULL, 0); return; }
    }
  }
  handle(c);
}
static void writable(client *c) {
  while (c->sent < c->outlen) {
    long n = write(c->fd, c->out + c->sent, c->outlen - c->sent);
    if (n < 0 && errno == EAGAIN) return;
    if (n <= 0) break;
    c->sent += (unsigned)n;
  }
  close_client(c); /* all sent (or the client left): one request per connection */
}

int main(void) {
  struct { unsigned short fam; char path[108]; } addr = {1, ""};
  clip_socket(addr.path);
  /* already running? then there is nothing to do */
  int probe = __syscall3(SYS_socket, 1, 1 | O_CLOEXEC, 0);
  if (probe >= 0 && __syscall3(SYS_connect, probe, &addr, sizeof addr) == 0) { close(probe); return 0; }
  if (probe >= 0) close(probe);
  mkdir("/run", 0755);
  unlink(addr.path); /* a name left by a clipd that is gone */
  int lfd = __syscall3(SYS_socket, 1, 1 | O_NONBLOCK | O_CLOEXEC, 0);
  if (lfd < 0 || __syscall3(SYS_bind, lfd, &addr, sizeof addr) < 0 || __syscall2(SYS_listen, lfd, 16) < 0) {
    fprintf(stderr, "clipd: cannot listen on %s\n", addr.path);
    return 1;
  }
  for (int i = 0; i < MAX_CLIENTS; i++) cl[i].fd = -1;
  host_fd = open("/dev/hostclip", O_RDWR | O_NONBLOCK | O_CLOEXEC); /* absent on older kernels: fine */
  for (;;) {
    struct pollfd pf[MAX_CLIENTS + 2];
    client *who[MAX_CLIENTS + 2];
    int n = 0;
    pf[n] = (struct pollfd){lfd, POLLIN, 0}; who[n++] = NULL;
    pf[n] = (struct pollfd){host_fd, POLLIN, 0}; who[n++] = NULL; /* fd -1 is skipped */
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (cl[i].fd < 0) continue;
      pf[n] = (struct pollfd){cl[i].fd, cl[i].out ? POLLOUT : POLLIN, 0};
      who[n++] = &cl[i];
    }
    if (poll(pf, n, -1) < 0) continue;
    if (pf[1].revents & POLLIN) host_readable();
    for (int k = 2; k < n; k++) {
      if (!pf[k].revents) continue;
      client *c = who[k];
      if (c->out) writable(c);
      else readable(c);
      if (c->fd >= 0 && c->out && !(pf[k].revents & POLLHUP)) writable(c); /* answer at once if it fits */
    }
    if (pf[0].revents & POLLIN)
      for (;;) {
        int fd = __syscall1(SYS_accept, lfd);
        if (fd < 0) break;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (cl[i].fd < 0) slot = i;
        if (slot < 0) { close(fd); continue; } /* full: the client sees the socket close and can try again */
        fcntl(fd, F_SETFL, O_NONBLOCK);
        memset(&cl[slot], 0, sizeof cl[slot]);
        cl[slot].fd = fd;
      }
  }
}
