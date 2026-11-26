// SPDX-License-Identifier: GPL-2.0-or-later
/* String routines shared by the kernel and the user libc. */
typedef unsigned long usize;
void *memcpy(void *, const void *, usize);

usize strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
usize strnlen(const char *s, usize n) { usize i = 0; while (i < n && s[i]) i++; return i; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, usize n) {
  while (n && *a && *a == *b) a++, b++, n--;
  return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, usize n) {
  usize i = 0;
  for (; i < n && s[i]; i++) d[i] = s[i];
  for (; i < n; i++) d[i] = 0;
  return d;
}
usize strlcpy(char *d, const char *s, usize n) {
  usize l = strlen(s);
  if (n) { usize c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
  return l;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) {
  usize l = strlen(n);
  if (!l) return (char *)h;
  for (; *h; h++) if (*h == *n && !strncmp(h, n, l)) return (char *)h;
  return 0;
}
int memcmp(const void *a, const void *b, usize n) {
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
  return 0;
}
void *memchr(const void *s, int c, usize n) {
  const unsigned char *p = s;
  for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
  return 0;
}
