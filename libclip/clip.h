// SPDX-License-Identifier: GPL-2.0-or-later
/* libclip: the client side of the clipboard. The clipboard is its own program, clipd,
 * which keeps a short history of copied data and answers on the AF_UNIX stream socket
 * /run/clip.sock. It has nothing to do with the window manager or the kernel: any
 * program can use it, with or without a window. The first call that finds no clipd
 * starts one (/bin/clipd); it then runs until the system stops.
 *
 * Wire format: one request per connection, then clipd answers and closes it.
 *   request  struct clip_req, then len bytes of data (PUT only)
 *   answer   struct clip_ans, then len bytes of data (GET and LIST only)
 * Entries are numbered from 0, the newest. */
#ifndef CLIP_H
#define CLIP_H
#include <stddef.h>

#define CLIP_SOCKET "/run/clip.sock"
#define CLIP_MAX_ENTRY (1 << 20)   /* largest single entry: 1 MiB */

enum { CLIP_PUT = 1, CLIP_GET, CLIP_LIST, CLIP_SELECT, CLIP_REMOVE, CLIP_CLEAR };
struct clip_req { unsigned op, arg, len; };
struct clip_ans { int status; unsigned len; };  /* status 0 = ok, else a negative errno */

/* Each returns 0 or a negative errno. A data result is malloc'ed and is the caller's to
 * free; it has an extra 0 byte at the end that *len does not count. */
/* -ENOSPC: more than CLIP_MAX_ENTRY bytes. -EIO: clipd did not answer properly in time. */
int clip_put(const void *data, size_t len);        /* becomes entry 0 */
int clip_get(int index, char **data, size_t *len); /* -ENOENT if there is no such entry */
/* One line per entry, newest first: "SIZE\tPREVIEW\n". The preview is the start of the
 * data with every control character (newlines, tabs ...) shown as a space. */
int clip_list(char **text, size_t *len);
int clip_select(int index); /* move entry index to the top, so that it is pasted next */
int clip_remove(int index);
int clip_clear(void);

#endif
