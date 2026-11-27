// SPDX-License-Identifier: GPL-2.0-or-later
/* gzip, gunzip, zcat; xz, unxz, xzcat; zstd, unzstd, zstdcat.
 * One program: what it does depends on the name it was started under
 * (bin/gzip/ALIASES installs the other names). gzip compresses and
 * decompresses; xz and zstd only decompress, test and list.
 * Options follow GNU gzip, xz and zstd so scripts written for Linux work. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "arc.h"

enum { T_GZIP, T_XZ, T_ZSTD };
static const char *prog = "gzip";
static int tool, fmt;
static int decompress, to_stdout, keep, force, test, list, quiet, verbose, no_name, rm_src = 1;
static int level = 6, rc;
static const char *suffix, *outname;
static unsigned long long memlimit;
static int listed_header;

static void warn(const char *name, const char *msg) {
  if (!quiet) fprintf(stderr, "%s: %s: %s\n", prog, name, msg);
  if (rc == 0) rc = 2;
}
static void fail(const char *name, const char *msg) {
  fprintf(stderr, "%s: %s: %s\n", prog, name, msg);
  rc = 1;
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

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* Output name for a compressed input, or NULL when the suffix is unknown. */
static char *strip_suffix(const char *name) {
  static const char *const gz[][2] = {{".gz", ""}, {"-gz", ""}, {".z", ""}, {"-z", ""}, {"_z", ""}, {".tgz", ".tar"}, {".taz", ".tar"}, {0, 0}};
  static const char *const xz[][2] = {{".xz", ""}, {".txz", ".tar"}, {0, 0}};
  static const char *const zs[][2] = {{".zst", ""}, {".tzst", ".tar"}, {".zstd", ""}, {0, 0}};
  const char *const(*t)[2] = tool == T_XZ ? xz : tool == T_ZSTD ? zs : gz;
  size_t n = strlen(name);
  if (suffix) {
    size_t k = strlen(suffix);
    if (n > k && !strcmp(name + n - k, suffix)) return strndup(name, n - k);
  }
  for (; t[0][0]; t++) {
    size_t k = strlen(t[0][0]);
    if (n > k && !strcmp(name + n - k, t[0][0]) && name[n - k - 1] != '/') {
      char *o = malloc(n - k + strlen(t[0][1]) + 1);
      if (!o) return 0;
      memcpy(o, name, n - k);
      strcpy(o + n - k, t[0][1]);
      return o;
    }
  }
  return 0;
}

static void copy_times(const char *dst, const struct stat *st) {
  struct timespec ts[2] = {{st->st_mtime, 0}, {st->st_mtime, 0}};
  utimensat(AT_FDCWD, dst, ts, 0); /* best effort, as gzip does */
}

static void show_list(const char *name, unsigned long long in, unsigned long long out) {
  if (!listed_header++) printf("%20s %20s  ratio uncompressed_name\n", "compressed", "uncompressed");
  double ratio = out ? 100.0 * (1.0 - (double)in / (double)out) : 0.0;
  printf("%20llu %20llu %5.1f%% %s\n", in, out, ratio, name);
}

static void do_decompress(const char *name) {
  int std_in = !name || !strcmp(name, "-");
  const char *shown = std_in ? "stdin" : name;
  int in = 0, out = -1, pass = 0;
  char *oname = 0;
  struct stat st;
  memset(&st, 0, sizeof st);
  if (!std_in) {
    in = open(name, O_RDONLY);
    if (in < 0) { fail(name, strerror(errno)); return; }
    fstat(in, &st);
    if (S_ISDIR(st.st_mode)) { warn(name, "is a directory -- ignored"); close(in); return; }
  }
  int sink = test || list;
  if (sink) out = -1;
  else if (to_stdout || (std_in && !outname)) out = 1;
  else {
    oname = outname ? strdup(outname) : strip_suffix(name);
    if (!oname) { warn(name, "unknown suffix -- ignored"); close(in); return; }
    out = open(oname, O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL), st.st_mode & 0777 ? st.st_mode & 0777 : 0644);
    if (out < 0) {
      fail(oname, errno == EEXIST ? "already exists; not overwritten (use -f)" : strerror(errno));
      free(oname); close(in); return;
    }
  }
  /* zcat -f and friends pass data that is not compressed through unchanged */
  int want = force && (to_stdout || std_in) && !sink ? ARC_AUTO : fmt;
  int bad = 0;
  arc_stream *s = arc_open_fd(want, in);
  if (!s) { fail(shown, "out of memory"); bad = 1; goto done; }
  if (memlimit) arc_set_memlimit(s, memlimit);
  static unsigned char buf[65536];
  long r;
  while ((r = arc_read(s, buf, sizeof buf)) > 0) {
    if (want == ARC_AUTO && arc_format(s) != fmt) pass = 1;
    if (out >= 0 && write_all(out, buf, r) < 0) { fail(oname ? oname : "stdout", strerror(errno)); bad = 1; break; }
  }
  if (!bad && r < 0) { fail(shown, arc_error(s)); bad = 1; }
  if (!bad && r == 0 && arc_total_in(s) == 0 && !pass && want != ARC_AUTO) { fail(shown, "unexpected end of file"); bad = 1; }
  if (!bad) {
    if (list) {
      char *plain = std_in ? 0 : strip_suffix(name);
      show_list(plain ? plain : shown, arc_total_in(s), arc_total_out(s));
      free(plain);
    }
    else if (test && verbose) fprintf(stderr, "%s:\t OK\n", shown);
    else if (verbose && oname) {
      double ratio = arc_total_out(s) ? 100.0 * (1.0 - (double)arc_total_in(s) / (double)arc_total_out(s)) : 0.0;
      fprintf(stderr, "%s:\t%5.1f%% -- replaced with %s\n", name, ratio, oname);
    }
  }
  arc_close(s);
done:
  if (oname) {
    close(out);
    if (bad) unlink(oname);
    else {
      copy_times(oname, &st);
      if (!keep && rm_src && !std_in && unlink(name) < 0) warn(name, strerror(errno));
    }
    free(oname);
  }
  if (!std_in) close(in);
  (void)pass;
}

static void do_compress(const char *name) {
  int std_in = !name || !strcmp(name, "-");
  const char *shown = std_in ? "stdin" : name;
  const char *suf = suffix ? suffix : ".gz";
  int in = 0, out = 1;
  char *oname = 0;
  struct stat st;
  memset(&st, 0, sizeof st);
  if (!std_in) {
    size_t n = strlen(name), k = strlen(suf);
    if (n > k && !strcmp(name + n - k, suf)) { warn(name, "already has the suffix -- unchanged"); return; }
    in = open(name, O_RDONLY);
    if (in < 0) { fail(name, strerror(errno)); return; }
    fstat(in, &st);
    if (S_ISDIR(st.st_mode)) { warn(name, "is a directory -- ignored"); close(in); return; }
  }
  if (test || list) { /* nothing to do for plain input; gzip -t on a plain file fails */
    fail(shown, "not in gzip format");
    if (!std_in) close(in);
    return;
  }
  if (!to_stdout && !std_in) {
    oname = malloc(strlen(name) + strlen(suf) + 1);
    if (!oname) { fail(name, "out of memory"); close(in); return; }
    strcpy(oname, name);
    strcat(oname, suf);
    out = open(oname, O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL), st.st_mode & 0777 ? st.st_mode & 0777 : 0644);
    if (out < 0) {
      fail(oname, errno == EEXIST ? "already exists; not overwritten (use -f)" : strerror(errno));
      free(oname); close(in); return;
    }
  } else if (isatty(1) && !force) {
    fprintf(stderr, "%s: compressed data not written to a terminal. Use -f to force compression.\n", prog);
    rc = 1;
    if (!std_in) close(in);
    return;
  }
  arc_writer *w = arc_wopen_fd(ARC_GZIP, level, out);
  int bad = 0;
  if (!w) { fail(shown, "out of memory"); bad = 1; goto done; }
  if (!no_name && !std_in) arc_wgzip_info(w, base_name(name), (unsigned)st.st_mtime);
  static unsigned char buf[65536];
  long r;
  while ((r = read(in, buf, sizeof buf)) > 0)
    if (arc_write(w, buf, r) != r) { fail(oname ? oname : "stdout", "write error"); bad = 1; break; }
  if (r < 0) { fail(shown, strerror(errno)); bad = 1; }
  if (arc_wfinish(w) < 0 && !bad) { fail(oname ? oname : "stdout", "write error"); bad = 1; }
  if (!bad && verbose && oname) {
    unsigned long long a = arc_wtotal_in(w), b = arc_wtotal_out(w);
    fprintf(stderr, "%s:\t%5.1f%% -- replaced with %s\n", name, a ? 100.0 * (1.0 - (double)b / (double)a) : 0.0, oname);
  }
  arc_wclose(w);
done:
  if (oname) {
    close(out);
    if (bad) unlink(oname);
    else {
      copy_times(oname, &st);
      if (!keep && unlink(name) < 0) warn(name, strerror(errno));
    }
    free(oname);
  }
  if (!std_in) close(in);
}

static unsigned long long parse_size(const char *s) {
  char *e;
  unsigned long long v = strtoull(s, &e, 10);
  if (*e == 'k' || *e == 'K') v <<= 10;
  else if (*e == 'm' || *e == 'M') v <<= 20;
  else if (*e == 'g' || *e == 'G') v <<= 30;
  return v;
}

static void usage(void) {
  if (tool == T_GZIP)
    printf("usage: %s [-cdfhklnNqtvV19] [-S suffix] [file...]\n"
           "  -c  write to standard output, keep the input files\n"
           "  -d  decompress\n  -f  overwrite files; with -c pass data that is not compressed through\n"
           "  -k  keep the input files\n  -l  list sizes\n  -n  do not save the name and time when compressing\n"
           "  -q  quiet\n  -t  test the compressed files\n  -v  verbose\n  -1 .. -9  fast .. best (default 6)\n", prog);
  else
    printf("usage: %s -d [-cfhkqtv] [-o file] [file...]\n"
           "  This version decompresses, tests (-t) and lists (-l); it does not compress.\n"
           "  -c  write to standard output\n  -f  overwrite files\n  -k  keep the input files\n"
           "  -M SIZE, --memory=SIZE  largest window or dictionary to accept\n", prog);
}

int main(int argc, char **argv) {
  const char *me = base_name(argv[0]);
  if (strstr(me, "xz")) { tool = T_XZ; fmt = ARC_XZ; prog = "xz"; }
  else if (strstr(me, "zst")) { tool = T_ZSTD; fmt = ARC_ZSTD; prog = "zstd"; rm_src = 0; }
  else { tool = T_GZIP; fmt = ARC_GZIP; prog = "gzip"; }
  if (!strcmp(me, "gunzip") || !strcmp(me, "unxz") || !strcmp(me, "unzstd")) decompress = 1;
  if (!strcmp(me, "zcat") || !strcmp(me, "xzcat") || !strcmp(me, "zstdcat")) decompress = to_stdout = 1;
  if (!strcmp(me, "zstdcat")) force = 1;

  char **files = calloc(argc + 1, sizeof *files);
  int nf = 0, opts = 1;
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (!opts || a[0] != '-' || !a[1]) { files[nf++] = a; continue; }
    if (!strcmp(a, "--")) { opts = 0; continue; }
    if (a[1] == '-') {
      const char *l = a + 2, *v = strchr(l, '=');
      size_t ln = v ? (size_t)(v - l) : strlen(l);
#define IS(s) (ln == sizeof s - 1 && !strncmp(l, s, ln))
      if (IS("decompress") || IS("uncompress")) decompress = 1;
      else if (IS("stdout") || IS("to-stdout")) to_stdout = 1;
      else if (IS("keep")) keep = 1;
      else if (IS("force")) force = 1;
      else if (IS("test")) test = decompress = 1;
      else if (IS("list")) list = decompress = 1;
      else if (IS("quiet") || IS("silent")) quiet = 1;
      else if (IS("verbose")) verbose = 1;
      else if (IS("no-name")) no_name = 1;
      else if (IS("name")) no_name = 0;
      else if (IS("fast")) level = 1;
      else if (IS("best")) level = 9;
      else if (IS("rm")) rm_src = 1;
      else if (IS("no-progress") || IS("no-check") || IS("single-stream") || IS("rsyncable") || IS("no-sparse") || IS("sparse")) {}
      else if (IS("suffix") && v) suffix = v + 1;
      else if ((IS("memory") || IS("memlimit") || IS("memlimit-decompress")) && v) memlimit = parse_size(v + 1);
      else if (IS("threads") && v) {}
      else if (IS("help")) { usage(); return 0; }
      else if (IS("version")) { printf("%s (UC.OS libarc) 1.0\n", prog); return 0; }
      else { fprintf(stderr, "%s: unknown option '%s'\n", prog, a); usage(); return 1; }
#undef IS
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      char c = *f;
      if (c >= '0' && c <= '9') { level = c - '0'; if (level == 0) level = 1; continue; }
      switch (c) {
      case 'd': decompress = 1; break;
      case 'z': decompress = 0; break;
      case 'c': to_stdout = 1; break;
      case 'k': keep = 1; break;
      case 'f': force = 1; break;
      case 't': test = decompress = 1; break;
      case 'l': list = decompress = 1; break;
      case 'q': quiet = 1; break;
      case 'v': verbose = 1; break;
      case 'n': no_name = 1; break;
      case 'N': no_name = 0; break;
      case 'h': usage(); return 0;
      case 'V': printf("%s (UC.OS libarc) 1.0\n", prog); return 0;
      case 'S': case 'o': case 'T': case 'M': {
        const char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (!v) { fprintf(stderr, "%s: option -%c needs a value\n", prog, c); return 1; }
        if (c == 'S') suffix = v;
        else if (c == 'o') { outname = v; }
        else if (c == 'M') memlimit = parse_size(v);
        f += strlen(f) - 1;
        break;
      }
      default:
        fprintf(stderr, "%s: invalid option -- '%c'\n", prog, c);
        usage();
        return 1;
      }
    }
  }
  if (!decompress && tool != T_GZIP) {
    fprintf(stderr, "%s: this version cannot compress; use -d to decompress\n", prog);
    return 1;
  }
  if (outname && nf > 1) { fprintf(stderr, "%s: -o works with one input file only\n", prog); return 1; }
  if (outname && !strcmp(outname, "-")) { outname = 0; to_stdout = 1; }
  if (nf == 0) files[nf++] = "-";
  for (int i = 0; i < nf; i++) {
    if (decompress) do_decompress(files[i]);
    else do_compress(files[i]);
  }
  free(files);
  return rc;
}
