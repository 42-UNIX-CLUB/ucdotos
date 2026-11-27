// SPDX-License-Identifier: GPL-2.0-or-later
/* ls [-alh1AFrtSd] [file...]: colours and columns when stdout is a terminal */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static int all, almost, lng, human, one, classify, rev, by_time, by_size, dir_only, tty, width = 80;
struct ent { char *name; struct stat st; char *target; }; /* target: where a symbolic link points (-l) */

static int ucols(const char *s) { int c = 0; for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) c++; return c; }
static int is_exec(const struct stat *st) { return S_ISREG(st->st_mode) && (st->st_mode & 0111); }
static const char *colour(const struct stat *st) {
  if (S_ISLNK(st->st_mode)) return "\033[1;36m";
  if (S_ISDIR(st->st_mode)) return "\033[1;34m";
  if (S_ISCHR(st->st_mode)) return "\033[1;33m";
  if (S_ISFIFO(st->st_mode)) return "\033[33m";
  if ((st->st_mode & S_IFMT) == S_IFSOCK) return "\033[1;35m";
  if (is_exec(st)) return "\033[1;32m";
  return NULL;
}
static char indicator(const struct stat *st) {
  if (!classify) return 0;
  if (S_ISLNK(st->st_mode)) return '@';
  if (S_ISDIR(st->st_mode)) return '/';
  if (S_ISFIFO(st->st_mode)) return '|';
  if ((st->st_mode & S_IFMT) == S_IFSOCK) return '=';
  if (is_exec(st)) return '*';
  return 0;
}
static int name_width(const struct ent *e) { return ucols(e->name) + (indicator(&e->st) != 0); }
static void put_name(const struct ent *e) {
  const char *c = tty ? colour(&e->st) : NULL;
  if (c) fputs(c, stdout);
  fputs(e->name, stdout);
  if (c) fputs("\033[0m", stdout);
  char ind = indicator(&e->st);
  if (ind) putchar(ind);
  if (lng && e->target) printf(" -> %s", e->target);
}

static int cmp(const void *a, const void *b) {
  const struct ent *x = a, *y = b;
  int r = 0;
  if (by_time) r = x->st.st_mtime < y->st.st_mtime ? 1 : x->st.st_mtime > y->st.st_mtime ? -1 : 0;
  else if (by_size) r = x->st.st_size < y->st.st_size ? 1 : x->st.st_size > y->st.st_size ? -1 : 0;
  if (!r) r = strcmp(x->name, y->name);
  return rev ? -r : r;
}

static void civil(long long t, int *y, int *mo, int *d, int *h, int *mi) {
  long long days = t / 86400, s = t % 86400;
  if (s < 0) { s += 86400; days--; }
  *h = s / 3600; *mi = s / 60 % 60;
  long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *mo = mp < 10 ? mp + 3 : mp - 9;
  *y = yoe + era * 400 + (*mo <= 2);
}
static void size_str(long long n, char *out) {
  if (!human || n < 1024) { sprintf(out, "%lld", n); return; }
  const char *u = "KMGT";
  double v = n;
  int k = -1;
  while (v >= 1024 && k < 3) { v /= 1024; k++; }
  if (v < 10) sprintf(out, "%d.%d%c", (int)v, (int)((v - (int)v) * 10 + 0.5) % 10, u[k]);
  else sprintf(out, "%d%c", (int)(v + 0.5), u[k]);
}
static void long_list(struct ent *v, int n) {
  int ws = 1, wl = 1;
  char tmp[32];
  for (int i = 0; i < n; i++) {
    size_str(v[i].st.st_size, tmp);
    if ((int)strlen(tmp) > ws) ws = strlen(tmp);
    int l = snprintf(tmp, sizeof tmp, "%u", v[i].st.st_nlink);
    if (l > wl) wl = l;
  }
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  long long now = time(0);
  for (int i = 0; i < n; i++) {
    struct stat *st = &v[i].st;
    unsigned m = st->st_mode;
    char mode[11];
    mode[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : (m & S_IFMT) == S_IFSOCK ? 's' : '-';
    for (int k = 0; k < 9; k++) mode[1 + k] = (m >> (8 - k)) & 1 ? "rwxrwxrwx"[k] : '-';
    if (m & S_ISUID) mode[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID) mode[6] = (m & 010) ? 's' : 'S';
    if (m & S_ISVTX) mode[9] = (m & 01) ? 't' : 'T';
    mode[10] = 0;
    char user[16], group[16];
    if (st->st_uid == 0) strcpy(user, "root"); else if (st->st_uid == 1000) strcpy(user, "user"); else sprintf(user, "%u", st->st_uid);
    if (st->st_gid == 0) strcpy(group, "root"); else if (st->st_gid == 1000) strcpy(group, "user"); else sprintf(group, "%u", st->st_gid);
    size_str(st->st_size, tmp);
    int y, mo, d, h, mi;
    civil(st->st_mtime, &y, &mo, &d, &h, &mi);
    char when[16];
    if (st->st_mtime > now - 15778476 && st->st_mtime <= now + 3600) sprintf(when, "%s %2d %02d:%02d", mon[mo - 1], d, h, mi);
    else sprintf(when, "%s %2d  %d", mon[mo - 1], d, y);
    printf("%s %*u %-5s %-5s %*s %s ", mode, wl, st->st_nlink, user, group, ws, tmp, when);
    put_name(&v[i]);
    putchar('\n');
  }
}
static void columns(struct ent *v, int n) {
  if (!n) return;
  if (one || !tty) { for (int i = 0; i < n; i++) { put_name(&v[i]); putchar('\n'); } return; }
  int *w = malloc(n * sizeof *w), *colw = malloc(n * sizeof *colw);
  for (int i = 0; i < n; i++) w[i] = name_width(&v[i]);
  int rows = n, cols = 1;
  for (int c = n; c >= 1; c--) { /* the most columns that fit */
    int r = (n + c - 1) / c, total = 0;
    if ((r - 1) * c >= n && c > 1) continue;
    for (int k = 0; k < c; k++) {
      int mx = 0;
      for (int j = k * r; j < (k + 1) * r && j < n; j++) if (w[j] > mx) mx = w[j];
      colw[k] = mx;
      total += mx + (k + 1 < c ? 2 : 0);
    }
    if (total <= width) { rows = r; cols = c; break; }
  }
  for (int k = 0; k < cols; k++) {
    int mx = 0;
    for (int j = k * rows; j < (k + 1) * rows && j < n; j++) if (w[j] > mx) mx = w[j];
    colw[k] = mx;
  }
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      int i = c * rows + r;
      if (i >= n) break;
      put_name(&v[i]);
      if (c + 1 < cols && i + rows < n) for (int k = w[i]; k < colw[c] + 2; k++) putchar(' ');
    }
    putchar('\n');
  }
  free(w);
  free(colw);
}
static void show(struct ent *v, int n) {
  qsort(v, n, sizeof *v, cmp);
  if (lng) long_list(v, n); else columns(v, n);
}
static int list_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d) { fprintf(stderr, "ls: cannot open directory '%s': %s\n", path, strerror(errno)); return 2; }
  struct ent *v = NULL;
  int n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' && !all && !almost) continue;
    if (almost && !all && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
    if (n == cap) v = realloc(v, (cap = cap ? cap * 2 : 64) * sizeof *v);
    v[n].name = strdup(e->d_name);
    char *full = malloc(strlen(path) + strlen(e->d_name) + 2);
    sprintf(full, "%s/%s", path, e->d_name);
    if (lstat(full, &v[n].st) != 0) memset(&v[n].st, 0, sizeof v[n].st);
    v[n].target = NULL;
    if (lng && S_ISLNK(v[n].st.st_mode)) {
      char t[4096];
      long tl = readlink(full, t, sizeof t - 1);
      if (tl >= 0) { t[tl] = 0; v[n].target = strdup(t); }
    }
    free(full);
    n++;
  }
  closedir(d);
  if (lng) {
    long long blocks = 0;
    for (int i = 0; i < n; i++) blocks += (v[i].st.st_size + 1023) / 1024;
    printf("total %lld\n", blocks);
  }
  show(v, n);
  for (int i = 0; i < n; i++) { free(v[i].name); free(v[i].target); }
  free(v);
  return 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strncmp(argv[i], "--color", 7)) continue;
    for (char *f = argv[i] + 1; *f; f++) {
      switch (*f) {
      case 'a': all = 1; break;
      case 'A': almost = 1; break;
      case 'l': lng = 1; break;
      case 'h': human = 1; break;
      case '1': one = 1; break;
      case 'F': classify = 1; break;
      case 'r': rev = 1; break;
      case 't': by_time = 1; break;
      case 'S': by_size = 1; break;
      case 'd': dir_only = 1; break;
      default: fprintf(stderr, "ls: invalid option -- '%c'\nusage: ls [-alh1AFrtSd] [file...]\n", *f); return 2;
      }
    }
  }
  struct termios t;
  struct winsize ws;
  tty = __syscall3(SYS_ioctl, 1, TCGETS, &t) == 0;
  if (__syscall3(SYS_ioctl, 1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) width = ws.ws_col;
  setvbuf(stdout, 0, _IOFBF, 0);
  if (i >= argc) { rc = list_dir("."); fflush(stdout); return rc; }
  struct ent *files = malloc((argc - i) * sizeof *files);
  char **dirs = malloc((argc - i) * sizeof *dirs);
  int nf = 0, nd = 0;
  for (int k = i; k < argc; k++) {
    struct stat st;
    /* with -l or -d a link named on the command line is shown, not followed (as GNU ls) */
    if (((lng || dir_only) ? lstat(argv[k], &st) : stat(argv[k], &st)) != 0) { fprintf(stderr, "ls: cannot access '%s': %s\n", argv[k], strerror(errno)); rc = 2; continue; }
    if (S_ISDIR(st.st_mode) && !dir_only) dirs[nd++] = argv[k];
    else {
      files[nf].name = argv[k]; files[nf].st = st; files[nf].target = NULL;
      char t[4096];
      long tl = S_ISLNK(st.st_mode) ? readlink(argv[k], t, sizeof t - 1) : -1;
      if (tl >= 0) { t[tl] = 0; files[nf].target = strdup(t); }
      nf++;
    }
  }
  show(files, nf);
  for (int k = 0; k < nd; k++) {
    if (nf || nd > 1) printf("%s%s:\n", (nf || k) ? "\n" : "", dirs[k]);
    rc |= list_dir(dirs[k]);
  }
  fflush(stdout);
  return rc;
}
