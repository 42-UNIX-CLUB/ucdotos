// SPDX-License-Identifier: GPL-2.0-or-later
/* The compositor: a damage list, a back buffer (scr) and the wallpaper (wall).
 * Each frame repaints only the damaged rectangles of scr, from the wallpaper up
 * through the windows to the cursor, then writes just those rectangles to /dev/fb0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "wm.h"

int scr_w, scr_h;
unsigned *scr, *wall;
int G_gap, G_border, G_radius, G_thin;
static int fb_fd = -1;
static rect dmg[MAX_DAMAGE];
static int ndmg;
static double write_ms;

int fb_setup(int fd) {
  if (fd >= 0) fb_fd = fd;
  unsigned v[40] = {0};
  if (__syscall3(SYS_ioctl, fb_fd, 0x4600, v) < 0 || v[6] != 32 || v[0] < 1 || v[1] < 1) return -1;
  if ((int)v[0] == scr_w && (int)v[1] == scr_h && scr) return 0;
  free(scr); free(wall);
  scr_w = v[0]; scr_h = v[1];
  scr = malloc((size_t)scr_w * scr_h * 4);
  wall = malloc((size_t)scr_w * scr_h * 4);
  if (!scr || !wall) { fprintf(stderr, "wm: out of memory for a %dx%d screen\n", scr_w, scr_h); return -1; }
  if (ptr_x >= scr_w) ptr_x = scr_w - 1;
  if (ptr_y >= scr_h) ptr_y = scr_h - 1;
  return 0;
}

/* ---------- damage ---------- */
static int touch(const rect *a, const rect *b) {
  return a->x <= b->x + b->w && b->x <= a->x + a->w && a->y <= b->y + b->h && b->y <= a->y + a->h;
}
static rect unite(rect a, rect b) {
  int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
  int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return (rect){x0, y0, x1 - x0, y1 - y0};
}
void damage(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w > scr_w - x) w = scr_w - x;
  if (h > scr_h - y) h = scr_h - y;
  if (w <= 0 || h <= 0) return;
  rect r = {x, y, w, h};
  for (int i = 0; i < ndmg;) { /* merge with anything it touches, then look again */
    if (touch(&r, &dmg[i])) { r = unite(r, dmg[i]); dmg[i] = dmg[--ndmg]; i = 0; }
    else i++;
  }
  if (ndmg == MAX_DAMAGE) { for (int i = 1; i < ndmg; i++) dmg[0] = unite(dmg[0], dmg[i]); ndmg = 1; r = unite(r, dmg[0]); ndmg = 0; }
  dmg[ndmg++] = r;
}
void damage_all(void) { ndmg = 0; damage(0, 0, scr_w, scr_h); }
int damage_pending(void) { return ndmg > 0; }
void damage_client_border(client *c) {
  if (!visible(c)) return;
  int k = (G_radius > c->bw ? G_radius : c->bw) + 1;
  rect r = c->r;
  damage(r.x, r.y, r.w, k);
  damage(r.x, r.y + r.h - k, r.w, k);
  damage(r.x, r.y, k, r.h);
  damage(r.x + r.w - k, r.y, k, r.h);
}

/* ---------- windows ---------- */
/* Coverage (0..255) of a rounded rectangle inset by o, with corner radius rr, at pixel (lx, ly)
 * of the top-left corner square. */
static unsigned cov(int lx, int ly, int o, int rr) {
  if (lx < o || ly < o) return 0;
  if (lx >= o + rr || ly >= o + rr) return 255;
  float dx = lx + 0.5f - (o + rr), dy = ly + 0.5f - (o + rr);
  float c = rr - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f;
  return c <= 0 ? 0 : c >= 1 ? 255 : (unsigned)(c * 255.f + 0.5f);
}

typedef struct {
  client *c;
  int t, t1, R, rad;    /* border width, outer ring width, corner square size, radius */
  unsigned c1, c2, bg;  /* outer ring, inner ring, content fill where the client has no pixels */
} wdraw;

static inline unsigned content_px(const wdraw *d, int x, int y) {
  const client *c = d->c;
  return (c->buf && x >= 0 && y >= 0 && x < c->pw && y < c->ph) ? c->buf[(size_t)y * c->pw + x] : d->bg;
}
static void content_span(const wdraw *d, unsigned *dst, int x, int y, int n) {
  const client *c = d->c;
  if (c->buf && y >= 0 && y < c->ph) {
    int a = x < 0 ? -x : 0, b = x + n > c->pw ? c->pw - x : n;
    for (int i = 0; i < a && i < n; i++) dst[i] = d->bg;
    if (b > a) memcpy(dst + a, c->buf + (size_t)y * c->pw + x + a, (size_t)(b - a) * 4);
    for (int i = b > a ? b : a; i < n; i++) dst[i] = d->bg;
  } else for (int i = 0; i < n; i++) dst[i] = d->bg;
}
static inline void corner_px(const wdraw *d, unsigned *p, int cx, int cy, int lx, int ly) {
  unsigned mo = cov(cx, cy, 0, d->rad);
  if (!mo) return;
  unsigned v = gfx_blend_px(*p, d->c1, mo);
  if (d->t > d->t1) { unsigned mm = cov(cx, cy, d->t1, d->rad - d->t1 > 0 ? d->rad - d->t1 : 0); if (mm) v = gfx_blend_px(v, d->c2, mm); }
  unsigned mi = cov(cx, cy, d->t, d->rad - d->t > 0 ? d->rad - d->t : 0);
  if (mi) v = gfx_blend_px(v, content_px(d, lx - d->t, ly - d->t), mi);
  *p = v;
}
static inline unsigned band(const wdraw *d, int k) { return k < d->t1 ? d->c1 : d->c2; }

static void draw_window(client *c, const rect *D) {
  rect r = c->r;
  int x0 = D->x > r.x ? D->x : r.x, x1 = D->x + D->w < r.x + r.w ? D->x + D->w : r.x + r.w;
  int y0 = D->y > r.y ? D->y : r.y, y1 = D->y + D->h < r.y + r.h ? D->y + D->h : r.y + r.h;
  if (x0 >= x1 || y0 >= y1) return;
  wdraw d = {c, c->bw, c->bw, 0, c->fullscreen ? 0 : G_radius, 0, 0, theme.bg};
  if (c->floating) d.c1 = d.c2 = theme.border_float;
  else if (c == focused()) d.c1 = d.c2 = theme.border_focus;
  else { d.c1 = theme.border_normal; d.c2 = theme.bg; d.t1 = G_thin < d.t ? G_thin : d.t; }
  if (c->role == UI_BAR) d.bg = theme.bar_bg;
  d.R = d.rad > d.t ? d.rad : d.t;
  if (d.R > r.w / 2) d.R = r.w / 2;
  if (d.R > r.h / 2) d.R = r.h / 2;
  if (d.rad > d.R) d.rad = d.R;
  int t = d.t, w = r.w, h = r.h, R = d.R;
  for (int y = y0; y < y1; y++) {
    unsigned *row = scr + (size_t)y * scr_w;
    int ly = y - r.y, a = x0 - r.x, b = x1 - r.x; /* window-local columns [a, b) */
    if (ly < R || ly >= h - R) {
      int cy = ly < R ? ly : h - 1 - ly;
      int m0 = a > R ? a : R, m1 = b < w - R ? b : w - R;
      for (int lx = a; lx < b && lx < R; lx++) corner_px(&d, row + r.x + lx, lx, cy, lx, ly);
      if (m0 < m1) {
        if (cy < t) { unsigned col = band(&d, cy); for (int lx = m0; lx < m1; lx++) row[r.x + lx] = col; }
        else content_span(&d, row + r.x + m0, m0 - t, ly - t, m1 - m0);
      }
      for (int lx = a > w - R ? a : w - R; lx < b; lx++) if (lx >= R) corner_px(&d, row + r.x + lx, w - 1 - lx, cy, lx, ly);
    } else {
      int lx = a;
      for (; lx < b && lx < t; lx++) row[r.x + lx] = band(&d, lx);
      int e = b < w - t ? b : w - t;
      if (lx < e) { content_span(&d, row + r.x + lx, lx - t, ly - t, e - lx); lx = e; }
      for (; lx < b; lx++) row[r.x + lx] = band(&d, w - 1 - lx);
    }
  }
}

/* ---------- cursor ---------- */
static const char *arrow[] = {
  "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
  "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
  "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
#define CUR_W 12
#define CUR_H 19
void cursor_moved(int ox, int oy) {
  if (ptr_shown) damage(ox, oy, CUR_W, CUR_H);
  ptr_shown = 1;
  damage(ptr_x, ptr_y, CUR_W, CUR_H);
}
static void draw_cursor(const rect *D) {
  if (!ptr_shown) return;
  for (int j = 0; j < CUR_H; j++) {
    int y = ptr_y + j;
    if (y < D->y || y >= D->y + D->h) continue;
    for (int i = 0; i < CUR_W; i++) {
      int x = ptr_x + i;
      char ch = arrow[j][i];
      if (ch == ' ' || x < D->x || x >= D->x + D->w) continue;
      scr[(size_t)y * scr_w + x] = ch == 'X' ? GFX_HEX(0x101010) : theme.fg;
    }
  }
}

/* ---------- frames ---------- */
static void paint(const rect *D) {
  for (int y = D->y; y < D->y + D->h; y++)
    memcpy(scr + (size_t)y * scr_w + D->x, wall + (size_t)y * scr_w + D->x, (size_t)D->w * 4);
  /* bottom to top: tiles, bars, the fullscreen window, floating windows */
  for (int pass = 0; pass < 4; pass++)
    for (int i = 0; i < nclients; i++) {
      client *c = order[i];
      if (!visible(c)) continue;
      int kind = c->floating ? 3 : c->fullscreen ? 2 : c->role == UI_BAR ? 1 : 0;
      if (kind == pass) draw_window(c, D);
    }
  draw_cursor(D);
}
static void put(const rect *D) {
  if (D->w * 2 >= scr_w) { /* wide: whole rows in one write */
    long n = (long)scr_w * D->h * 4;
    __syscall4(SYS_pwrite64, fb_fd, scr + (size_t)D->y * scr_w, n, (long)D->y * scr_w * 4);
    stats_writes++; stats_bytes += n;
    return;
  }
  for (int y = D->y; y < D->y + D->h; y++) {
    long off = ((long)y * scr_w + D->x) * 4;
    __syscall4(SYS_pwrite64, fb_fd, scr + (size_t)y * scr_w + D->x, D->w * 4, off);
    stats_writes++; stats_bytes += D->w * 4;
  }
}
void compose(void) {
  double t0 = now_ms();
  int n = ndmg;
  for (int i = 0; i < n; i++) paint(&dmg[i]);
  double t1 = now_ms();
  for (int i = 0; i < n; i++) put(&dmg[i]);
  ndmg = 0;
  stats_frames++;
  stats_compose_ms += t1 - t0;
  write_ms += now_ms() - t1;
}
double bench_compose(int n) {
  double c0 = stats_compose_ms, w0 = write_ms, t0 = now_ms();
  for (int i = 0; i < n; i++) { damage_all(); compose(); }
  double total = (now_ms() - t0) / n;
  fprintf(stderr, "wm: bench paint %.3f ms + framebuffer write %.3f ms per frame\n",
          (stats_compose_ms - c0) / n, (write_ms - w0) / n);
  return total;
}

/* ---------- wallpaper: theme colour, a faint dot grid, the club logo large and dim ---------- */
#define LX0 1412.91f
#define LY0 248.35f
static const float chevron[] = {1412.91f, 284.84f, 1441.26f, 313.19f, 1412.91f, 341.54f, 1431.81f, 341.54f, 1460.16f, 313.19f, 1431.81f, 284.84f};
static const float four[] = {1533.14f, 313.19f, 1495.35f, 313.19f, 1533.14f, 275.40f, 1514.24f, 275.40f, 1476.45f, 313.19f,
                             1476.45f, 328.56f, 1514.24f, 328.56f, 1514.24f, 347.46f, 1533.14f, 347.46f};
static const float two_a[] = {1560.18f, 332.17f, 1579.08f, 332.17f, 1579.08f, 313.19f};
static const float two_b[] = {1579.08f, 275.40f, 1560.18f, 275.40f, 1560.18f, 294.30f, 1541.29f, 313.19f,
                              1541.29f, 332.18f, 1560.18f, 332.18f, 1560.18f, 313.19f, 1579.08f, 294.30f};
static const float two_c[] = {1560.18f, 275.40f, 1541.29f, 275.40f, 1541.29f, 294.30f};
static const float cursor_bar[] = {1587.23f, 350.99f, 1606.12f, 350.99f, 1606.12f, 275.40f, 1587.23f, 275.40f};

/* Even-odd polygon fill with 4 sub-rows per pixel and exact horizontal coverage, blended into wall. */
static void logo_poly(const float *pts, int n, float s, int ox, int oy, unsigned color, float alpha) {
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < n; i++) {
    float x = (pts[2 * i] - LX0) * s + ox, y = (pts[2 * i + 1] - LY0) * s + oy;
    if (x < minx) minx = x;
    if (x > maxx) maxx = x;
    if (y < miny) miny = y;
    if (y > maxy) maxy = y;
  }
  int bx0 = (int)minx, bx1 = (int)maxx + 2, by0 = (int)miny, by1 = (int)maxy + 2;
  if (bx0 < 0) bx0 = 0;
  if (by0 < 0) by0 = 0;
  if (bx1 > scr_w) bx1 = scr_w;
  if (by1 > scr_h) by1 = scr_h;
  if (bx0 >= bx1 || by0 >= by1) return;
  float *acc = calloc(bx1 - bx0, sizeof(float));
  if (!acc) return;
  for (int py = by0; py < by1; py++) {
    memset(acc, 0, (bx1 - bx0) * sizeof(float));
    for (int sub = 0; sub < 4; sub++) {
      float sy = py + (sub + 0.5f) / 4, xs[16];
      int nx = 0;
      for (int i = 0; i < n && nx < 16; i++) {
        int j = (i + 1) % n;
        float ax = (pts[2 * i] - LX0) * s + ox, ay = (pts[2 * i + 1] - LY0) * s + oy;
        float bx = (pts[2 * j] - LX0) * s + ox, by = (pts[2 * j + 1] - LY0) * s + oy;
        if ((ay <= sy) == (by <= sy)) continue;
        xs[nx++] = ax + (sy - ay) * (bx - ax) / (by - ay);
      }
      for (int i = 1; i < nx; i++)
        for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) { float t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
      for (int k = 0; k + 1 < nx; k += 2) {
        float xa = xs[k], xb = xs[k + 1];
        for (int ix = (int)xa; ix <= (int)xb && ix < bx1; ix++) {
          if (ix < bx0) continue;
          float l = xa > ix ? xa : ix, r = xb < ix + 1 ? xb : ix + 1;
          if (r > l) acc[ix - bx0] += (r - l) * 0.25f;
        }
      }
    }
    for (int ix = bx0; ix < bx1; ix++) {
      float a = acc[ix - bx0] * alpha;
      if (a > 0.002f) { unsigned *p = wall + (size_t)py * scr_w + ix; *p = gfx_blend_px(*p, color, (unsigned)(a * 255 + 0.5f)); }
    }
  }
  free(acc);
}
static void logo_dot(float cx, float cy, float r, float s, int ox, int oy, unsigned color, float alpha) {
  float x = (cx - LX0) * s + ox, y = (cy - LY0) * s + oy, rr = r * s;
  for (int py = (int)(y - rr) - 1; py <= (int)(y + rr) + 1; py++)
    for (int px = (int)(x - rr) - 1; px <= (int)(x + rr) + 1; px++) {
      if (px < 0 || py < 0 || px >= scr_w || py >= scr_h) continue;
      float dx = px + 0.5f - x, dy = py + 0.5f - y, c = rr - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f;
      if (c <= 0) continue;
      if (c > 1) c = 1;
      unsigned *p = wall + (size_t)py * scr_w + px;
      *p = gfx_blend_px(*p, color, (unsigned)(c * alpha * 255 + 0.5f));
    }
}
void render_wallpaper(void) {
  unsigned bg = theme.desktop, dot = gfx_blend_px(theme.desktop, theme.fg, 16);
  for (int y = 0; y < scr_h; y++) {
    unsigned *row = wall + (size_t)y * scr_w;
    for (int x = 0; x < scr_w; x++) row[x] = bg;
    if (y % 24 == 12) for (int x = 12; x < scr_w; x += 24) row[x] = dot;
  }
  /* the logo, about a quarter of the screen high, in the bottom-right corner */
  float lh = scr_h * 0.24f, s = lh / 102.64f;
  int ox = scr_w - (int)(193.21f * s) - 4 * G_gap - 24, oy = scr_h - (int)lh - 4 * G_gap - 24;
  float al = 0.16f;
  logo_poly(chevron, 6, s, ox, oy, theme.color[10], al);
  logo_poly(four, 9, s, ox, oy, theme.fg, al);
  logo_poly(two_a, 3, s, ox, oy, theme.fg, al);
  logo_poly(two_b, 8, s, ox, oy, theme.fg, al);
  logo_poly(two_c, 3, s, ox, oy, theme.fg, al);
  logo_poly(cursor_bar, 4, s, ox, oy, theme.accent, al * 1.6f);
  logo_dot(1441.26f, 257.80f, 9.45f, s, ox, oy, theme.color[9], al * 1.3f);
  logo_dot(1468.30f, 257.80f, 9.45f, s, ox, oy, theme.color[11], al * 1.3f);
  logo_dot(1495.34f, 257.80f, 9.45f, s, ox, oy, theme.color[10], al * 1.3f);
}
