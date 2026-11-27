// SPDX-License-Identifier: GPL-2.0-or-later
/* Native test driver for libarc. Built by run.sh with the host compiler.
 *   arctool d [gzip|xz|zstd|zlib|deflate|raw|auto] [chunk]  < in > out   decompress
 *   arctool c [gzip|zlib|deflate] [level]                   < in > out   compress
 *   arctool sum [md5|sha256|sha512|crc32|crc64|adler32|xxh64] < in        print a checksum
 *   arctool tar [auto]           < archive                  list entries: type mode uid gid size mtime path -> link
 *   arctool tarcat NAME          < archive                  print one member's data
 *   arctool ar                   < archive                  list members of an ar archive with a sha256 of each
 * chunk: the largest read the source callback returns (to test refills). */
#include "../arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long chunk = 1 << 16;
static long src(void *ctx, void *buf, unsigned long n) {
  (void)ctx;
  if (n > chunk) n = chunk;
  return read(0, buf, n);
}
static long sink(void *ctx, const void *buf, unsigned long n) {
  (void)ctx;
  return write(1, buf, n);
}
static int fmt_of(const char *s) {
  if (!strcmp(s, "gzip")) return ARC_GZIP;
  if (!strcmp(s, "xz")) return ARC_XZ;
  if (!strcmp(s, "zstd")) return ARC_ZSTD;
  if (!strcmp(s, "zlib")) return ARC_ZLIB;
  if (!strcmp(s, "deflate")) return ARC_DEFLATE;
  if (!strcmp(s, "raw")) return ARC_RAW;
  return ARC_AUTO;
}
static void hex(const unsigned char *p, int n) {
  for (int i = 0; i < n; i++) printf("%02x", p[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  static unsigned char buf[1 << 16];
  if (!strcmp(argv[1], "d")) {
    if (argc > 3) chunk = strtoul(argv[3], 0, 0);
    arc_stream *s = arc_open(argc > 2 ? fmt_of(argv[2]) : ARC_AUTO, src, 0);
    long r;
    while ((r = arc_read(s, buf, sizeof buf)) > 0)
      if (write(1, buf, r) != r) return 3;
    if (r < 0) { fprintf(stderr, "arctool: %s\n", arc_error(s)); arc_close(s); return 1; }
    arc_close(s);
    return 0;
  }
  if (!strcmp(argv[1], "c")) {
    arc_writer *w = arc_wopen(argc > 2 ? fmt_of(argv[2]) : ARC_GZIP, argc > 3 ? atoi(argv[3]) : -1, sink, 0);
    if (!w) return 2;
    long r;
    while ((r = read(0, buf, (argc > 4 ? strtoul(argv[4], 0, 0) : sizeof buf))) > 0)
      if (arc_write(w, buf, r) != r) break;
    if (arc_wclose(w) < 0) { fprintf(stderr, "arctool: write failed\n"); return 1; }
    return 0;
  }
  if (!strcmp(argv[1], "sum")) {
    const char *k = argc > 2 ? argv[2] : "sha256";
    arc_md5 m; arc_sha256 s2; arc_sha512 s5;
    arc_md5_init(&m); arc_sha256_init(&s2); arc_sha512_init(&s5);
    unsigned c32 = 0, ad = 1; unsigned long long c64 = 0;
    unsigned char *all = 0; size_t alln = 0;
    long r;
    while ((r = read(0, buf, sizeof buf)) > 0) {
      arc_md5_update(&m, buf, r); arc_sha256_update(&s2, buf, r); arc_sha512_update(&s5, buf, r);
      c32 = arc_crc32(c32, buf, r); c64 = arc_crc64(c64, buf, r); ad = arc_adler32(ad, buf, r);
      all = realloc(all, alln + r); memcpy(all + alln, buf, r); alln += r;
    }
    unsigned char out[64];
    if (!strcmp(k, "md5")) { arc_md5_final(&m, out); hex(out, 16); }
    else if (!strcmp(k, "sha256")) { arc_sha256_final(&s2, out); hex(out, 32); }
    else if (!strcmp(k, "sha512")) { arc_sha512_final(&s5, out); hex(out, 64); }
    else if (!strcmp(k, "crc32")) printf("%08x\n", c32);
    else if (!strcmp(k, "crc64")) printf("%016llx\n", c64);
    else if (!strcmp(k, "adler32")) printf("%08x\n", ad);
    else if (!strcmp(k, "xxh64")) printf("%016llx\n", arc_xxh64(all, alln, 0));
    free(all);
    return 0;
  }
  if (!strcmp(argv[1], "tar") || !strcmp(argv[1], "tarcat")) {
    arc_stream *s = arc_open(ARC_AUTO, src, 0);
    static tar_entry e;
    int r;
    while ((r = tar_next(s, &e)) == 1) {
      if (!strcmp(argv[1], "tar")) {
        printf("%c %o %u %u %llu %lld %s", e.type, e.mode, e.uid, e.gid, e.size, e.mtime, e.path);
        if (e.link[0]) printf(" -> %s", e.link);
        printf("\n");
      } else if (argc > 2 && !strcmp(e.path, argv[2])) {
        long n;
        while ((n = tar_read(s, buf, sizeof buf)) > 0) fwrite(buf, 1, n, stdout);
        if (n < 0) { fprintf(stderr, "arctool: %s\n", arc_error(s)); return 1; }
      }
    }
    if (r < 0) { fprintf(stderr, "arctool: %s\n", arc_error(s)); arc_close(s); return 1; }
    arc_close(s);
    return 0;
  }
  if (!strcmp(argv[1], "ar")) {
    if (ar_begin(src, 0) < 0) { fprintf(stderr, "arctool: not an ar archive\n"); return 1; }
    ar_member m;
    int r;
    while ((r = ar_next(src, 0, &m)) == 1) {
      arc_sha256 c;
      arc_sha256_init(&c);
      unsigned long left = m.size;
      while (left) {
        long n = src(0, buf, left < sizeof buf ? left : sizeof buf);
        if (n <= 0) { fprintf(stderr, "arctool: truncated\n"); return 1; }
        arc_sha256_update(&c, buf, n);
        left -= n;
      }
      if (ar_skip_pad(src, 0, &m) < 0) { fprintf(stderr, "arctool: bad pad\n"); return 1; }
      unsigned char out[32];
      arc_sha256_final(&c, out);
      printf("%s %lu %o ", m.name, m.size, m.mode);
      hex(out, 32);
    }
    if (r < 0) { fprintf(stderr, "arctool: bad ar archive\n"); return 1; }
    return 0;
  }
  return 2;
}
