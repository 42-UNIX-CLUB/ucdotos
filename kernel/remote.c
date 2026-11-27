// SPDX-License-Identifier: GPL-2.0-or-later
/* Files that live on the web server until someone opens them, and files the host
 * pushes in from outside (a file the user dropped onto the page).
 *
 * Remote files. The image carries a manifest, /etc/remote-files, one line per file:
 *     PATH SIZE SHA256 URL [gz]
 * At boot each line becomes an ordinary-looking regular file with its real size but no
 * data (st_blocks 0). The first open asks the host to fetch URL (relative to the page;
 * with "gz", URL.gz is tried first). The opener waits on the normal blocking path; other
 * openers join the same wait, so the host is asked once. The host writes the bytes
 * straight into a kernel buffer of SIZE bytes as they arrive (remote_data), the kernel
 * hashes them on the way, and at the end (remote_end) the file gets its data if the size
 * and the SHA-256 match. From then on it is a plain file in kernel memory.
 *
 * A failed fetch (offline, HTTP error, wrong size or hash) gives every process that was
 * waiting for it EIO on its next try of open, once, and leaves the file as it was, so a
 * later open fetches again. Because a blocked call is re-run from the start, "was waiting
 * for that fetch" is remembered per pid in a mask (`waiting` while the fetch runs,
 * `failed` after it failed). A pid that dies while waiting leaves its bit; the next
 * process with that number gets one spurious EIO from that one file. Opening with
 * O_NONBLOCK starts the fetch and returns EAGAIN until it is done; stat shows progress in
 * st_blocks (bytes received / 512).
 *
 * Imports. The host creates a file from outside with import_begin(len, size) (the path is
 * first written into remote_scratch()), then the same remote_data calls, then remote_end.
 * The finished file replaces any regular file of that name, as cp would. */
#include "kernel.h"
void store_dirty(struct inode *i, u32 lo, u32 hi); /* store.c */

HOST(fetch) void host_fetch(int id, const char *url, u32 url_len, u8 *dst, u32 size, int gz);

#define NREMOTE 32
#define MANIFEST "/etc/remote-files"
#define IMPORT_MAX (256u << 20) /* largest file the host may push in */

/* ---- SHA-256 (FIPS 180-4) ---- */
struct sha256 { u32 h[8]; u64 len; u8 buf[64]; u32 n; };
static const u32 K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(u32 *h, const u8 *p) {
  u32 w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  for (int i = 0; i < 64; i++) {
    u32 t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
static void sha_init(struct sha256 *s) {
  static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void sha_update(struct sha256 *s, const u8 *p, u32 n) {
  s->len += n;
  if (s->n) {
    while (n && s->n < 64) { s->buf[s->n++] = *p++; n--; }
    if (s->n < 64) return;
    sha_block(s->h, s->buf); s->n = 0;
  }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  memcpy(s->buf, p, n); s->n = n;
}
static void sha_final(struct sha256 *s, u8 out[32]) {
  u64 bits = s->len * 8;
  u8 pad = 0x80;
  sha_update(s, &pad, 1);
  pad = 0;
  while (s->n != 56) sha_update(s, &pad, 1);
  for (int i = 7; i >= 0; i--) { u8 b = bits >> (8 * i); sha_update(s, &b, 1); }
  for (int i = 0; i < 32; i++) out[i] = s->h[i / 4] >> (24 - 8 * (i % 4));
}

static char *next_slash(char *s) { while (*s && *s != '/') s++; return *s ? s : NULL; }

/* ---- records ---- */
enum { R_IDLE, R_FETCHING };
enum { K_REMOTE = 1, K_IMPORT };
struct remote {
  u8 kind, state, gz;
  u32 size, got;
  u8 sha[32];
  char str[PATH_MAX]; /* the URL (remote) or the target path (import) */
  u8 *buf;
  struct inode *ino;  /* the file being filled; holds a reference while fetching */
  struct sha256 h;
  waitq wq;
  u64 waiting, failed; /* pids waiting for the current fetch / owed the error of the last one */
};
static struct remote rt[NREMOTE];

static struct remote *remote_of(struct inode *i) {
  return (i->mode & S_IFMT) == S_IFREG ? i->priv : NULL;
}
/* st_blocks for a remote file: what has arrived so far (0 before any fetch). */
u32 remote_blocks(struct inode *i) {
  struct remote *r = remote_of(i);
  return r && r->state == R_FETCHING ? r->got / 512 : 0;
}

static void finish(struct remote *r, int ok) {
  struct inode *i = r->ino;
  if (ok) {
    if (i->owned) kfree(i->data);
    i->data = r->buf; i->size = i->cap = r->size; i->owned = 1;
    i->priv = NULL; /* an ordinary file from now on */
    r->failed = 0;
  } else {
    kfree(r->buf);
    r->failed |= r->waiting;
  }
  r->buf = NULL; r->ino = NULL; r->waiting = 0; r->state = R_IDLE;
  if (r->kind == K_IMPORT) r->kind = 0; /* the slot is free again */
  inode_put(i);
  wq_wake(&r->wq);
}

/* Called by sys_openat for a regular file. 0: go on and open it. */
long remote_open(struct inode *i, int flags) {
  struct remote *r = remote_of(i);
  if (!r) return 0;
  u64 bit = 1ull << cur->pid;
  if (r->failed & bit) { r->failed &= ~bit; return -EIO; }
  if (r->state == R_IDLE) {
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) { i->priv = NULL; i->size = 0; return 0; } /* its old contents do not matter */
    if (!(r->buf = kmalloc(r->size ? r->size : 1))) return -ENOMEM;
    r->got = 0; r->state = R_FETCHING; r->ino = i;
    sha_init(&r->h);
    inode_get(i);
    kprintf("[remote] fetching %s (%u bytes)\n", r->str, r->size);
    host_fetch(r - rt, r->str, strlen(r->str), r->buf, r->size, r->gz);
  }
  r->waiting |= bit;
  if (flags & O_NONBLOCK) return -EAGAIN;
  wq_add(&r->wq);
  return block(-1);
}

static struct remote *fetching(int id) {
  if (id < 0 || id >= NREMOTE || !rt[id].kind || rt[id].state != R_FETCHING) return NULL;
  return &rt[id];
}
/* The host wrote len more bytes at buf + got. -1: refused (the fetch is now failed). */
EXPORT(remote_data) int remote_data(int id, u32 len) {
  struct remote *r = fetching(id);
  if (!r) return -1;
  if (len > r->size - r->got) { kprintf("[remote] %s: more than %u bytes\n", r->str, r->size); finish(r, 0); return -1; }
  if (r->kind == K_REMOTE) sha_update(&r->h, r->buf + r->got, len);
  r->got += len;
  return 0;
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* The host is done: ok = 1 if every byte arrived, 0 if the transfer failed. Returns 0 if
 * the file is now in place, -1 if it was refused. */
EXPORT(remote_end) int remote_end(int id, int ok) {
  struct remote *r = fetching(id);
  if (!r) return -1;
  const char *why = ok ? NULL : "transfer failed";
  if (!why && r->got != r->size) why = "short";
  if (!why && r->kind == K_REMOTE) {
    u8 sum[32];
    sha_final(&r->h, sum);
    if (memcmp(sum, r->sha, 32)) why = "SHA-256 does not match";
  }
  if (r->kind == K_IMPORT && !why) {
    /* put it in place now: replace the regular file of that name, or create it */
    struct inode *i = vfs_lookup(vfs_root, r->str);
    int e = 0;
    if (i && ((i->mode & S_IFMT) != S_IFREG || (remote_of(i) && remote_of(i)->state == R_FETCHING))) why = "the name is taken";
    else if (!i && !(i = vfs_create(vfs_root, r->str, S_IFREG | 0644, &e))) why = "cannot create it";
    else {
      inode_put(r->ino); /* the placeholder reference */
      inode_get(i);
      i->priv = NULL;
      i->mtime = i->ctime = KT_MAKE((s64)(host_epoch() / 1000), 0); /* file times are packed (kernel.h KT_MAKE) */
      r->ino = i;
    }
  }
  if (why) kprintf("[remote] %s: %s, not used\n", r->str, why);
  else kprintf("[remote] %s: %u bytes, ready\n", r->str, r->size);
  struct inode *imp = (r->kind == K_IMPORT && !why) ? r->ino : NULL;
  finish(r, !why);
  /* a file the user brought in is theirs like any other: it is stored (a remote file is not) */
  if (imp && imp->sflags) { store_dirty(imp, 0, imp->size); vfs_changed(imp); }
  return why ? -1 : 0;
}

/* ---- imports ---- */
static char scratch[PATH_MAX];
static struct inode import_dummy = {.mode = S_IFREG, .nlink = 1, .refs = 1}; /* held while the bytes arrive */
EXPORT(remote_scratch) char *remote_scratch(void) { return scratch; }
EXPORT(remote_buf) u8 *remote_buf(int id) { struct remote *r = fetching(id); return r ? r->buf : NULL; }
/* The path (len bytes) is in remote_scratch(). Returns an id for remote_data/remote_end, or -errno. */
EXPORT(import_begin) int import_begin(u32 len, u32 size) {
  if (len == 0 || len >= PATH_MAX || scratch[0] != '/') return -EINVAL;
  if (size > IMPORT_MAX) return -ENOSPC;
  struct remote *r = NULL;
  for (int i = 0; i < NREMOTE && !r; i++) if (!rt[i].kind) r = &rt[i];
  if (!r) return -ENFILE; /* every slot is busy */
  memset(r, 0, sizeof *r);
  memcpy(r->str, scratch, len);
  /* the directory must exist: create the missing levels */
  for (char *s = r->str + 1; (s = next_slash(s)); s++) {
    *s = 0;
    int e;
    if (!vfs_lookup(vfs_root, r->str)) vfs_create(vfs_root, r->str, S_IFDIR | 0755, &e);
    *s = '/';
  }
  const char *base = strrchr(r->str, '/') + 1;
  if (!*base || strlen(base) > VFS_NAME_MAX || !strcmp(base, ".") || !strcmp(base, "..")) return -EINVAL;
  if (!(r->buf = kmalloc(size ? size : 1))) return -ENOSPC;
  r->kind = K_IMPORT; r->state = R_FETCHING; r->size = size;
  r->ino = &import_dummy;
  inode_get(r->ino);
  kprintf("[remote] importing %s (%u bytes)\n", r->str, size);
  return r - rt;
}

/* ---- the manifest ---- */
static const char *word(const char *p, const char *end, const char **w, u32 *n) {
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  *w = p;
  while (p < end && *p != ' ' && *p != '\t') p++;
  *n = p - *w;
  return p;
}
void remote_init(void) {
  struct inode *m = vfs_lookup(vfs_root, MANIFEST);
  if (!m || (m->mode & S_IFMT) != S_IFREG) return;
  const char *p = (const char *)m->data, *end = p + m->size;
  int n = 0;
  while (p < end) {
    const char *eol = p;
    while (eol < end && *eol != '\n') eol++;
    const char *w[5]; u32 l[5]; const char *q = p;
    for (int k = 0; k < 5; k++) q = word(q, eol, &w[k], &l[k]);
    p = eol + 1;
    if (!l[0] || w[0][0] == '#') continue;
    struct remote *r = &rt[n];
    char path[PATH_MAX];
    u32 size = 0;
    int bad = n >= NREMOTE || l[0] >= PATH_MAX || w[0][0] != '/' || l[2] != 64 || !l[3] || l[3] >= PATH_MAX || !l[1];
    for (u32 k = 0; !bad && k < l[1]; k++) {
      if (w[1][k] < '0' || w[1][k] > '9' || size > 0x7fffffff / 10) bad = 1;
      else size = size * 10 + (w[1][k] - '0');
    }
    for (int k = 0; !bad && k < 32; k++) {
      int hi = hexval(w[2][2 * k]), lo = hexval(w[2][2 * k + 1]);
      if (hi < 0 || lo < 0) bad = 1; else r->sha[k] = hi << 4 | lo;
    }
    if (bad) { kprintf("[remote] %s: bad line skipped\n", MANIFEST); continue; }
    memcpy(path, w[0], l[0]); path[l[0]] = 0;
    for (char *s = path + 1; (s = next_slash(s)); s++) { /* create the missing directories */
      *s = 0;
      int e;
      if (!vfs_lookup(vfs_root, path)) vfs_create(vfs_root, path, S_IFDIR | 0755, &e);
      *s = '/';
    }
    if (vfs_lookup(vfs_root, path)) continue; /* a real file of that name is in the image: it wins */
    int e;
    struct inode *i = vfs_create(vfs_root, path, S_IFREG | 0444, &e);
    if (!i) continue;
    i->owned = 0; i->data = NULL; i->size = size; i->cap = 0;
    i->priv = r;
    i->sflags |= S_NEVER_BIT; /* a 28 MB game file must not end up in the browser's store (store.c) */
    r->kind = K_REMOTE; r->size = size;
    memcpy(r->str, w[3], l[3]); r->str[l[3]] = 0;
    r->gz = l[4] == 2 && w[4][0] == 'g' && w[4][1] == 'z';
    n++;
  }
  if (n) kprintf("[remote] %d files fetched on first use\n", n);
}
