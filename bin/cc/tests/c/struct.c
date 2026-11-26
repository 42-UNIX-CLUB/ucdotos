// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stddef.h>
struct point { int x, y; };
struct rect { struct point a, b; char name[8]; };
union num { int i; float f; unsigned char b[4]; };
struct one { double d; };
struct big { int v[5]; };
struct flex { int n; int data[]; };
struct bits { unsigned a : 3; signed b : 4; unsigned c : 9; int d : 1; unsigned long long e : 40; };
struct point mkpoint(int x, int y) { struct point p = {x, y}; return p; }
struct one mkone(double d) { struct one o; o.d = d; return o; }
struct big mkbig(int base) { struct big b; for (int i = 0; i < 5; i++) b.v[i] = base + i; return b; }
int area(struct rect r) { return (r.b.x - r.a.x) * (r.b.y - r.a.y); }
double getd(struct one o) { return o.d; }
int sumbig(struct big b) { int s = 0; for (int i = 0; i < 5; i++) s += b.v[i]; b.v[0] = 999; return s; }
struct rect g = {{1, 2}, {11, 22}, "global"};
int main(void) {
  struct point p = mkpoint(3, 4);
  printf("%d %d\n", p.x, p.y);
  struct rect r = {{0, 0}, {5, 6}, "r"};
  printf("%d %s %d\n", area(r), r.name, area(g));
  struct one o = mkone(2.5);
  printf("%g %g\n", o.d, getd(o));
  struct big b = mkbig(10);
  printf("%d %d\n", sumbig(b), b.v[0]);
  union num n;
  n.f = 1.0f;
  printf("%x %d\n", n.i, n.b[3]);
  struct rect *pr = &r;
  pr->a.x = 1;
  printf("%d %d\n", pr->a.x, (*pr).b.y);
  struct rect copy = r;
  copy.b.x = 100;
  printf("%d %d\n", r.b.x, copy.b.x);
  printf("%d %d %d\n", (int)sizeof(struct rect), (int)offsetof(struct rect, name), (int)sizeof(union num));
  struct bits bf = {5, -3, 300, -1, 0x123456789aULL};
  printf("%u %d %u %d %llx %d\n", bf.a, bf.b, bf.c, bf.d, (unsigned long long)bf.e, (int)sizeof bf);
  bf.a = 9; bf.b = 7; bf.c++; bf.e += 1;
  printf("%u %d %u %llx\n", bf.a, bf.b, bf.c, (unsigned long long)bf.e);
  static struct flex *fp;
  static int storage[8];
  fp = (struct flex *)storage;
  fp->n = 3; fp->data[2] = 42;
  printf("%d %d %d\n", fp->n, fp->data[2], (int)sizeof(struct flex));
  struct point arr[3] = {{1, 2}, {3, 4}, [2].y = 9};
  printf("%d %d %d\n", arr[1].x, arr[2].x, arr[2].y);
  struct { int a; struct { int b, c; }; union { int d; float e; }; } anon = {1, {2, 3}, {4}};
  printf("%d %d %d %d\n", anon.a, anon.b, anon.c, anon.d);
  p = (struct point){7, 8};
  printf("%d %d\n", p.x, p.y);
  int k = mkpoint(5, 6).y;
  printf("%d\n", k);
  return 0;
}
