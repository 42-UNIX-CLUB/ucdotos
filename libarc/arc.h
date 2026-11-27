// SPDX-License-Identifier: GPL-2.0-or-later
/* libarc: archives, compression and checksums for UC.OS.
 *
 * Decoders: gzip, zlib and raw deflate (RFC 1950, 1951, 1952), xz with LZMA2
 * (and the delta filter), Zstandard (RFC 8878). Encoder: deflate for gzip,
 * zlib and raw deflate. Containers: ar (read and write), tar (read and write).
 * Checksums: CRC32, CRC64, Adler-32, XXH64, MD5, SHA-256, SHA-512.
 *
 * Every decoder treats its input as hostile: it checks every length against
 * its buffers, caps the memory a header may ask for, and verifies every
 * integrity check the format carries. Memory use depends on the window or
 * dictionary size, never on the length of the data.
 *
 * The library is plain C with no system dependency except read() and write()
 * in the two *_fd helpers, so it also builds with the host compiler for tests. */
#ifndef ARC_H
#define ARC_H

/* ---------------------------------------------------------------------------
 * Decompression: a pull stream. The caller supplies compressed bytes through
 * a callback. */
typedef long (*arc_src)(void *ctx, void *buf, unsigned long n);   /* returns bytes read, 0 at end, <0 on error */
enum { ARC_AUTO, ARC_RAW, ARC_GZIP, ARC_XZ, ARC_ZSTD, ARC_ZLIB };  /* ARC_AUTO sniffs the magic bytes */
typedef struct arc_stream arc_stream;
arc_stream *arc_open(int format, arc_src src, void *ctx);          /* NULL when out of memory */
long arc_read(arc_stream *s, void *buf, unsigned long n);          /* decompressed bytes, 0 at end, <0 on error */
const char *arc_error(arc_stream *s);                              /* text for the last error, "" if none */
void arc_close(arc_stream *s);
arc_stream *arc_open_fd(int format, int fd);                       /* convenience: source is a file descriptor */

/* ar archives (the outer layer of a .deb). Members are read in order. */
typedef struct {
  char name[64]; unsigned long size;
  /* additions (after the fixed fields): */
  unsigned long long size64;  /* the same size, never truncated */
  long long mtime;
  unsigned mode, uid, gid;
} ar_member;
int ar_begin(arc_src src, void *ctx);                              /* checks the "!<arch>\n" magic; 0 or -1 */
int ar_next(arc_src src, void *ctx, ar_member *m);                 /* 1 = a member follows, 0 = end, -1 = bad */
/* after ar_next the caller reads exactly m->size bytes (plus one pad byte if size is odd; ar_skip_pad does it) */
int ar_skip_pad(arc_src src, void *ctx, const ar_member *m);

/* tar (ustar, GNU long names and long links, pax extended headers for path, linkpath and size). */
typedef struct {
  char path[4096], link[4096];
  unsigned mode, uid, gid;
  unsigned long long size;
  long long mtime;
  char type;       /* '0' file, '1' hard link, '2' symlink, '3' char dev, '4' block dev, '5' directory, '6' fifo */
  unsigned devmajor, devminor;
  char uname[32], gname[32];
} tar_entry;
int tar_next(arc_stream *s, tar_entry *e);                         /* 1 = entry, 0 = end of archive, -1 = bad */
long tar_read(arc_stream *s, void *buf, unsigned long n);          /* the current entry's data; 0 when it is done */
/* tar_next skips whatever is left of the previous entry's data and its padding by itself */

/* Checksums. */
typedef struct { unsigned long long len; unsigned int h[8]; unsigned char buf[64]; unsigned n; } arc_sha256;
void arc_sha256_init(arc_sha256 *c);
void arc_sha256_update(arc_sha256 *c, const void *p, unsigned long n);
void arc_sha256_final(arc_sha256 *c, unsigned char out[32]);
unsigned arc_crc32(unsigned crc, const void *p, unsigned long n);
/* md5 and sha512 with the same three-call shape: arc_md5_*, arc_sha512_* */
typedef struct { unsigned long long len; unsigned int h[4]; unsigned char buf[64]; unsigned n; } arc_md5;
void arc_md5_init(arc_md5 *c);
void arc_md5_update(arc_md5 *c, const void *p, unsigned long n);
void arc_md5_final(arc_md5 *c, unsigned char out[16]);
typedef struct { unsigned long long len; unsigned long long h[8]; unsigned char buf[128]; unsigned n; } arc_sha512;
void arc_sha512_init(arc_sha512 *c);
void arc_sha512_update(arc_sha512 *c, const void *p, unsigned long n);
void arc_sha512_final(arc_sha512 *c, unsigned char out[64]);

/* ===========================================================================
 * Additions to the interface above. Nothing above this line changes. */

/* More formats for arc_open and arc_wopen. ARC_AUTO recognises gzip, xz and
 * zstd (with skippable frames); anything else is passed through as ARC_RAW.
 * zlib and raw deflate have no reliable magic, so they are never guessed. */
enum { ARC_DEFLATE = 6 };

/* Running checksums, all with the conventional start value 0:
 * crc = arc_crc32(0, p, n); crc = arc_crc32(crc, q, m); ... */
unsigned long long arc_crc64(unsigned long long crc, const void *p, unsigned long n); /* xz's CRC64 (ECMA-182) */
unsigned arc_adler32(unsigned adler, const void *p, unsigned long n);  /* start with 1, as in zlib */
unsigned long long arc_xxh64(const void *p, unsigned long n, unsigned long long seed);

/* Information about an open stream. */
int arc_format(arc_stream *s);                       /* ARC_GZIP, ...; ARC_AUTO until the first arc_read */
unsigned long long arc_total_in(arc_stream *s);      /* compressed bytes taken from the source so far */
unsigned long long arc_total_out(arc_stream *s);     /* decompressed bytes returned so far */
/* Memory cap for the window or dictionary a header may ask for. Call before
 * the first arc_read. The default is ARC_MEMLIMIT_DEFAULT. A stream that needs
 * more fails with an error saying so; nothing is allocated for it. */
#define ARC_MEMLIMIT_DEFAULT (128ull << 20)
void arc_set_memlimit(arc_stream *s, unsigned long long bytes);

/* ---------------------------------------------------------------------------
 * Compression: a push stream. The library hands compressed bytes to a sink. */
typedef long (*arc_sink)(void *ctx, const void *buf, unsigned long n);  /* returns n, or <0 on error */
typedef struct arc_writer arc_writer;
/* format: ARC_GZIP, ARC_ZLIB, ARC_DEFLATE or ARC_RAW (no compression, bytes
 * pass straight through; useful for an uncompressed tar). level: 0 (store
 * only) to 9 (smallest), or -1 for the default, 6. NULL when out of memory or
 * the format cannot be written. */
arc_writer *arc_wopen(int format, int level, arc_sink sink, void *ctx);
arc_writer *arc_wopen_fd(int format, int level, int fd);
/* gzip only, before the first arc_write: the file name and time to record in
 * the header (as gzip does by default). name may be NULL. */
void arc_wgzip_info(arc_writer *w, const char *name, unsigned mtime);
long arc_write(arc_writer *w, const void *buf, unsigned long n);  /* n, or <0 on error */
int arc_wfinish(arc_writer *w);           /* flushes and writes the trailer; 0 or -1 */
int arc_wclose(arc_writer *w);            /* arc_wfinish if not done yet, then frees w; 0 or -1 */
const char *arc_werror(arc_writer *w);    /* text for the last error, "" if none */
unsigned long long arc_wtotal_in(arc_writer *w);   /* bytes given to arc_write */
unsigned long long arc_wtotal_out(arc_writer *w);  /* bytes handed to the sink */

/* ar writer. Common (System V / GNU) format. Names longer than 15 bytes use
 * the BSD "#1/len" form, which GNU binutils and bsdtar read. */
int ar_write_begin(arc_sink sink, void *ctx);                       /* writes "!<arch>\n"; 0 or -1 */
int ar_write_header(arc_sink sink, void *ctx, const ar_member *m);  /* uses name, size64 (or size if size64 is 0), mtime, mode, uid, gid */
/* then the caller writes exactly the member's bytes, then: */
int ar_write_pad(arc_sink sink, void *ctx, const ar_member *m);     /* one '\n' if the size is odd */

/* tar writer, on top of an arc_writer (use ARC_RAW for a plain .tar).
 * The archive is written in the format GNU tar writes by default ("gnu"):
 * ustar headers, GNU 'L' and 'K' records for names longer than 100 bytes,
 * base-256 numbers for values too large for octal. */
int tar_write_header(arc_writer *w, const tar_entry *e);           /* 0 or -1; pads the previous entry first */
long tar_write(arc_writer *w, const void *buf, unsigned long n);   /* the entry's data; never more than e->size in total */
int tar_write_end(arc_writer *w);                                   /* pads the last entry, writes the two zero blocks */

/* SHA-384 uses the SHA-512 state and arc_sha512_update. SHA-1 is here only
 * because OpenPGP version 4 key fingerprints are SHA-1 values; it must not be
 * used to check signatures or data. */
void arc_sha384_init(arc_sha512 *c);
void arc_sha384_final(arc_sha512 *c, unsigned char out[48]);
typedef struct { unsigned long long len; unsigned int h[5]; unsigned char buf[64]; unsigned n; } arc_sha1;
void arc_sha1_init(arc_sha1 *c);
void arc_sha1_update(arc_sha1 *c, const void *p, unsigned long n);
void arc_sha1_final(arc_sha1 *c, unsigned char out[20]);

#endif
