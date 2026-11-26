// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

struct __file {
  int fd, flags;       /* flags: 1 = eof, 2 = error, 4 = line buffered, 8 = unbuffered, 16 = owns buf */
  int unget;           /* -1 = none */
  char *buf;
  size_t cap, rpos, rlen, wlen;
  struct __file *next;
  int pid;             /* popen child, or 0 */
};
static char out_buf[BUFSIZ], in_buf[BUFSIZ];
static FILE f_in = {0, 0, -1, in_buf, BUFSIZ, 0, 0, 0, 0, 0};
static FILE f_out = {1, 4, -1, out_buf, BUFSIZ, 0, 0, 0, &f_in, 0};
static FILE f_err = {2, 8, -1, 0, 0, 0, 0, 0, &f_out, 0};
static FILE *all_files = &f_err;
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;

int fflush(FILE *f) {
  if (!f) { for (FILE *p = all_files; p; p = p->next) fflush(p); return 0; }
  if (f->wlen) { if (write(f->fd, f->buf, f->wlen) < 0) f->flags |= 2; f->wlen = 0; }
  return 0;
}
void __stdio_flush_all(void) { fflush(0); }
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
  size_t len = size * n;
  const char *p = ptr;
  if (!len) return 0;
  if (f->rlen > f->rpos) { lseek(f->fd, (long)f->rpos - (long)f->rlen, SEEK_CUR); }
  f->rpos = f->rlen = 0;
  if ((f->flags & 8) || !f->buf) { if (write(f->fd, p, len) < 0) { f->flags |= 2; return 0; } return n; }
  size_t i = 0;
  while (i < len) {
    size_t room = f->cap - f->wlen, c = len - i < room ? len - i : room;
    memcpy(f->buf + f->wlen, p + i, c);
    f->wlen += c; i += c;
    if (f->wlen == f->cap) fflush(f);
  }
  if ((f->flags & 4) && memchr(p, '\n', len)) fflush(f);
  return n;
}
int fputc(int c, FILE *f) { char ch = c; return fwrite(&ch, 1, 1, f) == 1 ? (unsigned char)ch : EOF; }
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { fwrite(s, 1, strlen(s), f); return 0; }
int puts(const char *s) { fputs(s, stdout); fputc('\n', stdout); return 0; }

static int fill(FILE *f) {
  if (f == stdin) fflush(stdout);
  if (f->wlen) fflush(f);
  long r = read(f->fd, f->buf, f->cap);
  if (r <= 0) { f->flags |= r == 0 ? 1 : 2; f->rpos = f->rlen = 0; return EOF; }
  f->rpos = 0; f->rlen = r;
  return 0;
}
int fgetc(FILE *f) {
  if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
  if (!f->buf) { unsigned char c; return read(f->fd, &c, 1) == 1 ? c : (f->flags |= 1, EOF); }
  if (f->rpos >= f->rlen && fill(f)) return EOF;
  return (unsigned char)f->buf[f->rpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE *f) { if (c == EOF) return EOF; f->unget = (unsigned char)c; f->flags &= ~1; return f->unget; }
char *fgets(char *s, int n, FILE *f) {
  int i = 0;
  while (i < n - 1) { int c = fgetc(f); if (c == EOF) break; s[i++] = c; if (c == '\n') break; }
  if (!i) return 0;
  s[i] = 0;
  return s;
}
size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
  size_t len = size * n, done = 0;
  char *p = ptr;
  if (!len) return 0;
  if (f->unget >= 0) { p[done++] = f->unget; f->unget = -1; }
  while (done < len) {
    if (f->rpos < f->rlen) {
      size_t c = f->rlen - f->rpos < len - done ? f->rlen - f->rpos : len - done;
      memcpy(p + done, f->buf + f->rpos, c);
      f->rpos += c; done += c;
    } else if (len - done >= f->cap || !f->buf) { /* large read: skip the buffer */
      if (f->wlen) fflush(f);
      long r = read(f->fd, p + done, len - done);
      if (r <= 0) { f->flags |= r == 0 ? 1 : 2; break; }
      done += r;
    } else if (fill(f)) break;
  }
  return done / size;
}
FILE *fdopen(int fd, const char *mode) {
  (void)mode;
  FILE *f = calloc(1, sizeof *f);
  if (!f) return 0;
  f->fd = fd; f->unget = -1; f->buf = malloc(BUFSIZ); f->cap = BUFSIZ; f->flags = 16;
  f->next = all_files; all_files = f;
  return f;
}
FILE *fopen(const char *path, const char *mode) {
  int flags = mode[0] == 'r' ? O_RDONLY : mode[0] == 'w' ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND);
  if (strchr(mode, '+')) flags = (flags & ~O_ACCMODE) | O_RDWR;
  int fd = open(path, flags, 0644);
  return fd < 0 ? 0 : fdopen(fd, mode);
}
int fclose(FILE *f) {
  fflush(f);
  int r = close(f->fd);
  for (FILE **pp = &all_files; *pp; pp = &(*pp)->next) if (*pp == f) { *pp = f->next; break; }
  if (f->flags & 16) { free(f->buf); free(f); }
  return r;
}
int fseek(FILE *f, long off, int whence) {
  fflush(f);
  if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->unget >= 0);
  f->rpos = f->rlen = 0; f->unget = -1; f->flags &= ~1;
  return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}
long ftell(FILE *f) {
  long p = lseek(f->fd, 0, SEEK_CUR);
  return p < 0 ? -1 : p - (long)(f->rlen - f->rpos) + (long)f->wlen - (f->unget >= 0);
}
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }
int feof(FILE *f) { return f->flags & 1; }
int ferror(FILE *f) { return (f->flags & 2) != 0; }
void clearerr(FILE *f) { f->flags &= ~3; }
int fileno(FILE *f) { return f->fd; }
void setbuf(FILE *f, char *b) { if (!b) f->flags |= 8; }
int setvbuf(FILE *f, char *b, int mode, size_t n) {
  (void)b; (void)n;
  f->flags &= ~12;
  if (mode == _IONBF) f->flags |= 8; else if (mode == _IOLBF) f->flags |= 4;
  return 0;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
  char small[512];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(small, sizeof small, fmt, ap);
  if (n < (int)sizeof small) fwrite(small, 1, n, f);
  else {
    char *big = malloc(n + 1);
    vsnprintf(big, n + 1, fmt, ap2);
    fwrite(big, 1, n, f);
    free(big);
  }
  va_end(ap2);
  return n;
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(stdout, fmt, ap); va_end(ap); return n; }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }
int dprintf(int fd, const char *fmt, ...) {
  char buf[1024];
  va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (n >= (int)sizeof buf) n = sizeof buf - 1;
  return write(fd, buf, n);
}
int vsprintf(char *s, const char *fmt, va_list ap) { return vsnprintf(s, 1 << 30, fmt, ap); }
int sprintf(char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnprintf(s, 1 << 30, fmt, ap); va_end(ap); return n; }
void perror(const char *s) { fprintf(stderr, "%s: %s\n", s, strerror(errno)); }
int rename(const char *a, const char *b) { return __sysret(__syscall4(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b)); }
int vasprintf(char **out, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(0, 0, fmt, ap);
  *out = malloc(n + 1);
  if (*out) vsnprintf(*out, n + 1, fmt, ap2);
  va_end(ap2);
  return *out ? n : -1;
}
int asprintf(char **out, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vasprintf(out, fmt, ap); va_end(ap); return n; }
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f) {
  size_t n = 0;
  int c;
  if (!*line || !*cap) { *cap = 128; *line = malloc(*cap); if (!*line) return -1; }
  while ((c = fgetc(f)) != EOF) {
    if (n + 2 > *cap) {
      char *p = realloc(*line, *cap * 2);
      if (!p) return -1;
      *line = p; *cap *= 2;
    }
    (*line)[n++] = c;
    if (c == delim) break;
  }
  (*line)[n] = 0;
  return n ? (ssize_t)n : -1;
}
ssize_t getline(char **line, size_t *cap, FILE *f) { return getdelim(line, cap, '\n', f); }
FILE *popen(const char *cmd, const char *mode) {
  int p[2], rd = mode[0] == 'r';
  if (pipe2(p, O_CLOEXEC) < 0) return 0;
  int map[3] = {rd ? 0 : p[0], rd ? p[1] : 1, 2};
  char *argv[] = {"sh", "-c", (char *)cmd, 0};
  int pid = spawn("/bin/sh", argv, 0, map);
  close(rd ? p[1] : p[0]);
  if (pid < 0) { close(rd ? p[0] : p[1]); return 0; }
  FILE *f = fdopen(rd ? p[0] : p[1], mode);
  if (f) f->pid = pid;
  return f;
}
int pclose(FILE *f) {
  int pid = f->pid, st = 0;
  fclose(f);
  return waitpid(pid, &st, 0) < 0 ? -1 : st;
}
void __assert_fail(const char *x, const char *file, int line) { fprintf(stderr, "assertion failed: %s (%s:%d)\n", x, file, line); abort(); }
