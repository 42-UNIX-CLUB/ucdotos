// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _REGEX_H
#define _REGEX_H
/* POSIX regular expressions, basic and extended, with back-references and
 * the GNU escapes (\w \W \s \S \b \B \< \> \` \' and \| \+ \? in basic syntax).
 * Matching is leftmost-longest. Code: libc/src/regex.c. */
#include <stddef.h>

typedef int regoff_t;
typedef struct {
  size_t re_nsub; /* number of parenthesised subexpressions */
  void *__impl;
} regex_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;

/* regcomp flags */
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8
#define REG_UC_AWK 0x100   /* not POSIX: a backslash inside [ ] escapes the next character (awk) */
#define REG_UC_NOBACKREF 0x200 /* not POSIX: \1..\9 are plain digits (awk) */
/* regexec flags */
#define REG_NOTBOL 1
#define REG_NOTEOL 2
#define REG_STARTEND 4    /* as in glibc and the BSDs: pmatch[0] gives the subject [rm_so, rm_eo) */
#define REG_UC_OFFSET 0x100 /* not POSIX: the subject is [0, rm_eo) but the search starts at rm_so,
                               so ^, \b and \< still see the text before rm_so */

/* errors */
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

#define RE_DUP_MAX 255

int regcomp(regex_t *, const char *, int);
int regexec(const regex_t *, const char *, size_t, regmatch_t *, int);
size_t regerror(int, const regex_t *, char *, size_t);
void regfree(regex_t *);
#endif
