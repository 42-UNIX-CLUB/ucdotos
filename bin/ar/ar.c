// SPDX-License-Identifier: GPL-2.0-or-later
/* ar: create, list, print, extract, replace and delete members of an ar
 * archive (the outer layer of a .deb). GNU spellings:
 *   ar t[v] archive [member...]      list
 *   ar p[v] archive [member...]      print member data
 *   ar x[vo] archive [member...]     extract into the current directory
 *   ar r[cuv] archive file...        insert or replace (creates the archive)
 *   ar q[cv] archive file...         append
 *   ar d[v] archive member...        delete
 * Modifiers: c (no "creating" note), v (verbose), o (keep times on extract),
 * D (zero times, owners and modes 644 in new members), U (real values; the
 * default), s and S (symbol index: accepted, nothing to do). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "arc.h"

static int verbose, quiet_create, keep_time, determ;
static char op;
static const char *archive;
static char **names;
static int nnames;
static char *found;   /* found[i]: names[i] matched a member */

static long fd_src(void *ctx, void *buf, unsigned long n) { return read((int)(long)ctx, buf, n); }
static long fd_sink(void *ctx, const void *buf, unsigned long n) { return write((int)(long)ctx, buf, n); }

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static int wanted(const char *member) {
  if (!nnames) return 1;
  for (int i = 0; i < nnames; i++)
    if (!found[i] && !strcmp(base_name(names[i]), member)) { found[i] = 1; return 1; }
  return 0;
}

static int write_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) {
    long r = write(fd, c, n);
    if (r <= 0) return -1;
    c += r;
    n -= r;
  }
  return 0;
}

/* Moves exactly n bytes from the archive to fd (or nowhere if fd < 0). */
static int pass(int in, int out, unsigned long long n) {
  static char buf[65536];
  while (n) {
    long k = read(in, buf, n < sizeof buf ? (size_t)n : sizeof buf);
    if (k <= 0) return -1;
    if (out >= 0 && write_all(out, buf, k) < 0) return -2;
    n -= k;
  }
  return 0;
}

static void mode_string(unsigned m, char *s) {
  const char *x = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s[i] = m & (0400 >> i) ? x[i] : '-';
  s[9] = 0;
}

static int read_ops(void) {
  int fd = open(archive, O_RDONLY);
  if (fd < 0) { fprintf(stderr, "ar: %s: %s\n", archive, strerror(errno)); return 1; }
  void *ctx = (void *)(long)fd;
  if (ar_begin(fd_src, ctx) < 0) { fprintf(stderr, "ar: %s: file format not recognized\n", archive); close(fd); return 1; }
  ar_member m;
  int r, rc = 0;
  while ((r = ar_next(fd_src, ctx, &m)) == 1) {
    int out = -1, want = wanted(m.name);
    if (want && op == 't') {
      if (verbose) {
        char ms[10], ts[32];
        time_t t = m.mtime;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%b %e %H:%M %Y", &tm);
        mode_string(m.mode, ms);
        printf("%s %u/%u %6llu %s %s\n", ms, m.uid, m.gid, m.size64, ts, m.name);
      } else printf("%s\n", m.name);
    } else if (want && op == 'p') {
      if (verbose) { printf("\n<%s>\n\n", m.name); }
      fflush(stdout);
      out = 1;
    } else if (want && op == 'x') {
      if (verbose) printf("x - %s\n", m.name);
      out = open(m.name, O_WRONLY | O_CREAT | O_TRUNC, (m.mode & 0777) ? m.mode & 0777 : 0644);
      if (out < 0) { fprintf(stderr, "ar: %s: %s\n", m.name, strerror(errno)); rc = 1; }
    }
    int pr = pass(fd, out, m.size64);
    if (op == 'x' && out >= 0) {
      close(out);
      if (keep_time) {
        struct timespec ts[2] = {{m.mtime, 0}, {m.mtime, 0}};
        utimensat(AT_FDCWD, m.name, ts, 0);
      }
    }
    if (pr == -1) { fprintf(stderr, "ar: %s: truncated archive\n", archive); close(fd); return 1; }
    if (pr == -2) { fprintf(stderr, "ar: write error: %s\n", strerror(errno)); close(fd); return 1; }
    if (ar_skip_pad(fd_src, ctx, &m) < 0) { fprintf(stderr, "ar: %s: truncated archive\n", archive); close(fd); return 1; }
  }
  close(fd);
  fflush(stdout);
  if (r < 0) { fprintf(stderr, "ar: %s: malformed archive\n", archive); return 1; }
  for (int i = 0; i < nnames; i++)
    if (!found[i]) { fprintf(stderr, "no entry %s in archive\n", names[i]); rc = 1; }
  return rc;
}

static int add_file(int out, const char *path) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) < 0) { fprintf(stderr, "ar: %s: %s\n", path, strerror(errno)); if (fd >= 0) close(fd); return -1; }
  ar_member m;
  memset(&m, 0, sizeof m);
  const char *b = base_name(path);
  if (strlen(b) >= sizeof m.name) { fprintf(stderr, "ar: %s: name too long\n", path); close(fd); return -1; }
  strcpy(m.name, b);
  m.size64 = st.st_size;
  m.size = (unsigned long)st.st_size;
  if (determ) m.mode = 0100644;
  else { m.mode = st.st_mode; m.uid = st.st_uid; m.gid = st.st_gid; m.mtime = st.st_mtime; }
  void *ctx = (void *)(long)out;
  if (ar_write_header(fd_sink, ctx, &m) < 0) { fprintf(stderr, "ar: write error\n"); close(fd); return -1; }
  int r = pass(fd, out, m.size64);
  close(fd);
  if (r < 0) { fprintf(stderr, "ar: %s: %s\n", path, r == -1 ? "file changed while reading" : "write error"); return -1; }
  if (ar_write_pad(fd_sink, ctx, &m) < 0) return -1;
  return 0;
}

static int write_ops(void) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s.tmp%d", archive, (int)getpid());
  int in = open(archive, O_RDONLY);
  if (in < 0 && errno != ENOENT) { fprintf(stderr, "ar: %s: %s\n", archive, strerror(errno)); return 1; }
  if (in < 0 && op == 'd') { fprintf(stderr, "ar: %s: %s\n", archive, strerror(errno)); return 1; }
  if (in < 0 && !quiet_create) fprintf(stderr, "ar: creating %s\n", archive);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { fprintf(stderr, "ar: %s: %s\n", tmp, strerror(errno)); if (in >= 0) close(in); return 1; }
  void *octx = (void *)(long)out;
  int rc = 0;
  if (ar_write_begin(fd_sink, octx) < 0) rc = 1;
  if (in >= 0 && !rc) {
    void *ictx = (void *)(long)in;
    if (ar_begin(fd_src, ictx) < 0) { fprintf(stderr, "ar: %s: file format not recognized\n", archive); rc = 1; }
    ar_member m;
    int r = 0;
    while (!rc && (r = ar_next(fd_src, ictx, &m)) == 1) {
      int hit = -1;
      if (op != 'q')
        for (int i = 0; i < nnames; i++)
          if (!found[i] && !strcmp(base_name(names[i]), m.name)) { hit = i; break; }
      if (hit >= 0) {
        found[hit] = 1;
        if (pass(in, -1, m.size64) < 0) rc = 1;
        if (op == 'd') { if (verbose) printf("d - %s\n", m.name); }
        else { if (verbose) printf("r - %s\n", m.name); if (add_file(out, names[hit]) < 0) rc = 1; }
      } else {
        if (ar_write_header(fd_sink, octx, &m) < 0 || pass(in, out, m.size64) < 0 || ar_write_pad(fd_sink, octx, &m) < 0) rc = 1;
      }
      if (ar_skip_pad(fd_src, ictx, &m) < 0) rc = 1;
    }
    if (r < 0) { fprintf(stderr, "ar: %s: malformed archive\n", archive); rc = 1; }
    close(in);
  }
  for (int i = 0; i < nnames && !rc; i++) {
    if (found[i]) continue;
    if (op == 'd') { fprintf(stderr, "No member named '%s'\n", names[i]); continue; }
    if (verbose) printf("a - %s\n", names[i]);
    if (add_file(out, names[i]) < 0) rc = 1;
  }
  if (close(out) < 0) rc = 1;
  if (rc) { unlink(tmp); return 1; }
  if (rename(tmp, archive) < 0) { fprintf(stderr, "ar: %s: %s\n", archive, strerror(errno)); unlink(tmp); return 1; }
  return 0;
}

static void usage(void) {
  fprintf(stderr, "usage: ar {t|p|x|r|q|d}[cvoDUsS] archive [member...]\n");
}

int main(int argc, char **argv) {
  if (argc < 3) { usage(); return 1; }
  const char *k = argv[1];
  if (*k == '-') k++;
  for (; *k; k++) {
    switch (*k) {
    case 't': case 'p': case 'x': case 'r': case 'q': case 'd':
      if (op) { fprintf(stderr, "ar: two different operation options specified\n"); return 1; }
      op = *k;
      break;
    case 'v': verbose = 1; break;
    case 'c': quiet_create = 1; break;
    case 'o': keep_time = 1; break;
    case 'D': determ = 1; break;
    case 'U': determ = 0; break;
    case 's': case 'S': case 'u': case 'f': case 'P': break;
    default: fprintf(stderr, "ar: invalid option -- '%c'\n", *k); usage(); return 1;
    }
  }
  if (!op) {
    if (strchr(argv[1], 's')) return 0; /* "ar s lib.a": build a symbol index; nothing to do here */
    usage();
    return 1;
  }
  archive = argv[2];
  names = argv + 3;
  nnames = argc - 3;
  found = calloc(nnames + 1, 1);
  if ((op == 'r' || op == 'q' || op == 'd') && !nnames && op != 'r') { usage(); return 1; }
  if (op == 't' || op == 'p' || op == 'x') return read_ops();
  return write_ops();
}
