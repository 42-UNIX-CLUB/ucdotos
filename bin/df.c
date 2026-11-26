// SPDX-License-Identifier: GPL-2.0-or-later
/* df [-hkPT] [file...]: free space on the file systems holding the files
 * (default: /). Numbers come from statfs; the file system name and mount
 * point from /proc/mounts when it exists. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __wasm__
#include <sys/syscall.h>
/* Linux's statfs64 layout for 32-bit systems, as the kernel fills it */
struct uc_statfs64 {
  unsigned f_type, f_bsize;
  unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int f_fsid[2];
  unsigned f_namelen, f_frsize, f_flags, f_spare[4];
};
#else
#include <sys/statfs.h>
#endif

static int human, posix, show_type;
static long long unit = 1024;

struct fsinfo { unsigned long long blocks, bfree, bavail, bsize; unsigned type; };
static int get_fs(const char *path, struct fsinfo *fi) {
#ifdef __wasm__
  struct uc_statfs64 s;
  long r = __syscall2(43, path, &s); /* statfs(path, buf), as the kernel's vfs work implements it */
  if (r < 0) { errno = -r; return -1; }
#else
  struct statfs s;
  if (statfs(path, &s) != 0) return -1;
#endif
  fi->blocks = s.f_blocks; fi->bfree = s.f_bfree; fi->bavail = s.f_bavail;
  fi->bsize = s.f_frsize ? s.f_frsize : s.f_bsize;
  fi->type = s.f_type;
  return 0;
}

/* the /proc/mounts line whose mount point is the longest prefix of path */
static void find_mount(const char *path, char *dev, char *mnt, char *type, size_t n) {
  snprintf(dev, n, "-"); snprintf(mnt, n, "/"); snprintf(type, n, "-");
  char real[4096];
  if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) { snprintf(dev, n, "rootfs"); return; }
  char line[1024];
  size_t best = 0;
  while (fgets(line, sizeof line, f)) {
    char d[512], m[512], t[64];
    if (sscanf(line, "%511s %511s %63s", d, m, t) != 3) continue;
    size_t ml = strlen(m);
    int match = !strcmp(m, "/") || (!strncmp(real, m, ml) && (real[ml] == '/' || !real[ml]));
    if (match && (ml >= best)) { best = ml; snprintf(dev, n, "%s", d); snprintf(mnt, n, "%s", m); snprintf(type, n, "%s", t); }
  }
  fclose(f);
}

static void human_str(unsigned long long bytes, char *buf, size_t n) {
  static const char units[] = "KMGTPE";
  if (bytes < 1024) { snprintf(buf, n, "%llu", bytes); return; }
  double v = bytes;
  int k = -1;
  while (v >= 1024 && k < 5) { v /= 1024; k++; }
  if (v < 10) { long long t = (long long)(v * 10); if (t < v * 10) t++; if (t >= 100) snprintf(buf, n, "%lld%c", t / 10, units[k]); else snprintf(buf, n, "%lld.%lld%c", t / 10, t % 10, units[k]); }
  else { long long t = (long long)v; if (t < v) t++; snprintf(buf, n, "%lld%c", t, units[k]); }
}

struct row { char c[7][512]; };
static struct row *rows;
static int nrows;
static void add_row(const char *a, const char *b, const char *c, const char *d, const char *e, const char *f, const char *g) {
  rows = realloc(rows, (nrows + 1) * sizeof *rows);
  const char *v[7] = {a, b, c, d, e, f, g};
  for (int k = 0; k < 7; k++) snprintf(rows[nrows].c[k], sizeof rows[nrows].c[k], "%s", v[k]);
  nrows++;
}
static int do_one(const char *path, const char *dev_in, const char *mnt_in, const char *type_in, int quiet) {
  struct fsinfo fi;
  struct stat st;
  if (stat(path, &st) != 0) { if (!quiet) fprintf(stderr, "df: %s: %s\n", path, strerror(errno)); return 1; }
  if (get_fs(path, &fi) != 0) { if (!quiet) fprintf(stderr, "df: %s: cannot read file system information: %s\n", path, strerror(errno)); return 1; }
  if (quiet && fi.blocks == 0) return 0; /* pseudo file systems, as GNU df hides them */
  char dev[512], mnt[512], type[64];
  if (dev_in) { snprintf(dev, sizeof dev, "%s", dev_in); snprintf(mnt, sizeof mnt, "%s", mnt_in); snprintf(type, sizeof type, "%s", type_in); }
  else find_mount(path, dev, mnt, type, sizeof dev);
  unsigned long long total = fi.blocks * fi.bsize, freeb = fi.bfree * fi.bsize, avail = fi.bavail * fi.bsize, used = total - freeb;
  unsigned long long denom = used + avail;
  char pcts[8], a[32], b[32], c[32];
  if (denom) snprintf(pcts, sizeof pcts, "%d%%", (int)((used * 100 + denom - 1) / denom)); else snprintf(pcts, sizeof pcts, "-");
  if (human) { human_str(total, a, sizeof a); human_str(used, b, sizeof b); human_str(avail, c, sizeof c); }
  else {
    snprintf(a, sizeof a, "%llu", (total + unit - 1) / unit);
    snprintf(b, sizeof b, "%llu", (used + unit - 1) / unit);
    snprintf(c, sizeof c, "%llu", avail / unit);
  }
  add_row(dev, type, a, b, c, pcts, mnt);
  return 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--human-readable")) { human = 1; continue; }
    if (!strcmp(a, "--portability")) { posix = 1; continue; }
    if (!strcmp(a, "--print-type")) { show_type = 1; continue; }
    if (a[1] == '-') { fprintf(stderr, "df: unrecognized option '%s'\n", a); return 1; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'h') human = 1;
      else if (*f == 'k') unit = 1024, human = 0;
      else if (*f == 'm') unit = 1024 * 1024, human = 0;
      else if (*f == 'P') posix = 1;
      else if (*f == 'T') show_type = 1;
      else if (*f == 'a' || *f == 'l') ;
      else { fprintf(stderr, "df: invalid option -- '%c'\nusage: df [-hkPT] [file...]\n", *f); return 1; }
    }
  }
  add_row("Filesystem", "Type", human ? "Size" : unit == 1024 ? (posix ? "1024-blocks" : "1K-blocks") : "1M-blocks",
          "Used", human ? "Avail" : "Available", posix ? "Capacity" : "Use%", "Mounted on");
  if (i < argc) for (; i < argc; i++) rc |= do_one(argv[i], NULL, NULL, NULL, 0);
  else {
    /* every mounted file system, or / when the kernel has no mount table */
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    int any = 0;
    while (f && fgets(line, sizeof line, f)) {
      char d[512], m[512], t[64];
      if (sscanf(line, "%511s %511s %63s", d, m, t) != 3) continue;
      int dup = 0;
      for (int k = 1; k < nrows; k++) if (!strcmp(rows[k].c[6], m)) dup = 1;
      if (!dup) { do_one(m, d, m, t, 1); any = 1; }
    }
    if (f) fclose(f);
    if (!any) rc |= do_one("/", NULL, NULL, NULL, 0);
  }
  if (nrows > 1) {
    int w[7] = {0};
    if (human) w[2] = w[3] = w[4] = 5; /* GNU's minimum for human-readable sizes */
    for (int r = 0; r < nrows; r++) for (int k = 0; k < 7; k++) { int l = strlen(rows[r].c[k]); if (l > w[k]) w[k] = l; }
    for (int r = 0; r < nrows; r++) {
      printf("%-*s", w[0], rows[r].c[0]);
      if (show_type) printf(" %-*s", w[1], rows[r].c[1]);
      for (int k = 2; k < 6; k++) printf(" %*s", w[k], rows[r].c[k]);
      printf(" %s\n", rows[r].c[6]);
    }
  }
  if (fflush(stdout)) return 1;
  return rc;
}
