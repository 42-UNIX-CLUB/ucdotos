// SPDX-License-Identifier: GPL-2.0-or-later
/* diff [-abBiNqrsTwZ] [-u | -U N | -c | -C N | -e | --normal] [--label L]
 *      [--strip-trailing-cr] [-x PAT] [--minimal] file1 file2
 * Compare files line by line. Exit status: 0 same, 1 different, 2 trouble.
 *
 * The edit script comes from Myers' O(ND) algorithm in its linear-space form:
 * find the middle snake of an optimal path from both ends at once, then solve
 * the two halves the same way. Runs of changes are then slid as far towards
 * the end as the text allows, as GNU diff does, so ambiguous changes are
 * reported in the same place. When the edit distance gets very large (two
 * mostly different big files) the search stops early and splits at the
 * furthest point reached, which keeps the time bounded but may report a
 * longer script than the shortest one; --minimal turns that off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>

enum { F_NORMAL, F_UNIFIED, F_CONTEXT, F_ED };
static int format = F_NORMAL, context = 3, brief, report_same, recursive, new_file, text, icase, ign_all_space,
           ign_space_change, ign_blank, ign_trailing, strip_cr, initial_tab, minimal;
static const char *label[2];
static char **excludes;
static int nexcludes;
static char *switches; /* the options as given, for "diff -r ..." lines */

struct file {
  const char *name;
  char *data;
  long size;
  char **line; long *len; int *id;
  long n;
  int last_nl; /* the last line ended with a newline */
  struct stat st;
};

/* ---- reading ---- */
static int read_file(struct file *f, const char *name) {
  f->name = name;
  int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
  if (fd < 0) return -1;
  if (fstat(fd, &f->st) != 0) memset(&f->st, 0, sizeof f->st);
  long cap = 65536, n = 0, r;
  char *d = malloc(cap + 1);
  while ((r = read(fd, d + n, cap - n)) > 0) {
    n += r;
    if (n == cap) d = realloc(d, (cap *= 2) + 1);
  }
  if (fd) close(fd);
  if (r < 0) { free(d); return -1; }
  f->data = d;
  f->size = n;
  long lines = 0;
  for (long i = 0; i < n; i++) if (d[i] == '\n') lines++;
  if (n && d[n - 1] != '\n') lines++;
  f->line = malloc((lines + 1) * sizeof *f->line);
  f->len = malloc((lines + 1) * sizeof *f->len);
  f->n = 0;
  for (long i = 0; i < n;) {
    char *e = memchr(d + i, '\n', n - i);
    long l = e ? e - (d + i) : n - i;
    f->line[f->n] = d + i;
    f->len[f->n++] = l;
    i += l + 1;
  }
  f->last_nl = !n || d[n - 1] == '\n';
  return 0;
}
static int is_binary(const struct file *f) {
  long n = f->size < 8192 ? f->size : 8192;
  return !text && memchr(f->data, 0, n) != NULL;
}

/* ---- line identities: equal lines (under -i -b -w -Z) get equal numbers ---- */
struct hent { char *key; long klen; int id; struct hent *next; };
static struct hent **htab;
static long hsize, nids;
static char *norm(const char *s, long n, long *outn, int last_no_nl) {
  char *o = malloc(n + 2);
  long k = 0;
  if (strip_cr && n && s[n - 1] == '\r') n--;
  for (long i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (ign_all_space && isspace(c)) continue;
    if (ign_space_change && isspace(c)) {
      while (i + 1 < n && isspace((unsigned char)s[i + 1])) i++;
      if (i + 1 < n) o[k++] = ' ';
      continue;
    }
    o[k++] = icase ? tolower(c) : c;
  }
  if (ign_trailing) while (k && (o[k - 1] == ' ' || o[k - 1] == '\t')) k--;
  if (last_no_nl) o[k++] = 0; /* a last line without newline differs from one with it */
  *outn = k;
  return o;
}
static int line_id(const char *s, long n, int last_no_nl) {
  long kn;
  char *k = norm(s, n, &kn, last_no_nl);
  unsigned long h = 5381;
  for (long i = 0; i < kn; i++) h = h * 33 + (unsigned char)k[i];
  struct hent **b = &htab[h % hsize];
  for (struct hent *e = *b; e; e = e->next)
    if (e->klen == kn && !memcmp(e->key, k, kn)) { free(k); return e->id; }
  struct hent *e = malloc(sizeof *e);
  *e = (struct hent){k, kn, (int)nids++, *b};
  *b = e;
  return e->id;
}
static void assign_ids(struct file *a, struct file *b) {
  hsize = (a->n + b->n) * 2 + 101;
  htab = calloc(hsize, sizeof *htab);
  nids = 0;
  struct file *fs[2] = {a, b};
  for (int k = 0; k < 2; k++) {
    struct file *f = fs[k];
    f->id = malloc((f->n + 1) * sizeof *f->id);
    /* a last line without newline differs from the same text with one, except in an ed script, which cannot say so */
    for (long i = 0; i < f->n; i++) f->id[i] = line_id(f->line[i], f->len[i], i == f->n - 1 && !f->last_nl && format != F_ED);
  }
}

/* ---- Myers, linear space ---- */
static int *A, *B;
static char *chA, *chB; /* changed: deleted from A, inserted in B */
static long *vf, *vb;
static long voff;
static long cost_limit;
#define NONE_F (LONG_MIN / 2) /* long is 32 bits on wasm32 */
#define NONE_B (LONG_MAX / 2)

static void breathe(void) {
#ifdef __wasm__
  static unsigned long n;
  if ((++n & 0xfffff) == 0) getpid(); /* a system call now and then: long work must not freeze the tab */
#endif
}

/* find the middle snake of A[a0,a1) x B[b0,b1); returns its ends in *x0 *y0 *x1 *y1 (relative) */
static void middle_snake(long a0, long a1, long b0, long b1, long *x0, long *y0, long *x1, long *y1) {
  long N = a1 - a0, M = b1 - b0, delta = N - M;
  int odd = delta & 1;
  for (long k = -M - 1; k <= N + 1; k++) { vf[voff + k] = NONE_F; vb[voff + k] = NONE_B; }
  vf[voff + 0] = 0;
  long x = 0, y = 0;
  while (x < N && y < M && A[a0 + x] == B[b0 + y]) x++, y++;
  vf[voff + 0] = x;
  vb[voff + delta] = N;
  x = N; y = M;
  while (x > 0 && y > 0 && A[a0 + x - 1] == B[b0 + y - 1]) x--, y--;
  vb[voff + delta] = x;
  if (!odd && delta == 0 && vb[voff] <= vf[voff]) { *x0 = *x1 = vf[voff]; *y0 = *y1 = vf[voff]; return; }
  for (long d = 1;; d++) {
    long kmin = -d < -M ? -M : -d, kmax = d > N ? N : d;
    /* forward: furthest reaching x on each diagonal k = x - y */
    for (long k = -d; k <= d; k += 2) {
      if (k < kmin || k > kmax) continue;
      long best = NONE_F;
      if (k + 1 <= N && vf[voff + k + 1] != NONE_F) best = vf[voff + k + 1];             /* down: a line of B */
      if (k - 1 >= -M && vf[voff + k - 1] != NONE_F && vf[voff + k - 1] + 1 > best) best = vf[voff + k - 1] + 1; /* right: a line of A */
      if (best == NONE_F) continue;
      x = best; y = x - k;
      if (x > N || y > M || y < 0) continue;
      long sx = x, sy = y;
      while (x < N && y < M && A[a0 + x] == B[b0 + y]) { x++; y++; breathe(); }
      vf[voff + k] = x;
      if (odd && k - delta >= -(d - 1) && k - delta <= d - 1 && vb[voff + k] != NONE_B && x >= vb[voff + k]) {
        *x0 = sx; *y0 = sy; *x1 = x; *y1 = y;
        return;
      }
    }
    /* backward: smallest x on each diagonal, from the end */
    for (long c = -d; c <= d; c += 2) {
      long k = c + delta;
      if (k < -M || k > N) continue;
      long best = NONE_B;
      if (k - 1 >= -M && vb[voff + k - 1] != NONE_B) best = vb[voff + k - 1];             /* up: a line of B */
      if (k + 1 <= N && vb[voff + k + 1] != NONE_B && vb[voff + k + 1] - 1 < best) best = vb[voff + k + 1] - 1; /* left: a line of A */
      if (best == NONE_B) continue;
      x = best; y = x - k;
      if (x < 0 || y < 0 || y > M) continue;
      long ex = x, ey = y;
      while (x > 0 && y > 0 && A[a0 + x - 1] == B[b0 + y - 1]) { x--; y--; breathe(); }
      vb[voff + k] = x;
      if (!odd && k >= -d && k <= d && vf[voff + k] != NONE_F && x <= vf[voff + k]) {
        *x0 = x; *y0 = y; *x1 = ex; *y1 = ey;
        return;
      }
    }
    if (!minimal && d >= cost_limit) {
      /* too expensive: split at the forward point that got furthest */
      long bx = 0, by = 0, bs = -1;
      for (long k = kmin; k <= kmax; k++) {
        long fx = vf[voff + k];
        if (fx == NONE_F) continue;
        long fy = fx - k;
        if (fy < 0 || fy > M || fx > N) continue;
        if (fx + fy > bs && fx + fy < N + M) { bs = fx + fy; bx = fx; by = fy; }
      }
      if (bs > 0) { *x0 = *x1 = bx; *y0 = *y1 = by; return; }
    }
  }
}

static void compare(long a0, long a1, long b0, long b1) {
  for (;;) {
    while (a0 < a1 && b0 < b1 && A[a0] == B[b0]) a0++, b0++;
    while (a0 < a1 && b0 < b1 && A[a1 - 1] == B[b1 - 1]) a1--, b1--;
    if (a0 == a1) { for (long i = b0; i < b1; i++) chB[i] = 1; return; }
    if (b0 == b1) { for (long i = a0; i < a1; i++) chA[i] = 1; return; }
    long x0, y0, x1, y1;
    middle_snake(a0, a1, b0, b1, &x0, &y0, &x1, &y1);
    /* the smaller half by recursion, the other by iterating (bounded stack) */
    long la = x0, lb = y0, ra = a1 - a0 - x1, rb = b1 - b0 - y1;
    if (la + lb <= ra + rb) {
      compare(a0, a0 + x0, b0, b0 + y0);
      a0 += x1; b0 += y1;
    } else {
      compare(a0 + x1, a1, b0 + y1, b1);
      a1 = a0 + x0; b1 = b0 + y0;
    }
  }
}

/* Where to report a change when several places cost the same, as GNU diff
 * does: each run of changed lines in one file slides as far towards the end
 * as the text allows (merging with runs it meets), except that it stops at
 * the last position where it ends next to changed lines of the other file,
 * so that the two make one change instead of a deletion and an insertion.
 * gap[k] says whether the other file has changed lines between its k-th and
 * (k+1)-th unchanged lines; a run whose start is preceded by k unchanged
 * lines is next to such a gap. */
static char *gap_of(const char *ch, long n) {
  long u = 0;
  for (long i = 0; i < n; i++) if (!ch[i]) u++;
  char *gap = calloc(u + 2, 1);
  long k = 0;
  for (long i = 0; i < n; i++) { if (ch[i]) gap[k] = 1; else k++; }
  return gap;
}
static void shift(const int *id, char *ch, long n, const char *gap) {
  long cb = 0; /* changed lines before the current run */
  for (long i = 0; i < n;) {
    if (!ch[i]) { i++; continue; }
    long s = i;
    while (i < n && ch[i]) i++;
    long len, best;
    do {
      len = i - s;
      /* up while the line above equals the last line of the run */
      while (s > 0 && !ch[s - 1] && id[s - 1] == id[i - 1]) {
        ch[--s] = 1; ch[--i] = 0;
        while (s > 0 && ch[s - 1]) { s--; cb--; }
      }
      best = gap[s - cb] ? i : -1;
      /* down while the first line of the run equals the line below */
      while (i < n && id[s] == id[i]) {
        ch[s++] = 0; ch[i++] = 1;
        while (i < n && ch[i]) i++;
        if (gap[s - cb]) best = i;
      }
    } while (len != i - s);
    while (best >= 0 && best < i) { ch[--s] = 1; ch[--i] = 0; }
    cb += i - s;
  }
}

/* ---- output ---- */
struct hunk { long a0, a1, b0, b1; }; /* ranges of changed lines */
static struct hunk *hunks;
static long nhunks;

static void collect(const struct file *a, const struct file *b) {
  nhunks = 0;
  long i = 0, j = 0;
  static long cap;
  while (i < a->n || j < b->n) {
    if (i < a->n && j < b->n && !chA[i] && !chB[j]) { i++; j++; continue; }
    long a0 = i, b0 = j;
    while (i < a->n && chA[i]) i++;
    while (j < b->n && chB[j]) j++;
    if (nhunks == cap) hunks = realloc(hunks, (cap = cap * 2 + 64) * sizeof *hunks);
    hunks[nhunks++] = (struct hunk){a0, i, b0, j};
    if (a0 == i && b0 == j) { i++; j++; nhunks--; } /* cannot happen; guards the loop */
  }
}
/* -B: a hunk made only of blank lines on both sides is not a change */
static int blank_hunk(const struct file *a, const struct file *b, const struct hunk *h) {
  for (long i = h->a0; i < h->a1; i++) for (long k = 0; k < a->len[i]; k++) if (!isspace((unsigned char)a->line[i][k])) return 0;
  for (long i = h->b0; i < h->b1; i++) for (long k = 0; k < b->len[i]; k++) if (!isspace((unsigned char)b->line[i][k])) return 0;
  return 1;
}

static void put_line(const char *prefix, const struct file *f, long i) {
  if (initial_tab && *prefix) {
    /* -T: the prefix without its trailing space, then a tab */
    size_t n = strlen(prefix);
    if (n > 1 && prefix[n - 1] == ' ') n--;
    fwrite(prefix, 1, n, stdout);
    putchar('\t');
  } else fputs(prefix, stdout);
  fwrite(f->line[i], 1, f->len[i], stdout);
  putchar('\n');
  if (i == f->n - 1 && !f->last_nl) puts("\\ No newline at end of file");
}
static void stamp(const char *mark, const struct file *f, int k) {
  if (label[k]) { printf("%s %s\n", mark, label[k]); return; }
  struct tm tm;
  time_t t = f->st.st_mtime;
  long nsec = 0;
#ifndef __wasm__
  nsec = f->st.st_mtim.tv_nsec;
#endif
  localtime_r(&t, &tm);
  char tb[64];
  strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
  char zone[16];
  strftime(zone, sizeof zone, "%z", &tm);
  if (!*zone) strcpy(zone, "+0000");
  printf("%s %s\t%s.%09ld %s\n", mark, f->name, tb, nsec, zone);
}
static void range_normal(long s, long e) { /* 1-based, inclusive */
  if (e <= s) printf("%ld", s); else printf("%ld,%ld", s, e);
}
static void range_unified(long start, long count) {
  if (count == 1) printf("%ld", start + 1);
  else printf("%ld,%ld", count ? start + 1 : start, count);
}
static void range_context(long start, long count) {
  if (count == 0) printf("%ld", start);
  else if (count == 1) printf("%ld", start + 1);
  else printf("%ld,%ld", start + 1, start + count);
}

static void print_normal(const struct file *a, const struct file *b) {
  for (long h = 0; h < nhunks; h++) {
    struct hunk *k = &hunks[h];
    if (k->a0 == k->a1) { printf("%ld", k->a0); putchar('a'); range_normal(k->b0 + 1, k->b1); }
    else if (k->b0 == k->b1) { range_normal(k->a0 + 1, k->a1); putchar('d'); printf("%ld", k->b0); }
    else { range_normal(k->a0 + 1, k->a1); putchar('c'); range_normal(k->b0 + 1, k->b1); }
    putchar('\n');
    for (long i = k->a0; i < k->a1; i++) put_line("< ", a, i);
    if (k->a0 < k->a1 && k->b0 < k->b1) puts("---");
    for (long i = k->b0; i < k->b1; i++) put_line("> ", b, i);
  }
}
static void print_ed(const struct file *a, const struct file *b) {
  for (long h = nhunks - 1; h >= 0; h--) {
    struct hunk *k = &hunks[h];
    if (k->a0 == k->a1) printf("%lda\n", k->a0);
    else { range_normal(k->a0 + 1, k->a1); puts(k->b0 == k->b1 ? "d" : "c"); }
    if (k->b0 < k->b1) {
      for (long i = k->b0; i < k->b1; i++) {
        if (b->len[i] == 1 && b->line[i][0] == '.') puts("..");
        else { fwrite(b->line[i], 1, b->len[i], stdout); putchar('\n'); }
      }
      puts(".");
    }
  }
}
/* unified and context output: hunks closer than 2*context lines are merged */
static void print_grouped(const struct file *a, const struct file *b) {
  for (long h = 0; h < nhunks;) {
    long g = h;
    while (g + 1 < nhunks && hunks[g + 1].a0 - hunks[g].a1 <= 2 * context) g++;
    /* context lines are common to both files, so both sides get the same number */
    long before = hunks[h].a0 < context ? hunks[h].a0 : context;
    long after = a->n - hunks[g].a1 < context ? a->n - hunks[g].a1 : context;
    long ca0 = hunks[h].a0 - before, cb0 = hunks[h].b0 - before;
    long ca1 = hunks[g].a1 + after, cb1 = hunks[g].b1 + after;
    if (format == F_UNIFIED) {
      printf("@@ -"); range_unified(ca0, ca1 - ca0); printf(" +"); range_unified(cb0, cb1 - cb0); printf(" @@\n");
      long i = ca0, j = cb0;
      for (long k = h; k <= g; k++) {
        while (i < hunks[k].a0) { put_line(" ", a, i); i++; j++; }
        for (; i < hunks[k].a1; i++) put_line("-", a, i);
        for (; j < hunks[k].b1; j++) put_line("+", b, j);
      }
      while (i < ca1) { put_line(" ", a, i); i++; j++; }
    } else {
      puts("***************");
      printf("*** "); range_context(ca0, ca1 - ca0); puts(" ****");
      int anyA = 0, anyB = 0;
      for (long k = h; k <= g; k++) { if (hunks[k].a0 < hunks[k].a1) anyA = 1; if (hunks[k].b0 < hunks[k].b1) anyB = 1; }
      if (anyA) {
        long i = ca0;
        for (long k = h; k <= g; k++) {
          for (; i < hunks[k].a0; i++) put_line("  ", a, i);
          const char *p = hunks[k].b0 < hunks[k].b1 ? "! " : "- ";
          for (; i < hunks[k].a1; i++) put_line(p, a, i);
        }
        for (; i < ca1; i++) put_line("  ", a, i);
      }
      printf("--- "); range_context(cb0, cb1 - cb0); puts(" ----");
      if (anyB) {
        long j = cb0;
        for (long k = h; k <= g; k++) {
          for (; j < hunks[k].b0; j++) put_line("  ", b, j);
          const char *p = hunks[k].a0 < hunks[k].a1 ? "! " : "+ ";
          for (; j < hunks[k].b1; j++) put_line(p, b, j);
        }
        for (; j < cb1; j++) put_line("  ", b, j);
      }
    }
    h = g + 1;
  }
}

static void free_file(struct file *f) { free(f->data); free(f->line); free(f->len); free(f->id); }

/* compare two regular files; returns 0 same, 1 different, 2 trouble */
static int diff_files(const char *n1, const char *n2, int header) {
  struct file a = {0}, b = {0};
  int missing[2] = {0, 0};
  if (read_file(&a, n1) != 0) {
    if (errno == ENOENT && new_file) { missing[0] = 1; a.name = n1; a.data = malloc(1); a.line = malloc(sizeof *a.line); a.len = malloc(sizeof *a.len); a.last_nl = 1; }
    else { fprintf(stderr, "diff: %s: %s\n", n1, strerror(errno)); return 2; }
  }
  if (read_file(&b, n2) != 0) {
    if (errno == ENOENT && new_file) { missing[1] = 1; b.name = n2; b.data = malloc(1); b.line = malloc(sizeof *b.line); b.len = malloc(sizeof *b.len); b.last_nl = 1; }
    else { fprintf(stderr, "diff: %s: %s\n", n2, strerror(errno)); free_file(&a); return 2; }
  }
  int rc = 0;
  if (a.size == b.size && !memcmp(a.data, b.data, a.size)) rc = 0;
  else if (is_binary(&a) || is_binary(&b)) {
    if (brief) printf("Files %s and %s differ\n", n1, n2);
    else {
      if (header) printf("diff%s %s %s\n", switches, n1, n2);
      printf("Binary files %s and %s differ\n", n1, n2);
    }
    rc = 1;
  } else {
    assign_ids(&a, &b);
    A = a.id; B = b.id;
    chA = calloc(a.n + 1, 1);
    chB = calloc(b.n + 1, 1);
    voff = a.n + b.n + 2;
    vf = malloc((2 * voff + 3) * sizeof *vf);
    vb = malloc((2 * voff + 3) * sizeof *vb);
    long tot = a.n + b.n;
    cost_limit = 4096;
    while (cost_limit * cost_limit < tot) cost_limit *= 2; /* about sqrt(N+M), at least 4096 */
    compare(0, a.n, 0, b.n);
    char *gap = gap_of(chB, b.n);
    shift(a.id, chA, a.n, gap);
    free(gap);
    gap = gap_of(chA, a.n);
    shift(b.id, chB, b.n, gap);
    free(gap);
    collect(&a, &b);
    if (ign_blank) {
      long k = 0;
      for (long h = 0; h < nhunks; h++) if (!blank_hunk(&a, &b, &hunks[h])) hunks[k++] = hunks[h];
      nhunks = k;
    }
    if (nhunks) {
      rc = 1;
      if (brief) printf("Files %s and %s differ\n", n1, n2);
      else {
        if (header) printf("diff%s %s %s\n", switches, n1, n2);
        if (format == F_UNIFIED || format == F_CONTEXT) {
          if (missing[0]) a.st.st_mtime = 0;
          if (missing[1]) b.st.st_mtime = 0;
          stamp(format == F_UNIFIED ? "---" : "***", &a, 0);
          stamp(format == F_UNIFIED ? "+++" : "---", &b, 1);
          print_grouped(&a, &b);
        } else if (format == F_ED) {
          print_ed(&a, &b);
          /* an ed script cannot say that a line has no newline */
          const struct file *fs[2] = {&a, &b};
          for (int k = 0; k < 2; k++) if (!fs[k]->last_nl && fs[k]->size) { fflush(stdout); fprintf(stderr, "diff: %s: No newline at end of file\n\n", fs[k]->name); rc = 2; }
        }
        else print_normal(&a, &b);
      }
    }
    for (long i = 0; i < hsize; i++) for (struct hent *e = htab[i], *nx; e; e = nx) { nx = e->next; free(e->key); free(e); }
    free(htab); free(chA); free(chB); free(vf); free(vb);
  }
  if (rc == 0 && report_same) printf("Files %s and %s are identical\n", n1, n2);
  free_file(&a);
  free_file(&b);
  return rc;
}

static int excluded(const char *name) {
  for (int i = 0; i < nexcludes; i++) if (fnmatch(excludes[i], name, 0) == 0) return 1;
  return 0;
}
static int cmpstr(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }
static char **list_dir(const char *path, int *n) {
  DIR *d = opendir(path);
  *n = 0;
  if (!d) return NULL;
  char **v = NULL;
  int cap = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || excluded(e->d_name)) continue;
    if (*n == cap) v = realloc(v, (cap = cap * 2 + 32) * sizeof *v);
    v[(*n)++] = strdup(e->d_name);
  }
  closedir(d);
  if (*n) qsort(v, *n, sizeof *v, cmpstr);
  return v;
}
static char *join(const char *a, const char *b) {
  char *p = malloc(strlen(a) + strlen(b) + 2);
  sprintf(p, "%s%s%s", a, a[strlen(a) - 1] == '/' ? "" : "/", b);
  return p;
}
static int diff_dirs(const char *d1, const char *d2) {
  int n1, n2, rc = 0;
  char **l1 = list_dir(d1, &n1), **l2 = list_dir(d2, &n2);
  /* with -N a directory may exist on one side only */
  if (!l1 && access(d1, R_OK) != 0 && !(new_file && errno == ENOENT)) { fprintf(stderr, "diff: %s: %s\n", d1, strerror(errno)); return 2; }
  if (!l2 && access(d2, R_OK) != 0 && !(new_file && errno == ENOENT)) { fprintf(stderr, "diff: %s: %s\n", d2, strerror(errno)); return 2; }
  int i = 0, j = 0;
  while (i < n1 || j < n2) {
    int c = i >= n1 ? 1 : j >= n2 ? -1 : strcmp(l1[i], l2[j]);
    const char *name = c <= 0 ? l1[i] : l2[j];
    char *p1 = join(d1, name), *p2 = join(d2, name);
    struct stat s1, s2;
    int e1 = c <= 0 ? stat(p1, &s1) : -1, e2 = c >= 0 ? stat(p2, &s2) : -1;
    int r = 0;
    if (c != 0 && !new_file) {
      printf("Only in %s: %s\n", c < 0 ? d1 : d2, name);
      r = 1;
    } else if (c != 0) {
      /* -N: something on one side only is compared with nothing */
      int isdir = c < 0 ? S_ISDIR(s1.st_mode) : S_ISDIR(s2.st_mode);
      if (isdir && recursive) r = diff_dirs(p1, p2);
      else if (isdir) { printf("Only in %s: %s\n", c < 0 ? d1 : d2, name); r = 1; }
      else r = diff_files(p1, p2, 1);
    } else if (e1 == 0 && e2 == 0 && S_ISDIR(s1.st_mode) && S_ISDIR(s2.st_mode)) {
      if (recursive) r = diff_dirs(p1, p2);
      else printf("Common subdirectories: %s and %s\n", p1, p2);
    } else if (e1 == 0 && e2 == 0 && S_ISDIR(s1.st_mode) != S_ISDIR(s2.st_mode)) {
      printf("File %s is a %s while file %s is a %s\n", p1, S_ISDIR(s1.st_mode) ? "directory" : "regular file", p2, S_ISDIR(s2.st_mode) ? "directory" : "regular file");
      r = 1;
    } else r = diff_files(p1, p2, 1);
    if (r > rc) rc = r;
    free(p1); free(p2);
    if (c <= 0) i++;
    if (c >= 0) j++;
  }
  for (int k = 0; k < n1; k++) free(l1[k]);
  for (int k = 0; k < n2; k++) free(l2[k]);
  free(l1); free(l2);
  return rc;
}

/* the options as given, quoted for the shell when needed, as GNU diff shows them */
static void add_switch(const char *s) {
  size_t n = switches ? strlen(switches) : 0;
  int quote = !*s || s[strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./=:,+-@%")];
  switches = realloc(switches, n + strlen(s) * 4 + 4);
  char *w = switches + n;
  *w++ = ' ';
  if (quote) *w++ = '\'';
  for (; *s; s++) {
    if (*s == '\'' && quote) { memcpy(w, "'\\''", 4); w += 4; }
    else *w++ = *s;
  }
  if (quote) *w++ = '\'';
  *w = 0;
}
static long ctx_num(const char *v) {
  char *e;
  long n = strtol(v, &e, 10);
  if (*e || n < 0) { fprintf(stderr, "diff: invalid context length '%s'\n", v); exit(2); }
  return n;
}

int main(int argc, char **argv) {
  int i = 1, nlabel = 0;
  switches = calloc(1, 1);
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    int took_next = 0;
    if (a[1] == '-') {
      char *eq = strchr(a, '=');
      const char *v = eq ? eq + 1 : NULL;
      size_t kl = eq ? (size_t)(eq - a) : strlen(a);
#define IS(s) (kl == strlen(s) && !strncmp(a, s, kl))
      if (IS("--unified")) { format = F_UNIFIED; context = v ? ctx_num(v) : 3; }
      else if (IS("--context")) { format = F_CONTEXT; context = v ? ctx_num(v) : 3; }
      else if (IS("--brief")) brief = 1;
      else if (IS("--report-identical-files")) report_same = 1;
      else if (IS("--recursive")) recursive = 1;
      else if (IS("--new-file")) new_file = 1;
      else if (IS("--text")) text = 1;
      else if (IS("--ignore-case")) icase = 1;
      else if (IS("--ignore-all-space")) ign_all_space = 1;
      else if (IS("--ignore-space-change")) ign_space_change = 1;
      else if (IS("--ignore-blank-lines")) ign_blank = 1;
      else if (IS("--ignore-trailing-space")) ign_trailing = 1;
      else if (IS("--strip-trailing-cr")) strip_cr = 1;
      else if (IS("--initial-tab")) initial_tab = 1;
      else if (IS("--minimal")) minimal = 1;
      else if (IS("--normal")) format = F_NORMAL;
      else if (IS("--ed")) format = F_ED;
      else if (IS("--color") || IS("--no-dereference") || IS("--speed-large-files")) ;
      else if (IS("--label")) {
        if (!v) { if (i + 1 >= argc) { fprintf(stderr, "diff: option '--label' requires an argument\n"); return 2; } v = argv[++i]; took_next = 1; }
        if (nlabel < 2) label[nlabel++] = v;
      } else if (IS("--exclude")) {
        if (!v) { if (i + 1 >= argc) return 2; v = argv[++i]; took_next = 1; }
        excludes = realloc(excludes, (nexcludes + 1) * sizeof *excludes); excludes[nexcludes++] = (char *)v;
      } else { fprintf(stderr, "diff: unrecognized option '%s'\n", a); return 2; }
#undef IS
      add_switch(a);
      if (took_next) add_switch(argv[i]);
      continue;
    }
    add_switch(a);
    for (char *f = a + 1; *f; f++) {
      switch (*f) {
      case 'u': format = F_UNIFIED; break;
      case 'c': format = F_CONTEXT; break;
      case 'e': format = F_ED; break;
      case 'q': brief = 1; break;
      case 's': report_same = 1; break;
      case 'r': recursive = 1; break;
      case 'N': new_file = 1; break;
      case 'a': text = 1; break;
      case 'i': icase = 1; break;
      case 'w': ign_all_space = 1; break;
      case 'b': ign_space_change = 1; break;
      case 'B': ign_blank = 1; break;
      case 'Z': ign_trailing = 1; break;
      case 'T': initial_tab = 1; break;
      case 'd': minimal = 1; break;
      case 'p': case 'P': case 't': case 'E': break;
      case 'U': case 'C': case 'x': case 'L': {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "diff: option requires an argument -- '%c'\n", *f); return 2; }
        if (!f[1]) add_switch(v);
        if (*f == 'U' || *f == 'C') { format = *f == 'U' ? F_UNIFIED : F_CONTEXT; context = ctx_num(v); }
        else if (*f == 'x') { excludes = realloc(excludes, (nexcludes + 1) * sizeof *excludes); excludes[nexcludes++] = v; }
        else if (nlabel < 2) label[nlabel++] = v;
        f += strlen(f) - 1;
        break;
      }
      default:
        if (isdigit((unsigned char)*f)) { context = strtol(f, &f, 10); f--; break; }
        fprintf(stderr, "diff: invalid option -- '%c'\nusage: diff [-u | -c | -e] [-abBiqrsw] file1 file2\n", *f);
        return 2;
      }
    }
  }
  if (argc - i != 2) {
    if (argc - i < 2) fprintf(stderr, "diff: missing operand after '%s'\n", argv[argc - 1]);
    else fprintf(stderr, "diff: extra operand '%s'\n", argv[i + 2]);
    return 2;
  }
  setvbuf(stdout, NULL, _IOFBF, 65536);
  const char *n1 = argv[i], *n2 = argv[i + 1];
  struct stat s1, s2;
  int d1 = strcmp(n1, "-") && stat(n1, &s1) == 0 && S_ISDIR(s1.st_mode);
  int d2 = strcmp(n2, "-") && stat(n2, &s2) == 0 && S_ISDIR(s2.st_mode);
  int rc;
  if (d1 && d2 && s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino) rc = 0; /* the same directory */
  else if (d1 && d2) rc = diff_dirs(n1, n2);
  else if (d1 || d2) {
    /* a directory and a file: compare with the file of the same name in it */
    const char *file = d1 ? n2 : n1, *dir = d1 ? n1 : n2;
    if (!strcmp(file, "-")) { fprintf(stderr, "diff: cannot compare '-' to a directory\n"); return 2; }
    const char *base = strrchr(file, '/');
    char *p = join(dir, base ? base + 1 : file);
    rc = d1 ? diff_files(p, n2, 0) : diff_files(n1, p, 0);
    free(p);
  } else rc = diff_files(n1, n2, 0);
  if (fflush(stdout) != 0) { fprintf(stderr, "diff: write error: %s\n", strerror(errno)); return 2; }
  return rc;
}
