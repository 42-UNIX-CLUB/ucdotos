// SPDX-License-Identifier: GPL-2.0-or-later
/* persisttest: workloads and checkers for the persistent store.
 *
 *   persisttest fill DIR [NSMALL] [BIGMB]   make a tree with every kind of object, then sync
 *   persisttest verify DIR [NSMALL] [BIGMB] check it byte for byte and attribute for attribute
 *   persisttest crash-run SEED NOPS DIR     a random sequence of single-call changes, with a
 *                                           sync every 20 changes ("synced K" on stdout)
 *   persisttest crash-check SEED NOPS DIR K the tree must equal the state after some change
 *                                           i >= K of that same sequence: no change half
 *                                           applied, nothing synced lost
 *   persisttest bigwrite FILE MB            write MB MiB in 1 MiB pieces ("wrote N MiB")
 *   persisttest bigcheck FILE               whatever survived must be a consistent prefix
 *   persisttest full DIR                    with a small quota (run.mjs --quota): write until
 *                                           ENOSPC, check the warnings, delete, write again
 *   persisttest fullcheck DIR               after a reboot: what fsync acknowledged is there
 *
 * File contents follow a pattern from an id and the offset, so any file can be checked
 * on its own: bytes 0..3 hold the id, byte o (o >= 4) is pat(id, o). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

static int checks, fails;
static void check(const char *what, int ok, const char *detail) {
  checks++;
  if (!ok) { fails++; printf("  FAIL  %s %s\n", what, detail ? detail : ""); }
}
static unsigned char pat(unsigned id, unsigned o) { return (unsigned char)(o * 131u + id * 7u + (o >> 8) + (o >> 16) * 3u); }
static void fillpat(unsigned char *b, unsigned id, unsigned off, unsigned n) {
  for (unsigned k = 0; k < n; k++) {
    unsigned o = off + k;
    b[k] = o < 4 ? (unsigned char)(id >> (8 * o)) : pat(id, o);
  }
}
static unsigned char buf[1 << 20];
static int write_file(const char *path, unsigned id, unsigned size, int mode) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
  if (fd < 0) return -1;
  for (unsigned off = 0; off < size;) {
    unsigned n = size - off < sizeof buf ? size - off : sizeof buf;
    fillpat(buf, id, off, n);
    if (write(fd, buf, n) != (long)n) { close(fd); return -1; }
    off += n;
  }
  return close(fd);
}
/* 1: contents follow the pattern of id for exactly st_size bytes (id read from the file if id < 0) */
static int file_ok(const char *path, long id, unsigned *got_id) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  struct stat st;
  fstat(fd, &st);
  unsigned char cmp[65536];
  unsigned off = 0, ok = 1, fid = (unsigned)id;
  while (off < st.st_size) {
    long n = read(fd, buf, sizeof buf);
    if (n <= 0) { ok = 0; break; }
    if (off == 0 && id < 0 && n >= 4) fid = buf[0] | buf[1] << 8 | buf[2] << 16 | (unsigned)buf[3] << 24;
    for (long k = 0; k < n; k += sizeof cmp) {
      long m = n - k < (long)sizeof cmp ? n - k : (long)sizeof cmp;
      fillpat(cmp, fid, off + k, m);
      if (memcmp(cmp, buf + k, m)) { ok = 0; break; }
    }
    off += n;
  }
  if (read(fd, buf, 1) != 0) ok = 0;
  close(fd);
  if (got_id) *got_id = fid;
  return ok && off == (unsigned)st.st_size;
}

/* ---- fill and verify ---- */
static const unsigned sizes[] = {0, 1, 3, 100, 4095, 4096, 4097, 65535, 65536, 65537, 131072, 1000000};
#define NSIZES (sizeof sizes / sizeof *sizes)
static const struct timespec t_set[2] = {{1100000000, 123456789}, {1200000000, 987654321}};
static int fill(const char *dir, int nsmall, int bigmb) {
  char p[512], q[512];
  mkdir(dir, 0755);
  for (unsigned k = 0; k < NSIZES; k++) { snprintf(p, sizeof p, "%s/size-%u", dir, sizes[k]); if (write_file(p, 1000 + k, sizes[k], 0644)) return 1; }
  if (bigmb) { snprintf(p, sizeof p, "%s/big", dir); if (write_file(p, 7, (unsigned)bigmb << 20, 0644)) return 1; }
  snprintf(p, sizeof p, "%s/deep", dir);
  for (int k = 0; k < 30; k++) { mkdir(p, 0750); strcat(p, "/d"); }
  strcat(p, "-leaf"); write_file(p, 42, 1234, 0600);
  snprintf(p, sizeof p, "%s/many", dir); mkdir(p, 0755);
  for (int k = 0; k < nsmall; k++) { snprintf(p, sizeof p, "%s/many/f%05d", dir, k); if (write_file(p, 100000 + k, 16 + k % 200, 0644)) return 1; }
  snprintf(p, sizeof p, "%s/size-100", dir); snprintf(q, sizeof q, "%s/hard", dir); link(p, q);
  snprintf(q, sizeof q, "%s/many/hard2", dir); link(p, q);
  snprintf(p, sizeof p, "%s/rel", dir); symlink("size-100", p);
  snprintf(p, sizeof p, "%s/abs", dir); snprintf(q, sizeof q, "%s/deep/d", dir); symlink(q, p);
  snprintf(p, sizeof p, "%s/dangling", dir); symlink("no/such/thing", p);
  snprintf(p, sizeof p, "%s/suid", dir); write_file(p, 9, 10, 0644); chmod(p, 04755);
  snprintf(p, sizeof p, "%s/none", dir); write_file(p, 10, 10, 0644); chmod(p, 0);
  snprintf(p, sizeof p, "%s/owned", dir); write_file(p, 11, 10, 0644); chown(p, 123, 45);
  snprintf(p, sizeof p, "%s/timed", dir); write_file(p, 12, 10, 0644); utimensat(AT_FDCWD, p, t_set, 0);
  snprintf(p, sizeof p, "%s/sticky", dir); mkdir(p, 0755); chmod(p, 01777);
  snprintf(p, sizeof p, "%s/fifo", dir); mkfifo(p, 0640);
  snprintf(p, sizeof p, "%s/empty-dir", dir); mkdir(p, 0700);
  snprintf(p, sizeof p, "%s/renamed-src", dir); write_file(p, 13, 50, 0644);
  snprintf(q, sizeof q, "%s/renamed", dir); rename(p, q);
  snprintf(p, sizeof p, "%s/deleted", dir); write_file(p, 14, 50, 0644); unlink(p);
  double t0 = (double)clock() / CLOCKS_PER_SEC;
  sync();
  printf("persisttest: filled %s (%d small files, %d MiB big file); sync took %.0f ms\n", dir, nsmall, bigmb, ((double)clock() / CLOCKS_PER_SEC - t0) * 1000);
  return 0;
}
static int verify(const char *dir, int nsmall, int bigmb) {
  char p[512], q[512], d[128];
  struct stat st, s2;
  for (unsigned k = 0; k < NSIZES; k++) {
    snprintf(p, sizeof p, "%s/size-%u", dir, sizes[k]);
    snprintf(d, sizeof d, "(%u bytes)", sizes[k]);
    check("file of each size, every byte", stat(p, &st) == 0 && st.st_size == sizes[k] && (st.st_mode & 07777) == 0644 && file_ok(p, 1000 + k, 0), d);
  }
  if (bigmb) { snprintf(p, sizeof p, "%s/big", dir); check("the big file, every byte", stat(p, &st) == 0 && st.st_size == (long long)bigmb << 20 && file_ok(p, 7, 0), 0); }
  snprintf(p, sizeof p, "%s/deep", dir);
  int ok = 1;
  for (int k = 0; k < 30; k++) { ok &= stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0750; strcat(p, "/d"); }
  strcat(p, "-leaf");
  check("30 nested directories and the file at the bottom", ok && stat(p, &st) == 0 && (st.st_mode & 0777) == 0600 && file_ok(p, 42, 0) && st.st_size == 1234, 0);
  int bad = 0;
  for (int k = 0; k < nsmall; k++) {
    snprintf(p, sizeof p, "%s/many/f%05d", dir, k);
    if (stat(p, &st) || st.st_size != 16 + k % 200 || !file_ok(p, 100000 + k, 0)) { if (!bad) printf("  first bad small file: %s\n", p); bad++; }
  }
  snprintf(d, sizeof d, "(%d of %d bad)", bad, nsmall);
  check("every small file, every byte", bad == 0, d);
  int n = 0;
  snprintf(p, sizeof p, "%s/many", dir);
  DIR *dd = opendir(p);
  struct dirent *e;
  while (dd && (e = readdir(dd))) n++;
  if (dd) closedir(dd);
  check("the big directory lists exactly its entries", n == nsmall + 3, 0);
  snprintf(p, sizeof p, "%s/size-100", dir); snprintf(q, sizeof q, "%s/hard", dir);
  check("hard links: one inode, link count 3", stat(p, &st) == 0 && stat(q, &s2) == 0 && st.st_ino == s2.st_ino && st.st_nlink == 3, 0);
  snprintf(p, sizeof p, "%s/rel", dir);
  long l = readlink(p, (char *)buf, 100);
  check("relative symlink", l == 8 && !memcmp(buf, "size-100", 8) && file_ok(p, 1000 + 3, 0), 0);
  snprintf(p, sizeof p, "%s/abs", dir); snprintf(q, sizeof q, "%s/deep/d", dir);
  l = readlink(p, (char *)buf, 500);
  check("absolute symlink to a directory", l == (long)strlen(q) && !memcmp(buf, q, l) && stat(p, &st) == 0 && S_ISDIR(st.st_mode), 0);
  snprintf(p, sizeof p, "%s/dangling", dir);
  check("dangling symlink", lstat(p, &st) == 0 && S_ISLNK(st.st_mode) && stat(p, &st) != 0, 0);
  snprintf(p, sizeof p, "%s/suid", dir);
  check("set-user-ID mode 4755", stat(p, &st) == 0 && (st.st_mode & 07777) == 04755, 0);
  snprintf(p, sizeof p, "%s/none", dir);
  check("mode 0000", stat(p, &st) == 0 && (st.st_mode & 07777) == 0, 0);
  snprintf(p, sizeof p, "%s/owned", dir);
  /* only root can give a file away: run as the desktop user, the file keeps its owner */
  unsigned wu = geteuid() == 0 ? 123 : geteuid(), wg = geteuid() == 0 ? 45 : getegid();
  check(geteuid() == 0 ? "owner 123, group 45" : "owner and group stay the caller's (chown to 123:45 needs root)", stat(p, &st) == 0 && st.st_uid == wu && st.st_gid == wg, 0);
  snprintf(p, sizeof p, "%s/timed", dir);
  struct statx sx;
  check("times to the nanosecond", statx(AT_FDCWD, p, 0, STATX_BASIC_STATS, &sx) == 0 && sx.stx_atime.tv_sec == 1100000000 && sx.stx_atime.tv_nsec == 123456789 &&
        sx.stx_mtime.tv_sec == 1200000000 && sx.stx_mtime.tv_nsec == 987654321, 0);
  snprintf(p, sizeof p, "%s/sticky", dir);
  check("sticky directory 1777", stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 01777, 0);
  snprintf(p, sizeof p, "%s/fifo", dir);
  check("FIFO node", lstat(p, &st) == 0 && S_ISFIFO(st.st_mode) && (st.st_mode & 0777) == 0640, 0);
  snprintf(p, sizeof p, "%s/empty-dir", dir);
  check("empty directory 0700", stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700, 0);
  snprintf(p, sizeof p, "%s/renamed", dir); snprintf(q, sizeof q, "%s/renamed-src", dir);
  check("rename kept", file_ok(p, 13, 0) && stat(q, &st) != 0, 0);
  snprintf(p, sizeof p, "%s/deleted", dir);
  check("unlink kept", stat(p, &st) != 0, 0);
  printf("persisttest verify: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}

/* ---- the crash workload and its model ---- */
#define NNAMES 24
#define NDIRS 3
enum { T_NONE, T_FILE, T_LINK, T_DIR };
struct node { int type; unsigned id, size, mode; int target; };
struct model { struct node n[64]; int nn; int ent[NDIRS + 1][NNAMES]; int dir_of[NDIRS + 1]; };
static unsigned long long rs;
static unsigned rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return (unsigned)((rs * 2685821657736338717ull) >> 32); }
static void ename(char *out, int cap, const char *base, int d, int k) {
  if (d == 0) snprintf(out, cap, "%s/n%02d", base, k); else snprintf(out, cap, "%s/sub%d/n%02d", base, d, k);
}
static int newnode(struct model *m, int type) { for (int k = 1; k < 64; k++) if (m->n[k].type == T_NONE) { memset(&m->n[k], 0, sizeof m->n[k]); m->n[k].type = type; return k; } return 0; }
static int refs(struct model *m, int node) { int c = 0; for (int d = 0; d <= NDIRS; d++) for (int k = 0; k < NNAMES; k++) c += m->ent[d][k] == node; return c; }
static void drop(struct model *m, int d, int k) { int nd = m->ent[d][k]; m->ent[d][k] = 0; if (nd && !refs(m, nd)) m->n[nd].type = T_NONE; }
/* One step: pick a change that is valid in the model, do it on the real tree if real, and
 * apply it to the model. Every change is a single system call (a new file is written in
 * /tmp, which is not stored, and renamed into place). */
static unsigned next_id = 1;
static int step(struct model *m, const char *base, int real) {
  char a[300], b[300];
  for (int tries = 0; tries < 50; tries++) {
    int d = rnd() % (NDIRS + 1), k = rnd() % NNAMES, cur = m->ent[d][k];
    switch (rnd() % 9) {
    case 0: case 1: { /* new file, or replace a file */
      if (cur && m->n[cur].type == T_DIR) continue;
      unsigned id = next_id++, size = 4 + rnd() % 3000;
      int nd = newnode(m, T_FILE);
      if (!nd) continue;
      m->n[nd].id = id; m->n[nd].size = size; m->n[nd].mode = 0644;
      ename(a, sizeof a, base, d, k);
      if (real && (write_file("/tmp/pt-new", id, size, 0644) || rename("/tmp/pt-new", a))) return -1;
      drop(m, d, k); m->ent[d][k] = nd;
      return 0;
    }
    case 2: /* append */
      if (!cur || m->n[cur].type != T_FILE) continue;
      if (real) {
        ename(a, sizeof a, base, d, k);
        unsigned n = 1 + rnd() % 2000, off = m->n[cur].size;
        int fd = open(a, O_WRONLY | O_APPEND);
        fillpat(buf, m->n[cur].id, off, n);
        if (fd < 0 || write(fd, buf, n) != (long)n) return -1;
        close(fd);
        m->n[cur].size += n;
      } else m->n[cur].size += 1 + rnd() % 2000;
      return 0;
    case 3: { /* rename within or across directories, maybe over a file */
      int d2 = rnd() % (NDIRS + 1), k2 = rnd() % NNAMES, dst = m->ent[d2][k2];
      if (!cur || m->n[cur].type == T_DIR || (d2 == d && k2 == k) || (dst && m->n[dst].type == T_DIR) || dst == cur) continue;
      ename(a, sizeof a, base, d, k); ename(b, sizeof b, base, d2, k2);
      if (real && rename(a, b)) return -1;
      drop(m, d2, k2); m->ent[d2][k2] = cur; m->ent[d][k] = 0;
      return 0;
    }
    case 4: /* unlink */
      if (!cur || m->n[cur].type == T_DIR) continue;
      ename(a, sizeof a, base, d, k);
      if (real && unlink(a)) return -1;
      drop(m, d, k);
      return 0;
    case 5: { /* hard link */
      int d2 = rnd() % (NDIRS + 1), k2 = rnd() % NNAMES;
      if (!cur || m->n[cur].type != T_FILE || m->ent[d2][k2]) continue;
      ename(a, sizeof a, base, d, k); ename(b, sizeof b, base, d2, k2);
      if (real && link(a, b)) return -1;
      m->ent[d2][k2] = cur;
      return 0;
    }
    case 6: { /* symbolic link to another name */
      if (cur) continue;
      int nd = newnode(m, T_LINK);
      if (!nd) continue;
      m->n[nd].target = rnd() % NNAMES; m->n[nd].mode = 0777;
      snprintf(b, sizeof b, "n%02d", m->n[nd].target);
      ename(a, sizeof a, base, d, k);
      if (real && symlink(b, a)) return -1;
      m->ent[d][k] = nd;
      return 0;
    }
    case 7: /* chmod */
      if (!cur || m->n[cur].type != T_FILE) continue;
      { unsigned mode = 0400 | (rnd() & 0377);
        ename(a, sizeof a, base, d, k);
        if (real && chmod(a, mode)) return -1;
        m->n[cur].mode = mode; }
      return 0;
    case 8: /* truncate */
      if (!cur || m->n[cur].type != T_FILE) continue;
      { unsigned size = 4 + rnd() % (m->n[cur].size - 3);
        ename(a, sizeof a, base, d, k);
        if (real && truncate(a, size)) return -1;
        m->n[cur].size = size; }
      return 0;
    }
  }
  return 0; /* nothing applicable: a no-op step */
}
static void model_init(struct model *m, const char *base, int real) {
  memset(m, 0, sizeof *m);
  char a[300];
  if (real) { mkdir(base, 0755); for (int d = 1; d <= NDIRS; d++) { snprintf(a, sizeof a, "%s/sub%d", base, d); mkdir(a, 0755); } }
}
/* does the real tree equal the model? */
static int same(struct model *m, const char *base, char *why, int cap) {
  char a[300], t[64];
  struct stat st;
  for (int d = 0; d <= NDIRS; d++) {
    int count = 0;
    if (d == 0) snprintf(a, sizeof a, "%s", base); else snprintf(a, sizeof a, "%s/sub%d", base, d);
    DIR *dd = opendir(a);
    struct dirent *e;
    while (dd && (e = readdir(dd))) if (e->d_name[0] == 'n') count++;
    if (dd) closedir(dd);
    int want = 0;
    for (int k = 0; k < NNAMES; k++) want += m->ent[d][k] != 0;
    if (count != want) { snprintf(why, cap, "dir %d has %d names, model %d", d, count, want); return 0; }
    for (int k = 0; k < NNAMES; k++) {
      int nd = m->ent[d][k];
      if (!nd) continue;
      struct node *n = &m->n[nd];
      ename(a, sizeof a, base, d, k);
      if (lstat(a, &st)) { snprintf(why, cap, "%s missing", a); return 0; }
      if (n->type == T_LINK) {
        long l = readlink(a, t, sizeof t);
        if (!S_ISLNK(st.st_mode) || l != 3 || t[1] != '0' + n->target / 10 || t[2] != '0' + n->target % 10) { snprintf(why, cap, "%s: link differs", a); return 0; }
        continue;
      }
      unsigned id;
      if (!S_ISREG(st.st_mode) || st.st_size != n->size || (st.st_mode & 07777) != n->mode || st.st_nlink != (unsigned)refs(m, nd)) {
        snprintf(why, cap, "%s: size %lld mode %o nlink %u, model %u %o %d", a, st.st_size, st.st_mode & 07777, st.st_nlink, n->size, n->mode, refs(m, nd));
        return 0;
      }
      if (!file_ok(a, -1, &id) || id != n->id) { snprintf(why, cap, "%s: contents differ", a); return 0; }
    }
  }
  return 1;
}
static int crash_run(unsigned long long seed, int nops, const char *base) {
  static struct model m;
  rs = seed * 0x9e3779b97f4a7c15ull | 1;
  model_init(&m, base, 1);
  sync();
  printf("synced 0\n");
  for (int i = 1; i <= nops; i++) {
    if (step(&m, base, 1)) { printf("persisttest: change %d failed: %s\n", i, strerror(errno)); return 1; }
    if (i % 20 == 0) { sync(); printf("synced %d\n", i); fflush(stdout); }
    if (i == 30 || i == 110) usleep(700000); /* longer than the store's write-behind delay: a drain nobody waits for */
  }
  printf("persisttest crash-run: done\n");
  return 0;
}
static int crash_check(unsigned long long seed, int nops, const char *base, int k) {
  static struct model m;
  char why[300] = "", first[300] = "";
  rs = seed * 0x9e3779b97f4a7c15ull | 1;
  model_init(&m, base, 0);
  int match = -1;
  for (int i = 0; i <= nops; i++) {
    if (i) step(&m, base, 0);
    if (i < k) continue;
    if (same(&m, base, why, sizeof why)) { match = i; break; }
    if (i == k) strcpy(first, why);
  }
  /* every file must be consistent on its own as well */
  int bad = 0;
  char a[300];
  for (int d = 0; d <= NDIRS; d++) for (int j = 0; j < NNAMES; j++) {
    struct stat st;
    ename(a, sizeof a, base, d, j);
    if (lstat(a, &st) == 0 && S_ISREG(st.st_mode) && !file_ok(a, -1, 0)) bad++;
  }
  if (match >= 0 && !bad) printf("persisttest crash-check: PASS, the tree is the state after change %d (synced: %d)\n", match, k);
  else printf("persisttest crash-check: FAIL, no state at or after change %d matches (%s); %d inconsistent files\n", k, first, bad);
  return match < 0 || bad;
}

/* ---- one big file written while the machine is stopped ---- */
static int bigwrite(const char *path, int mb) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return 1;
  for (int k = 0; k < mb; k++) {
    fillpat(buf, 77, (unsigned)k << 20, 1 << 20);
    if (write(fd, buf, 1 << 20) != 1 << 20) return 1;
    printf("wrote %d MiB\n", k + 1);
    fflush(stdout);
    if (k % 8 == 7) usleep(1000); /* let the store drain in between */
  }
  fsync(fd);
  close(fd);
  printf("bigwrite: synced %d MiB\n", mb);
  return 0;
}
static int bigcheck(const char *path) {
  struct stat st;
  if (stat(path, &st)) { printf("bigcheck: no file (it was never stored): PASS\n"); return 0; }
  unsigned id;
  int ok = st.st_size == 0 || file_ok(path, -1, &id);
  printf("bigcheck: %lld bytes, %s\n", st.st_size, ok ? "consistent: PASS" : "contents do not match the size: FAIL");
  return !ok;
}

/* ---- a full store (run with tools/run.mjs --quota=BYTES; tools/tests/store-full.mjs) ---- */
#include <poll.h>
#include <sys/statfs.h>
static void store_field(const char *key, char *out, int cap) {
  char t[2048];
  out[0] = 0;
  int fd = open("/proc/ucos-store", O_RDONLY);
  if (fd < 0) return;
  long n = read(fd, t, sizeof t - 1);
  close(fd);
  t[n > 0 ? n : 0] = 0;
  char *p = strstr(t, key);
  if (!p) return;
  p += strlen(key);
  while (*p == ' ') p++;
  int k = 0;
  while (*p && *p != '\n' && k < cap - 1) out[k++] = *p++;
  out[k] = 0;
}
/* write files of 256 KiB into DIR until the store says no; then free room and go on */
static int full(const char *dir) {
  char path[512], v[128];
  struct statfs f;
  mkdir(dir, 0755);
  int ok = statfs("/", &f) == 0;
  printf("statfs /: %llu blocks of %u, %llu free\n", f.f_blocks, f.f_bsize, f.f_bfree);
  check("statfs reports the store's quota as the size of /", ok && f.f_blocks > 0, NULL);
  int sub = open("/proc/ucos-store", O_RDONLY); /* a subscriber to level changes */
  check("/proc/ucos-store opens for a subscriber", sub >= 0, NULL);
  snprintf(path, sizeof path, "%s/kept", dir);
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  fillpat(buf, 7, 0, 65536);
  check("a first file is written and fsync'ed", fd >= 0 && write(fd, buf, 65536) == 65536 && fsync(fd) == 0 && close(fd) == 0, NULL);
  int n = 0, err = 0, where = 0;
  for (; n < 4000; n++) {
    snprintf(path, sizeof path, "%s/f%d", dir, n);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { err = errno; where = 1; break; }
    fillpat(buf, 100 + n, 0, 262144);
    long w = write(fd, buf, 262144);
    if (w != 262144) { err = errno; where = 2; close(fd); break; }
    if (fsync(fd)) { err = errno; where = 3; close(fd); break; }
    close(fd);
  }
  static const char *wh[] = {"", "open", "write", "fsync"};
  printf("full after %d files of 256 KiB: %s failed with %s\n", n, wh[where], strerror(err));
  check("writes to the stored tree end with ENOSPC", where && err == ENOSPC, strerror(err));
  struct pollfd pf = {sub, POLLPRI, 0};
  check("a subscriber sees the level change (POLLPRI on /proc/ucos-store)", poll(&pf, 1, 0) == 1 && (pf.revents & POLLPRI), NULL);
  store_field("level:", v, sizeof v);
  printf("level: %s\n", v);
  check("/proc/ucos-store says critical or full", !strcmp(v, "full") || !strcmp(v, "critical"), v);
  check("statfs shows (nearly) no room left", statfs("/", &f) == 0 && f.f_bfree * f.f_bsize < 262144 + 65536, NULL);
  fd = open("/tmp/never-stored", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  check("writes to /tmp (never stored) still work", fd >= 0 && write(fd, buf, 262144) == 262144 && close(fd) == 0, NULL);
  unlink("/tmp/never-stored");
  /* free room: delete what was written after "kept" */
  for (int k = 0; k <= n; k++) { snprintf(path, sizeof path, "%s/f%d", dir, k); unlink(path); }
  int back = 0;
  for (int t = 0; t < 100 && !back; t++) { /* the deletions are stored, the browser reports the new figures */
    sync();
    usleep(50000);
    snprintf(path, sizeof path, "%s/again", dir);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { fillpat(buf, 9, 0, 65536); back = write(fd, buf, 65536) == 65536 && fsync(fd) == 0; close(fd); }
    if (!back) unlink(path);
  }
  store_field("state:", v, sizeof v);
  printf("after deleting: state %s\n", v);
  check("deleting files makes room: a new file is written and fsync'ed", back, v);
  close(sub);
  printf("persisttest: full %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
static int fullcheck(const char *dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/kept", dir);
  check("after a reboot, the file fsync'ed before the store filled is intact", file_ok(path, 7, NULL), NULL);
  snprintf(path, sizeof path, "%s/again", dir);
  check("...and so is the file written after room was made", file_ok(path, 9, NULL), NULL);
  snprintf(path, sizeof path, "%s/f0", dir);
  struct stat st;
  check("...and the deleted files stay deleted", stat(path, &st) == -1 && errno == ENOENT, NULL);
  printf("persisttest: fullcheck %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "fill")) return fill(argv[2], argc > 3 ? atoi(argv[3]) : 20000, argc > 4 ? atoi(argv[4]) : 50);
  if (argc >= 3 && !strcmp(argv[1], "verify")) return verify(argv[2], argc > 3 ? atoi(argv[3]) : 20000, argc > 4 ? atoi(argv[4]) : 50);
  if (argc >= 5 && !strcmp(argv[1], "crash-run")) return crash_run(strtoull(argv[2], 0, 10), atoi(argv[3]), argv[4]);
  if (argc >= 6 && !strcmp(argv[1], "crash-check")) return crash_check(strtoull(argv[2], 0, 10), atoi(argv[3]), argv[4], atoi(argv[5]));
  if (argc >= 4 && !strcmp(argv[1], "bigwrite")) return bigwrite(argv[2], atoi(argv[3]));
  if (argc >= 3 && !strcmp(argv[1], "bigcheck")) return bigcheck(argv[2]);
  if (argc >= 3 && !strcmp(argv[1], "full")) return full(argv[2]);
  if (argc >= 3 && !strcmp(argv[1], "fullcheck")) return fullcheck(argv[2]);
  fprintf(stderr, "usage: persisttest fill|verify DIR [NSMALL] [BIGMB] | crash-run SEED NOPS DIR | crash-check SEED NOPS DIR K | bigwrite FILE MB | bigcheck FILE\n");
  return 2;
}
