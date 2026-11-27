// SPDX-License-Identifier: GPL-2.0-or-later
/* tar: create, list and extract tar archives, plain or compressed.
 *   tar -c [-zv] [-f FILE] [-C DIR] path...
 *   tar -t [-v] [-f FILE] [member...]
 *   tar -x [-vkmOp] [-f FILE] [-C DIR] [--strip-components=N] [member...]
 * Reading detects gzip, xz and zstd by itself; -z, -J and --zstd insist on one.
 * Writing compresses with gzip only (-z, or -a with a .tgz/.tar.gz name).
 * The old style without a dash works too: tar xzvf file.tar.gz.
 *
 * This kernel may not have symbolic links, hard links, chmod, chown or device
 * files yet. Each kind that fails with ENOSYS is reported once and extraction
 * goes on; --strict turns those into a failing exit status. A hard link that
 * cannot be made is replaced by a copy of the file it points to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "arc.h"

static char op;
static const char *archive;
static int verbose, zfmt = -1, keep_old, no_mtime, to_stdout, same_owner, same_perms, strict, numeric, strip;
static int auto_compress;
static long long owner_uid = -1, owner_gid = -1;
static char owner_name[32], group_name[32];
static int rc;
static int unsupported_seen[8];
static int unsupported_count;
static int made_symlink;
static unsigned char buf[65536];

enum { U_SYMLINK, U_LINK, U_CHMOD, U_CHOWN, U_TIME, U_DEV, U_FIFO };
static const char *const unsupported_text[] = {
  "symbolic links are not supported by this system yet; skipping them",
  "hard links are not supported by this system yet; copying the files instead",
  "changing permission bits is not supported by this system yet",
  "changing owners is not supported by this system yet",
  "setting file times is not supported by this system yet",
  "device files are not supported by this system yet; skipping them",
  "named pipes are not supported by this system yet; skipping them",
};

static void unsupported(int kind) {
  unsupported_count++;
  if (unsupported_seen[kind]++) return;
  fprintf(stderr, "tar: %s\n", unsupported_text[kind]);
}

static void err(const char *what, const char *msg) {
  fprintf(stderr, "tar: %s: %s\n", what, msg);
  rc = 2;
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

/* ---------------------------------------------------------------- listing */

static void mode_string(const tar_entry *e, char *s) {
  static const char types[] = "-hlcbdp";
  char t = e->type;
  s[0] = t >= '0' && t <= '6' ? types[t - '0'] : '-';
  const char *x = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s[1 + i] = e->mode & (0400 >> i) ? x[i] : '-';
  if (e->mode & 04000) s[3] = s[3] == 'x' ? 's' : 'S';
  if (e->mode & 02000) s[6] = s[6] == 'x' ? 's' : 'S';
  if (e->mode & 01000) s[9] = s[9] == 'x' ? 't' : 'T';
  s[10] = 0;
}

static void list_entry(FILE *f, const tar_entry *e) {
  if (verbose < 2 && op == 'x') { fprintf(f, "%s\n", e->path); return; }
  if (!verbose) { fprintf(f, "%s\n", e->path); return; }
  char ms[11], who[80], size[32], date[32];
  mode_string(e, ms);
  if (e->uname[0] && !numeric) snprintf(who, sizeof who, "%s/%s", e->uname, e->gname[0] ? e->gname : "?");
  else snprintf(who, sizeof who, "%u/%u", e->uid, e->gid);
  if (e->type == '3' || e->type == '4') snprintf(size, sizeof size, "%u,%u", e->devmajor, e->devminor);
  else snprintf(size, sizeof size, "%llu", e->size);
  time_t t = (time_t)e->mtime;
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
  int pad = 19 - (int)strlen(who) - 1;
  if (pad < (int)strlen(size)) pad = (int)strlen(size);
  fprintf(f, "%s %s %*s %s %s", ms, who, pad, size, date, e->path);
  if (e->type == '2') fprintf(f, " -> %s", e->link);
  if (e->type == '1') fprintf(f, " link to %s", e->link);
  fprintf(f, "\n");
}

/* ------------------------------------------------------------- name rules */

static char **members;
static int nmembers;
static char *member_found;

static int member_match(const char *path) {
  if (!nmembers) return 1;
  for (int i = 0; i < nmembers; i++) {
    const char *m = members[i];
    size_t n = strlen(m);
    while (n > 1 && m[n - 1] == '/') n--;
    if (!strncmp(path, m, n) && (path[n] == 0 || path[n] == '/')) { member_found[i] = 1; return 1; }
  }
  return 0;
}

/* Turns an archive name into a safe relative name. Returns NULL to skip. */
static int warned_abs;
static char *safe_name(char *p, int strip_n, const char *orig) {
  if (*p == '/') {
    while (*p == '/') p++;
    if (!warned_abs++) fprintf(stderr, "tar: Removing leading '/' from member names\n");
  }
  for (const char *c = p; *c;) {
    const char *e = strchr(c, '/');
    size_t n = e ? (size_t)(e - c) : strlen(c);
    if (n == 2 && c[0] == '.' && c[1] == '.') {
      fprintf(stderr, "tar: %s: Member name contains '..'\n", orig);
      rc = 2;
      return 0;
    }
    c += n;
    while (*c == '/') c++;
  }
  for (int i = 0; i < strip_n; i++) {
    char *s = strchr(p, '/');
    if (!s) return 0;
    while (*s == '/') s++;
    p = s;
  }
  if (!*p) return 0;
  return p;
}

/* Creates the parent directories of path. */
static void make_parents(char *path) {
  for (char *s = path + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    if (mkdir(path, 0755) < 0 && errno != EEXIST) { /* reported when the file itself fails */ }
    *s = '/';
  }
}

/* With symbolic links in play, refuse to write through one that this
 * archive (or anyone) placed in a parent directory: a hostile archive could
 * otherwise write anywhere. */
static int parent_is_symlink(char *path) {
  if (!made_symlink) return 0;
  for (char *s = path + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    struct stat st;
    int bad = lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    *s = '/';
    if (bad) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------- extraction */

struct dirfix { char *path; unsigned mode; long long mtime; };
static struct dirfix *dirs;
static int ndirs, capdirs;

static void set_times(const char *path, long long mtime, int nofollow) {
  if (no_mtime) return;
  struct timespec ts[2] = {{mtime, 0}, {mtime, 0}};
  if (utimensat(AT_FDCWD, path, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0) < 0 && errno == ENOSYS) unsupported(U_TIME);
}

static void set_owner(const char *path, const tar_entry *e) {
  if (!same_owner) return;
  if (lchown(path, e->uid, e->gid) < 0) {
    if (errno == ENOSYS) unsupported(U_CHOWN);
    else if (errno != EPERM) err(path, strerror(errno));
  }
}

static void set_mode(const char *path, unsigned mode) {
  if (chmod(path, mode) < 0) {
    if (errno == ENOSYS) unsupported(U_CHMOD);
    else err(path, strerror(errno));
  }
}

/* Makes room for a new entry at path. */
static int clear_path(const char *path) {
  struct stat st;
  if (lstat(path, &st) < 0) return 0;
  if (keep_old) { err(path, "Cannot open: File exists"); return -1; }
  if (S_ISDIR(st.st_mode)) return 0;
  unlink(path);
  return 0;
}

static int copy_file(const char *from, const char *to, unsigned mode) {
  int in = open(from, O_RDONLY);
  if (in < 0) return -1;
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
  if (out < 0) { close(in); return -1; }
  long r;
  int bad = 0;
  while ((r = read(in, buf, sizeof buf)) > 0) if (write_all(out, buf, r) < 0) { bad = 1; break; }
  if (r < 0) bad = 1;
  close(in);
  if (close(out) < 0) bad = 1;
  return bad ? -1 : 0;
}

static void extract_one(arc_stream *s, tar_entry *e) {
  char orig[4096];
  strcpy(orig, e->path);
  if (verbose) list_entry(to_stdout ? stderr : stdout, e); /* the name as stored, like GNU tar */
  char *path = safe_name(e->path, strip, orig);
  if (!path) return;
  char type = e->type;
  if (type == '5') { size_t n = strlen(path); while (n > 1 && path[n - 1] == '/') path[--n] = 0; }
  if (to_stdout) {
    if (type != '0' && type != '7') return;
    long r;
    while ((r = tar_read(s, buf, sizeof buf)) > 0)
      if (write_all(1, buf, r) < 0) { err("stdout", strerror(errno)); return; }
    return;
  }
  make_parents(path);
  if (parent_is_symlink(path)) { err(path, "Cannot extract through a symbolic link"); return; }
  switch (type) {
  case '5': {
    if (mkdir(path, 0700 | (e->mode & 0777)) < 0 && errno != EEXIST) { err(path, strerror(errno)); return; }
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) { err(path, "Cannot mkdir: Not a directory"); return; }
    if (ndirs == capdirs) {
      capdirs = capdirs ? capdirs * 2 : 64;
      struct dirfix *nd = realloc(dirs, capdirs * sizeof *dirs);
      if (!nd) { err(path, "out of memory"); return; }
      dirs = nd;
    }
    dirs[ndirs].path = strdup(path);
    dirs[ndirs].mode = e->mode;
    dirs[ndirs].mtime = e->mtime;
    ndirs++;
    set_owner(path, e);
    return;
  }
  case '2':
    if (clear_path(path) < 0) return;
    if (symlink(e->link, path) < 0) {
      if (errno == ENOSYS) unsupported(U_SYMLINK);
      else err(path, strerror(errno));
      return;
    }
    made_symlink = 1;
    set_owner(path, e);
    set_times(path, e->mtime, 1);
    return;
  case '1': {
    char lorig[4096];
    strcpy(lorig, e->link);
    char *target = safe_name(e->link, strip, lorig);
    if (!target) { err(path, "hard link target is outside the extraction"); return; }
    if (clear_path(path) < 0) return;
    if (link(target, path) < 0) {
      if (errno != ENOSYS) { err(path, strerror(errno)); return; }
      unsupported(U_LINK);
      struct stat st;
      if (stat(target, &st) < 0 || copy_file(target, path, st.st_mode) < 0) err(path, "cannot copy the hard link target");
    }
    return;
  }
  case '3': case '4': case '6': {
    if (clear_path(path) < 0) return;
    int r;
    if (type == '6') r = mkfifo(path, e->mode & 07777);
    else r = mknod(path, (e->mode & 07777) | (type == '3' ? S_IFCHR : S_IFBLK),
                   ((unsigned long long)e->devmajor << 8) | (e->devminor & 0xff) | ((unsigned long long)(e->devminor & ~0xffu) << 12));
    if (r < 0) {
      if (errno == ENOSYS) unsupported(type == '6' ? U_FIFO : U_DEV);
      else err(path, strerror(errno));
      return;
    }
    set_owner(path, e);
    set_times(path, e->mtime, 0);
    return;
  }
  default:
    if (type != '0' && type != '7') fprintf(stderr, "tar: %s: Unknown file type '%c', extracted as normal file\n", path, type);
    break;
  }
  if (clear_path(path) < 0) return;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, e->mode & 0777);
  if (fd < 0) { err(path, strerror(errno)); return; }
  long r;
  int bad = 0;
  while ((r = tar_read(s, buf, sizeof buf)) > 0)
    if (!bad && write_all(fd, buf, r) < 0) { err(path, strerror(errno)); bad = 1; }
  if (close(fd) < 0 && !bad) err(path, strerror(errno));
  if (r < 0) return; /* the archive is broken; the caller reports it */
  set_owner(path, e);
  if (e->mode & 07000 || same_perms) set_mode(path, e->mode & 07777);
  set_times(path, e->mtime, 0);
}

static int open_archive_read(void) {
  if (!archive || !strcmp(archive, "-")) return 0;
  int fd = open(archive, O_RDONLY);
  if (fd < 0) { fprintf(stderr, "tar: %s: Cannot open: %s\n", archive, strerror(errno)); exit(2); }
  return fd;
}

static int read_archive(const char *dir) {
  int fd = open_archive_read();
  if (dir && op == 'x' && !to_stdout && chdir(dir) < 0) {
    fprintf(stderr, "tar: %s: Cannot open: %s\n", dir, strerror(errno));
    return 2;
  }
  arc_stream *s = arc_open_fd(zfmt >= 0 ? zfmt : ARC_AUTO, fd);
  if (!s) { fprintf(stderr, "tar: out of memory\n"); return 2; }
  tar_entry *e = malloc(sizeof *e);
  if (!e) { fprintf(stderr, "tar: out of memory\n"); return 2; }
  int r, n = 0;
  while ((r = tar_next(s, e)) == 1) {
    n++;
    if (!member_match(e->path)) continue;
    if (op == 't') list_entry(stdout, e);
    else extract_one(s, e);
  }
  if (r < 0) {
    fprintf(stderr, "tar: %s\n", arc_error(s));
    if (n == 0) fprintf(stderr, "tar: Error is not recoverable: exiting now\n");
    rc = 2;
  }
  /* directories last: writing files into them changed their times */
  for (int i = ndirs - 1; i >= 0; i--) {
    if (same_perms || (dirs[i].mode & 07000)) set_mode(dirs[i].path, dirs[i].mode & 07777);
    set_times(dirs[i].path, dirs[i].mtime, 0);
    free(dirs[i].path);
  }
  free(e);
  arc_close(s);
  if (fd) close(fd);
  for (int i = 0; i < nmembers; i++)
    if (!member_found[i]) { fprintf(stderr, "tar: %s: Not found in archive\n", members[i]); rc = 2; }
  fflush(stdout);
  if (unsupported_count && strict) {
    fprintf(stderr, "tar: %d item%s could not be restored exactly (--strict)\n", unsupported_count, unsupported_count == 1 ? "" : "s");
    rc = 2;
  }
  return rc;
}

/* --------------------------------------------------------------- creation */

struct seen { unsigned long long dev, ino; char *path; };
static struct seen *seen;
static int nseen, capseen;
static unsigned long long out_dev, out_ino;
static int out_known;

static void fill_owner(tar_entry *e, const struct stat *st) {
  e->uid = owner_uid >= 0 ? (unsigned)owner_uid : st->st_uid;
  e->gid = owner_gid >= 0 ? (unsigned)owner_gid : st->st_gid;
  if (owner_name[0]) strcpy(e->uname, owner_name);
  else if (!numeric && e->uid == 0) strcpy(e->uname, "root");
  if (group_name[0]) strcpy(e->gname, group_name);
  else if (!numeric && e->gid == 0) strcpy(e->gname, "root");
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_path(arc_writer *w, const char *path);

static void add_dir_contents(arc_writer *w, const char *path) {
  DIR *d = opendir(path);
  if (!d) { err(path, strerror(errno)); return; }
  char **names = 0;
  int n = 0, cap = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      char **nn = realloc(names, cap * sizeof *names);
      if (!nn) { err(path, "out of memory"); break; }
      names = nn;
    }
    names[n++] = strdup(de->d_name);
  }
  closedir(d);
  if (n > 1) qsort(names, n, sizeof *names, cmp_str);  /* the same tree always gives the same archive */
  for (int i = 0; i < n; i++) {
    char *child = malloc(strlen(path) + strlen(names[i]) + 2);
    size_t pl = strlen(path);
    sprintf(child, "%s%s%s", path, pl && path[pl - 1] == '/' ? "" : "/", names[i]);
    add_path(w, child);
    free(child);
    free(names[i]);
  }
  free(names);
}

static void add_path(arc_writer *w, const char *path) {
  struct stat st;
  if (lstat(path, &st) < 0) { err(path, strerror(errno)); return; }
  if (out_known && st.st_dev == out_dev && st.st_ino == out_ino && S_ISREG(st.st_mode)) {
    fprintf(stderr, "tar: %s: file is the archive; not dumped\n", path);
    return;
  }
  tar_entry *e = calloc(1, sizeof *e);
  if (!e) { err(path, "out of memory"); return; }
  const char *name = path;
  while (name[0] == '/') {
    name++;
    if (!warned_abs++) fprintf(stderr, "tar: Removing leading '/' from member names\n");
  }
  if (!*name) name = ".";
  size_t nl = strlen(name);
  if (nl + 2 >= sizeof e->path) { err(path, "name too long"); free(e); return; }
  strcpy(e->path, name);
  e->mode = st.st_mode & 07777;
  e->mtime = st.st_mtime;
  fill_owner(e, &st);
  int is_dir = 0, data_fd = -1;
  if (S_ISDIR(st.st_mode)) {
    e->type = '5';
    if (e->path[nl - 1] != '/') strcat(e->path, "/");
    is_dir = 1;
  } else if (S_ISLNK(st.st_mode)) {
    e->type = '2';
    long n = readlink(path, e->link, sizeof e->link - 1);
    if (n < 0) { err(path, strerror(errno)); free(e); return; }
    e->link[n] = 0;
  } else if (S_ISREG(st.st_mode)) {
    e->type = '0';
    if (st.st_nlink > 1) {
      for (int i = 0; i < nseen; i++)
        if (seen[i].dev == st.st_dev && seen[i].ino == st.st_ino) { e->type = '1'; strcpy(e->link, seen[i].path); break; }
      if (e->type == '0') {
        if (nseen == capseen) {
          int nc = capseen ? capseen * 2 : 32;
          struct seen *ns = realloc(seen, nc * sizeof *seen);
          if (!ns) { err(path, "out of memory"); free(e); return; }
          seen = ns;
          capseen = nc;
        }
        seen[nseen].dev = st.st_dev;
        seen[nseen].ino = st.st_ino;
        seen[nseen].path = strdup(e->path);
        nseen++;
      }
    }
    if (e->type == '0') {
      data_fd = open(path, O_RDONLY);
      if (data_fd < 0) { err(path, strerror(errno)); free(e); return; }
      e->size = st.st_size;
    }
  } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
    e->type = S_ISCHR(st.st_mode) ? '3' : '4';
    unsigned long long d = st.st_rdev;
    e->devmajor = (unsigned)(((d >> 8) & 0xfff) | ((d >> 32) & ~0xfffull));
    e->devminor = (unsigned)((d & 0xff) | ((d >> 12) & ~0xffull));
  } else if (S_ISFIFO(st.st_mode)) {
    e->type = '6';
  } else {
    fprintf(stderr, "tar: %s: socket ignored\n", path);
    free(e);
    return;
  }
  if (verbose) list_entry(archive && strcmp(archive, "-") ? stdout : stderr, e);
  if (tar_write_header(w, e) < 0) { err(path, "write error"); if (data_fd >= 0) close(data_fd); free(e); return; }
  if (data_fd >= 0) {
    unsigned long long left = e->size;
    while (left) {
      long r = read(data_fd, buf, left < sizeof buf ? (size_t)left : sizeof buf);
      if (r <= 0) {
        fprintf(stderr, "tar: %s: File shrank by %llu bytes; padding with zeros\n", path, left);
        rc = 2;
        memset(buf, 0, sizeof buf);
        while (left) {
          size_t k = left < sizeof buf ? (size_t)left : sizeof buf;
          if (tar_write(w, buf, k) != (long)k) break;
          left -= k;
        }
        break;
      }
      if (tar_write(w, buf, r) != r) { err(path, "write error"); break; }
      left -= r;
    }
    close(data_fd);
  }
  free(e);
  if (is_dir) add_dir_contents(w, path);
}

static int create_archive(int nargs, char **args, char **chdirs) {
  if (!nargs) { fprintf(stderr, "tar: Cowardly refusing to create an empty archive\n"); return 2; }
  int fd = 1;
  if (archive && strcmp(archive, "-")) {
    fd = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "tar: %s: Cannot open: %s\n", archive, strerror(errno)); return 2; }
    struct stat st;
    if (fstat(fd, &st) == 0) { out_dev = st.st_dev; out_ino = st.st_ino; out_known = 1; }
  } else if (isatty(1)) {
    fprintf(stderr, "tar: Refusing to write archive contents to terminal (missing -f option?)\n");
    return 2;
  }
  int f = zfmt;
  if (f < 0 && auto_compress && archive) {
    size_t n = strlen(archive);
#define ENDS(s) (n >= sizeof s - 1 && !strcmp(archive + n - (sizeof s - 1), s))
    if (ENDS(".tgz") || ENDS(".gz") || ENDS(".taz")) f = ARC_GZIP;
    else if (ENDS(".xz") || ENDS(".txz")) f = ARC_XZ;
    else if (ENDS(".zst") || ENDS(".tzst")) f = ARC_ZSTD;
#undef ENDS
  }
  if (f == ARC_XZ || f == ARC_ZSTD) {
    fprintf(stderr, "tar: %s compression is not supported (only decompression); use -z for gzip\n", f == ARC_XZ ? "xz" : "zstd");
    if (fd != 1) { close(fd); unlink(archive); }
    return 2;
  }
  arc_writer *w = arc_wopen_fd(f == ARC_GZIP ? ARC_GZIP : ARC_RAW, -1, fd);
  if (!w) { fprintf(stderr, "tar: out of memory\n"); return 2; }
  for (int i = 0; i < nargs; i++) {
    if (chdirs[i] && chdir(chdirs[i]) < 0) { fprintf(stderr, "tar: %s: Cannot open: %s\n", chdirs[i], strerror(errno)); rc = 2; break; }
    add_path(w, args[i]);
  }
  if (tar_write_end(w) < 0 || arc_wclose(w) < 0) { fprintf(stderr, "tar: write error\n"); rc = 2; }
  else if (fd != 1 && close(fd) < 0) { fprintf(stderr, "tar: %s: %s\n", archive, strerror(errno)); rc = 2; }
  fflush(stdout);
  return rc;
}

/* ---------------------------------------------------------------- options */

static void usage(void) {
  printf("usage: tar {-c|-t|-x} [-vzJakmOp] [-f FILE] [-C DIR] [--zstd] [--strip-components=N] [file...]\n"
         "  -c  create   -t  list   -x  extract\n"
         "  -f FILE  archive file (- is standard input or output)\n"
         "  -v  verbose (twice for long listings)\n"
         "  -z  gzip   -J  xz (read only)   --zstd  zstd (read only)   -a  choose by suffix\n"
         "  -C DIR  change to DIR first\n"
         "  -k  keep existing files   -m  do not restore times   -O  extract to standard output\n"
         "  -p  restore permission bits exactly   --same-owner  restore owners\n"
         "  --strip-components=N   --owner=ID --group=ID --numeric-owner (create)\n"
         "  --strict  fail if links, owners or permissions could not be restored\n");
}

static int set_op(char c) {
  if (op && op != c) { fprintf(stderr, "tar: You may not specify more than one of -c, -t, -x\n"); return -1; }
  op = c;
  return 0;
}

/* --owner and --group take a number (+N or N) or a name. There is no user
 * database, so the only name that maps to a number is root. */
static void parse_owner(const char *v, long long *id, char *name) {
  const char *d = *v == '+' ? v + 1 : v;
  char *e;
  long long n = strtoll(d, &e, 10);
  if (*d && *e == 0) { *id = n; if (n == 0) strcpy(name, "root"); return; }
  snprintf(name, 32, "%s", v);
  if (!strcmp(v, "root")) *id = 0;
}

int main(int argc, char **argv) {
  char **files = calloc(argc + 1, sizeof *files), **chdirs = calloc(argc + 1, sizeof *chdirs);
  int nf = 0, opts = 1;
  const char *pending_chdir = 0;
  int need_f = 0, need_C = 0;
  if (getuid() == 0) same_owner = same_perms = 1;
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (need_f) { archive = a; need_f = 0; continue; }
    if (need_C) { pending_chdir = a; need_C = 0; continue; }
    int oldstyle = i == 1 && a[0] != '-';
    if (!opts || (a[0] != '-' && !oldstyle) || !strcmp(a, "-")) {
      files[nf] = a;
      chdirs[nf++] = (char *)pending_chdir;
      pending_chdir = 0;
      continue;
    }
    if (!strcmp(a, "--")) { opts = 0; continue; }
    if (a[0] == '-' && a[1] == '-') {
      const char *l = a + 2, *v = strchr(l, '=');
      size_t ln = v ? (size_t)(v - l) : strlen(l);
      if (v) v++;
#define IS(s) (ln == sizeof s - 1 && !strncmp(l, s, ln))
      if (IS("create")) { if (set_op('c')) return 2; }
      else if (IS("list")) { if (set_op('t')) return 2; }
      else if (IS("extract") || IS("get")) { if (set_op('x')) return 2; }
      else if (IS("file")) { if (v) archive = v; else need_f = 1; }
      else if (IS("directory")) { if (v) pending_chdir = v; else need_C = 1; }
      else if (IS("verbose")) verbose++;
      else if (IS("gzip") || IS("gunzip") || IS("ungzip")) zfmt = ARC_GZIP;
      else if (IS("xz")) zfmt = ARC_XZ;
      else if (IS("zstd")) zfmt = ARC_ZSTD;
      else if (IS("bzip2") || IS("lzma") || IS("lzip") || IS("lzop")) { fprintf(stderr, "tar: %s is not supported\n", a); return 2; }
      else if (IS("auto-compress")) auto_compress = 1;
      else if (IS("keep-old-files")) keep_old = 1;
      else if (IS("overwrite") || IS("unlink-first") || IS("no-recursion-off") || IS("sort")) {}
      else if (IS("touch")) no_mtime = 1;
      else if (IS("to-stdout")) to_stdout = 1;
      else if (IS("same-owner")) same_owner = 1;
      else if (IS("no-same-owner")) same_owner = 0;
      else if (IS("same-permissions") || IS("preserve-permissions")) same_perms = 1;
      else if (IS("no-same-permissions")) same_perms = 0;
      else if (IS("numeric-owner")) numeric = 1;
      else if (IS("strict")) strict = 1;
      else if (IS("strip-components") && v) strip = atoi(v);
      else if (IS("owner") && v) parse_owner(v, &owner_uid, owner_name);
      else if (IS("group") && v) parse_owner(v, &owner_gid, group_name);
      else if (IS("format") && v) {
        if (strcmp(v, "gnu") && strcmp(v, "oldgnu")) fprintf(stderr, "tar: note: writing the gnu format, not %s\n", v);
      } else if (IS("help")) { usage(); return 0; }
      else if (IS("version")) { printf("tar (UC.OS libarc) 1.0\n"); return 0; }
      else { fprintf(stderr, "tar: unrecognized option '%s'\n", a); usage(); return 2; }
#undef IS
      continue;
    }
    /* clustered short options; in the old style their arguments follow in order */
    char *f = a + (a[0] == '-');
    int argi = i;
    for (; *f; f++) {
      switch (*f) {
      case 'c': case 't': case 'x': if (set_op(*f)) return 2; break;
      case 'v': verbose++; break;
      case 'z': zfmt = ARC_GZIP; break;
      case 'J': zfmt = ARC_XZ; break;
      case 'j': fprintf(stderr, "tar: bzip2 is not supported\n"); return 2;
      case 'a': auto_compress = 1; break;
      case 'k': keep_old = 1; break;
      case 'm': no_mtime = 1; break;
      case 'O': to_stdout = 1; break;
      case 'p': same_perms = 1; break;
      case 'o': same_owner = 0; break;
      case 'h': break;
      case 'f': case 'C': {
        char which = *f;
        const char *v;
        if (!oldstyle && f[1]) { v = f + 1; f += strlen(f) - 1; }
        else if (argi + 1 < argc) v = argv[++argi];
        else { fprintf(stderr, "tar: option requires an argument -- '%c'\n", which); return 2; }
        if (which == 'f') archive = v;
        else pending_chdir = v;
        break;
      }
      default:
        fprintf(stderr, "tar: invalid option -- '%c'\n", *f);
        usage();
        return 2;
      }
    }
    i = argi;
  }
  if (need_f || need_C) { fprintf(stderr, "tar: option requires an argument\n"); return 2; }
  if (!op) { fprintf(stderr, "tar: You must specify one of -c, -t, -x\n"); usage(); return 2; }
  if (op == 'c') return create_archive(nf, files, chdirs);
  /* for -t and -x, -C applies to the whole run; names are members to select */
  for (int i = 0; i < nf; i++) if (chdirs[i] && !pending_chdir) pending_chdir = chdirs[i];
  members = files;
  nmembers = nf;
  member_found = calloc(nf + 1, 1);
  return read_archive(pending_chdir); /* the archive is opened before changing directory, as GNU tar does */
}
