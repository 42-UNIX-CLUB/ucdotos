// SPDX-License-Identifier: GPL-2.0-or-later
/* stat [-L] [-c FORMAT] file...
 * FORMAT: %n name, %N name -> target, %s size, %a octal mode, %A mode string, %F type,
 * %h links, %i inode, %u %g owner ids, %b blocks, %X %Y %Z access, modify, change times
 * (seconds), %y modify time with nanoseconds, %% a percent sign. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
static const char *type(unsigned m) {
  return S_ISREG(m) ? "regular file" : S_ISDIR(m) ? "directory" : S_ISLNK(m) ? "symbolic link" : S_ISCHR(m) ? "character special file" :
         S_ISBLK(m) ? "block special file" : S_ISFIFO(m) ? "fifo" : S_ISSOCK(m) ? "socket" : "unknown";
}
static void modestr(unsigned m, char *o) {
  o[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
  for (int k = 0; k < 9; k++) o[1 + k] = (m >> (8 - k)) & 1 ? "rwxrwxrwx"[k] : '-';
  if (m & S_ISUID) o[3] = (m & 0100) ? 's' : 'S';
  if (m & S_ISGID) o[6] = (m & 010) ? 's' : 'S';
  if (m & S_ISVTX) o[9] = (m & 01) ? 't' : 'T';
  o[10] = 0;
}
static void when(long long t, unsigned ns, char *o) {
  struct tm tm;
  time_t tt = t;
  gmtime_r(&tt, &tm);
  int n = strftime(o, 40, "%Y-%m-%d %H:%M:%S", &tm);
  sprintf(o + n, ".%09u +0000", ns);
}
int main(int argc, char **argv) {
  int i = 1, follow = 0, rc = 0;
  const char *fmt = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-L")) follow = 1;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc) fmt = argv[++i];
    else { fprintf(stderr, "usage: stat [-L] [-c FORMAT] file...\n"); return 1; }
  }
  if (i >= argc) { fprintf(stderr, "usage: stat [-L] [-c FORMAT] file...\n"); return 1; }
  for (; i < argc; i++) {
    struct statx sx;
    if (statx(AT_FDCWD, argv[i], follow ? 0 : AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx)) {
      fprintf(stderr, "stat: cannot stat '%s': %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    char ms[11], link[4096] = "", t1[48], t2[48], t3[48];
    modestr(sx.stx_mode, ms);
    if (S_ISLNK(sx.stx_mode)) { long n = readlink(argv[i], link, sizeof link - 1); link[n > 0 ? n : 0] = 0; }
    if (!fmt) {
      when(sx.stx_atime.tv_sec, sx.stx_atime.tv_nsec, t1); when(sx.stx_mtime.tv_sec, sx.stx_mtime.tv_nsec, t2); when(sx.stx_ctime.tv_sec, sx.stx_ctime.tv_nsec, t3);
      printf("  File: %s%s%s\n  Size: %-15llu Blocks: %-10llu IO Block: %-6u %s\nDevice: %u     Inode: %-11llu Links: %u\n"
             "Access: (%04o/%s)  Uid: %5u   Gid: %5u\nAccess: %s\nModify: %s\nChange: %s\n",
             argv[i], *link ? " -> " : "", link, sx.stx_size, sx.stx_blocks, sx.stx_blksize, type(sx.stx_mode), sx.stx_dev_minor,
             sx.stx_ino, sx.stx_nlink, sx.stx_mode & 07777, ms, sx.stx_uid, sx.stx_gid, t1, t2, t3);
      continue;
    }
    for (const char *f = fmt; *f; f++) {
      if (*f != '%' || !f[1]) { putchar(*f); continue; }
      switch (*++f) {
      case 'n': fputs(argv[i], stdout); break;
      case 'N': printf(*link ? "'%s' -> '%s'" : "'%s'", argv[i], link); break;
      case 's': printf("%llu", sx.stx_size); break;
      case 'a': printf("%o", sx.stx_mode & 07777); break;
      case 'A': fputs(ms, stdout); break;
      case 'F': fputs(type(sx.stx_mode), stdout); break;
      case 'h': printf("%u", sx.stx_nlink); break;
      case 'i': printf("%llu", sx.stx_ino); break;
      case 'u': printf("%u", sx.stx_uid); break;
      case 'g': printf("%u", sx.stx_gid); break;
      case 'b': printf("%llu", sx.stx_blocks); break;
      case 'X': printf("%lld", sx.stx_atime.tv_sec); break;
      case 'Y': printf("%lld", sx.stx_mtime.tv_sec); break;
      case 'Z': printf("%lld", sx.stx_ctime.tv_sec); break;
      case 'y': when(sx.stx_mtime.tv_sec, sx.stx_mtime.tv_nsec, t2); fputs(t2, stdout); break;
      case '%': putchar('%'); break;
      default: putchar('%'); putchar(*f);
      }
    }
    putchar('\n');
  }
  return rc;
}
