// SPDX-License-Identifier: GPL-2.0-or-later
/* du [-abchkmsxLPH0] [-d N] [--apparent-size] [file...]: disk usage.
 * Sizes come from stat: st_blocks (512-byte units) by default, st_size with
 * --apparent-size or -b (a directory itself then counts 0, as in GNU du). Shown in 1024-byte units unless -b, -m or -h.
 * Hard links are counted once. Directories are listed after their contents. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == 0120000)
#endif

static int all, summarize, total, human, apparent, onefs, follow, args_follow, zero, sep_dirs;
static long long unit = 1024;
static long maxdepth = -1;
static int rc;
static unsigned long long root_dev;

/* inodes with more than one link, to count them once */
struct seen { unsigned long long dev, ino; };
static struct seen *seen;
static size_t nseen, capseen;
static int already(const struct stat *st) {
  for (size_t i = 0; i < nseen; i++) if (seen[i].dev == st->st_dev && seen[i].ino == st->st_ino) return 1;
  if (nseen == capseen) seen = realloc(seen, (capseen = capseen * 2 + 64) * sizeof *seen);
  seen[nseen++] = (struct seen){st->st_dev, st->st_ino};
  return 0;
}

static void show(long long bytes, const char *path) {
  char buf[32];
  if (human) {
    static const char units[] = "KMGTPE";
    if (bytes < 1024) snprintf(buf, sizeof buf, "%lld", bytes);
    else {
      double v = bytes;
      int k = -1;
      while (v >= 1024 && k < 5) { v /= 1024; k++; }
      /* GNU rounds up: one decimal below 10, whole numbers above */
      if (v < 10) {
        long long t = (long long)(v * 10);
        if (t < v * 10) t++;
        if (t >= 100) snprintf(buf, sizeof buf, "%lld%c", t / 10, units[k]);
        else snprintf(buf, sizeof buf, "%lld.%lld%c", t / 10, t % 10, units[k]);
      } else {
        long long t = (long long)v;
        if (t < v) t++;
        if (t >= 1024 && k < 5) snprintf(buf, sizeof buf, "1.0%c", units[k + 1]);
        else snprintf(buf, sizeof buf, "%lld%c", t, units[k]);
      }
    }
  } else snprintf(buf, sizeof buf, "%lld", (bytes + unit - 1) / unit);
  printf("%s\t%s%c", buf, path, zero ? 0 : '\n');
}
/* GNU du counts a directory's own apparent size as 0 */
static long long size_of(const struct stat *st) { return apparent ? (S_ISDIR(st->st_mode) ? 0 : st->st_size) : (long long)st->st_blocks * 512; }

static long long walk(const char *path, int depth, int top) {
  struct stat st;
  int r = (follow || (top && args_follow)) ? stat(path, &st) : lstat(path, &st);
  if (r != 0) { fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno)); rc = 1; return 0; }
  if (top) root_dev = st.st_dev;
  if (st.st_nlink > 1 && !S_ISDIR(st.st_mode) && already(&st)) return 0;
  long long sum = size_of(&st);
  if (S_ISDIR(st.st_mode) && !(onefs && st.st_dev != root_dev)) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "du: cannot read directory '%s': %s\n", path, strerror(errno)); rc = 1; }
    else {
      struct dirent *e;
      long long subdirs = 0;
      size_t pl = strlen(path);
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *sub = malloc(pl + strlen(e->d_name) + 2);
        sprintf(sub, "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", e->d_name);
        struct stat cs;
        int isdir = lstat(sub, &cs) == 0 && S_ISDIR(cs.st_mode);
        long long s = walk(sub, depth + 1, 0);
        if (isdir && sep_dirs) subdirs += s;
        sum += s;
        free(sub);
      }
      closedir(d);
      if (sep_dirs) sum -= subdirs;
    }
    if (!summarize && (maxdepth < 0 || depth <= maxdepth)) show(sum, path);
  } else if ((all || top) && !summarize && (maxdepth < 0 || depth <= maxdepth)) show(sum, path);
  return sum;
}

int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (a[1] == '-') {
      if (!strcmp(a, "--all")) all = 1;
      else if (!strcmp(a, "--summarize")) summarize = 1;
      else if (!strcmp(a, "--total")) total = 1;
      else if (!strcmp(a, "--human-readable")) human = 1;
      else if (!strcmp(a, "--apparent-size")) apparent = 1;
      else if (!strcmp(a, "--bytes")) apparent = 1, unit = 1, human = 0;
      else if (!strcmp(a, "--one-file-system")) onefs = 1;
      else if (!strcmp(a, "--dereference")) follow = 1;
      else if (!strcmp(a, "--null")) zero = 1;
      else if (!strcmp(a, "--separate-dirs")) sep_dirs = 1;
      else if (!strncmp(a, "--max-depth=", 12)) maxdepth = atol(a + 12);
      else if (!strncmp(a, "--block-size=", 13)) unit = atoll(a + 13) > 0 ? atoll(a + 13) : 1024;
      else { fprintf(stderr, "du: unrecognized option '%s'\n", a); return 1; }
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      switch (*f) {
      case 'a': all = 1; break;
      case 's': summarize = 1; break;
      case 'c': total = 1; break;
      case 'h': human = 1; break;
      case 'k': unit = 1024; human = 0; break;
      case 'm': unit = 1024 * 1024; human = 0; break;
      case 'b': apparent = 1; unit = 1; human = 0; break;
      case 'x': onefs = 1; break;
      case 'L': follow = 1; break;
      case 'P': follow = 0; break;
      case 'H': case 'D': args_follow = 1; break;
      case 'S': sep_dirs = 1; break;
      case '0': zero = 1; break;
      case 'd': {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL), *e;
        if (!v) { fprintf(stderr, "du: option requires an argument -- 'd'\n"); return 1; }
        maxdepth = strtol(v, &e, 10);
        if (*e || maxdepth < 0) { fprintf(stderr, "du: invalid maximum depth '%s'\n", v); return 1; }
        f += strlen(f) - 1;
        break;
      }
      default: fprintf(stderr, "du: invalid option -- '%c'\nusage: du [-abchkms] [-d depth] [file...]\n", *f); return 1;
      }
    }
  }
  if (all && summarize) { fprintf(stderr, "du: cannot both summarize and show all entries\n"); return 1; }
  if (summarize) maxdepth = 0, summarize = 0;
  setvbuf(stdout, NULL, _IOFBF, 65536);
  static char *dot[] = {"."};
  char **files = i < argc ? argv + i : dot;
  int n = i < argc ? argc - i : 1;
  long long grand = 0;
  for (int k = 0; k < n; k++) grand += walk(files[k], 0, 1);
  if (total) show(grand, "total");
  if (fflush(stdout)) return 1;
  return rc;
}
