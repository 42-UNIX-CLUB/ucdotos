// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <stddef_extra.h>
#define EOF (-1)
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
typedef struct __file FILE;
extern FILE *stdin, *stdout, *stderr;
int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *, const char *, ...) __attribute__((format(printf, 2, 3)));
int dprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *, const char *, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
int sscanf(const char *, const char *, ...) __attribute__((format(scanf, 2, 3)));
int fscanf(FILE *, const char *, ...) __attribute__((format(scanf, 2, 3)));
int scanf(const char *, ...) __attribute__((format(scanf, 1, 2)));
int vsscanf(const char *, const char *, va_list);
int vfscanf(FILE *, const char *, va_list);
int vscanf(const char *, va_list);
int asprintf(char **, const char *, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char **, const char *, va_list);
ssize_t getline(char **, size_t *, FILE *);
ssize_t getdelim(char **, size_t *, int, FILE *);
FILE *popen(const char *, const char *); /* runs /bin/sh -c cmd */
int pclose(FILE *);
int puts(const char *);
int putchar(int);
int putc(int, FILE *);
int fputc(int, FILE *);
int fputs(const char *, FILE *);
int getchar(void);
int getc(FILE *);
int fgetc(FILE *);
int ungetc(int, FILE *);
char *fgets(char *, int, FILE *);
FILE *fopen(const char *, const char *);
FILE *fdopen(int, const char *);
int fclose(FILE *);
int fflush(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
void rewind(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fileno(FILE *);
void perror(const char *);
int remove(const char *);
int rename(const char *, const char *);
void setbuf(FILE *, char *);
int setvbuf(FILE *, char *, int, size_t);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FILENAME_MAX 4096
#define FOPEN_MAX 32
#define L_tmpnam 20
#endif
