// SPDX-License-Identifier: GPL-2.0-or-later
/* The compositor: a damage list, a strip of screen rows to paint into, and the wallpaper.
 * Each frame repaints only the damaged rectangles, a band of rows at a time: the wallpaper,
 * then the windows, then the cursor go into the strip, and the band is written to /dev/fb0.
 * There is no full-screen back buffer and no full-screen copy of the wallpaper: the kernel
 * already keeps the screen, and the wallpaper is a flat colour and a dot grid that are
 * cheap to make again, plus the logo, which is kept as a small picture of its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "wm.h"

int scr_w, scr_h;
int G_gap, G_border, G_radius, G_thin;
static int fb_fd = -1;
static rect dmg[MAX_DAMAGE];
static int ndmg;
static double write_ms;

/* The strip: whole screen rows, strip_y0 .. strip_y0 + strip_rows - 1. Static, so it costs
 * the same at any screen size; at 1280 px wide it holds 51 rows. */
#define STRIP_BYTES (256 * 1024)
static unsigned strip[STRIP_BYTES / 4];
static int strip_y0, strip_rows;
static inline unsigned *row_at(int y) { return strip + (size_t)(y - strip_y0) * scr_w; }

/* The logo picture: logo_w x logo_h at (logo_x, logo_y), wallpaper and dots included. */
static unsigned *logo;
static int logo_x, logo_y, logo_w, logo_h;
static unsigned wall_dot;
/* Two whole wallpaper rows, ready to copy: wall_rows is the plain colour, wall_rows + scr_w
 * the same with the dot grid. */
static unsigned *wall_rows;

int fb_setup(int fd) {
  if (fd >= 0) fb_fd = fd;
  unsigned v[40] = {0};
  if (__syscall3(SYS_ioctl, fb_fd, 0x4600, v) < 0 || v[6] != 32 || v[0] < 1 || v[1] < 1) return -1;
  if ((int)v[0] == scr_w && (int)v[1] == scr_h) return 0;
  if ((size_t)v[0] * 4 > sizeof strip) { fprintf(stderr, "wm: a %u px wide screen is too wide\n", v[0]); return -1; }
  scr_w = v[0]; scr_h = v[1];
  strip_rows = (int)(sizeof strip / 4 / (size_t)scr_w);
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

/* Side of the square at each corner of a window where the frame is curved. Rows outside the
 * corner squares are painted from edge to edge with nothing showing through. */
static int corner_size(const client *c) {
  int rad = c->fullscreen ? 0 : G_radius, R = rad > c->bw ? rad : c->bw;
  if (R > c->r.w / 2) R = c->r.w / 2;
  if (R > c->r.h / 2) R = c->r.h / 2;
  return R;
}
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
  d.R = corner_size(c);
  if (d.rad > d.R) d.rad = d.R;
  int t = d.t, w = r.w, h = r.h, R = d.R;
  for (int y = y0; y < y1; y++) {
    unsigned *row = row_at(y);
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
      row_at(y)[x] = ch == 'X' ? GFX_HEX(0x101010) : theme.fg;
    }
  }
}

/* ---------- frames ---------- */
/* Areas that windows are about to paint completely, so the wallpaper under them can be left
 * out. Only an optimisation: an area that is not listed just gets painted twice. */
#define MAX_SOLID 24
static rect solid[MAX_SOLID];
static int nsolid;
static void add_solid(const client *c, const rect *D) {
  int R = corner_size(c);
  int x0 = c->r.x > D->x ? c->r.x : D->x, x1 = c->r.x + c->r.w < D->x + D->w ? c->r.x + c->r.w : D->x + D->w;
  int y0 = c->r.y + R > D->y ? c->r.y + R : D->y, y1 = c->r.y + c->r.h - R < D->y + D->h ? c->r.y + c->r.h - R : D->y + D->h;
  if (x0 < x1 && y0 < y1 && nsolid < MAX_SOLID) solid[nsolid++] = (rect){x0, y0, x1 - x0, y1 - y0};
}
/* Draw the windows of workspace ws that fall inside D, shifted dx pixels to the right, from
 * bottom to top: tiles, bars, the fullscreen window, floating windows. with_bars = 0 leaves
 * the bars out. only_list = 1 draws nothing and adds what would be covered to solid[]. */
static int sliding; /* compose_anim draws sticky windows itself, fixed in place */
static void draw_windows(int ws, int dx, const rect *D, int with_bars, int only_list) {
  int keep = cur_ws;
  cur_ws = ws; /* visible() and focused() answer for the current workspace */
  for (int pass = 0; pass < 4; pass++)
    for (int i = 0; i < nclients; i++) {
      client *c = order[i];
      if (!visible(c) || (c->role == UI_BAR && !with_bars) || (sliding && c->sticky)) continue;
      int kind = c->floating ? 3 : c->fullscreen ? 2 : c->role == UI_BAR ? 1 : 0;
      if (kind != pass) continue;
      c->r.x += dx;
      if (only_list) add_solid(c, D); else draw_window(c, D);
      c->r.x -= dx;
    }
  cur_ws = keep;
}
/* Wallpaper row y, columns [wx, wx + n), into dst. */
static void wall_make(unsigned *dst, int y, int wx, int n) {
  if (wall_rows) memcpy(dst, wall_rows + (y % 24 == 12 ? scr_w : 0) + wx, (size_t)n * 4);
  else { /* no memory for the rows: make the pixels one by one */
    for (int i = 0; i < n; i++) dst[i] = theme.desktop;
    if (y % 24 == 12) /* the dot grid: columns 12, 36, 60 ... */
      for (int x = wx + ((12 - wx % 24) + 24) % 24; x < wx + n; x += 24) dst[x - wx] = wall_dot;
  }
  if (logo && y >= logo_y && y < logo_y + logo_h) {
    int a = wx > logo_x ? wx : logo_x, b = wx + n < logo_x + logo_w ? wx + n : logo_x + logo_w;
    if (a < b) memcpy(dst + (a - wx), logo + (size_t)(y - logo_y) * logo_w + (a - logo_x), (size_t)(b - a) * 4);
  }
}
/* Row y of the wallpaper, moved left by shift columns (wrapping around), for the screen
 * columns [a, b). */
static void wall_span(int y, int shift, int a, int b) {
  unsigned *row = row_at(y);
  int seam = scr_w - shift; /* screen column where the wallpaper starts over */
  if (a < seam) { int e = b < seam ? b : seam; wall_make(row + a, y, a + shift, e - a); a = e; }
  if (a < b) wall_make(row + a, y, a - seam, b - a);
}
/* The same for a whole row, minus the parts listed in solid[]. */
static void wall_row(int y, int shift) {
  int x = 0;
  while (x < scr_w) {
    int next = scr_w, skip = -1;
    for (int i = 0; i < nsolid; i++) {
      const rect *r = &solid[i];
      if (y < r->y || y >= r->y + r->h || r->x + r->w <= x) continue;
      if (r->x <= x) { skip = r->x + r->w; break; }
      if (r->x < next) next = r->x;
    }
    if (skip >= 0) { x = skip; continue; }
    wall_span(y, shift, x, next);
    x = next;
  }
}
/* Paint D, which lies inside the strip. */
static void paint(const rect *D) {
  for (int y = D->y; y < D->y + D->h; y++) wall_span(y, 0, D->x, D->x + D->w);
  draw_windows(cur_ws, 0, D, 1, 0);
  draw_cursor(D);
}
/* Write D, which lies inside the strip, to the framebuffer. */
static void put(const rect *D) {
  if (D->x == 0 && D->w == scr_w) { /* whole rows: one write */
    long n = (long)scr_w * D->h * 4;
    __syscall4(SYS_pwrite64, fb_fd, row_at(D->y), n, (long)D->y * scr_w * 4);
    stats_writes++; stats_bytes += n;
    return;
  }
  for (int y = D->y; y < D->y + D->h; y++) {
    long off = ((long)y * scr_w + D->x) * 4;
    __syscall4(SYS_pwrite64, fb_fd, row_at(y) + D->x, D->w * 4, off);
    stats_writes++; stats_bytes += D->w * 4;
  }
}
/* ---------- workspace slide ---------- */
/* On a workspace switch the old and the new workspace pan across the screen for
 * cfg.animation_ms milliseconds (0 turns the slide off). Nothing is copied aside: every frame
 * draws the old workspace's windows shifted one way and the new one's shifted the other, each
 * clipped to its side of the seam, over the wallpaper panned by the same amount. So the slide
 * needs no extra memory, whatever the screen size, and windows keep updating while they move.
 * The bar stays put, unless one of the two workspaces hides it (a fullscreen window): then it
 * travels with the workspace that shows it. */
static int anim_on, anim_from, anim_to, anim_dir;
static double anim_t0, anim_len;                 /* start time and duration, in ms */
static int cut_on, cut_from, cut_to;             /* the slide that anim_begin() interrupted */
static double cut_pos;

/* How far the slide is, 0..1, eased out; 1 = over. */
static double anim_pos(void) {
  double p = anim_len > 0 ? (now_ms() - anim_t0) / anim_len : 1;
  if (p >= 1) return 1;
  if (p <= 0) return 0;
  double q = 1 - p;
  return 1 - q * q * q;
}
void anim_end(void) {
  if (!anim_on) return;
  anim_on = 0;
  damage_all();
}
/* Call before the workspace changes. */
void anim_begin(void) {
  cut_on = anim_on; cut_from = anim_from; cut_to = anim_to;
  cut_pos = anim_on ? anim_pos() : 0;
  anim_end();
  anim_from = cur_ws;
}
/* Call after it. dir +1: the viewer moves right along the workspaces, so the picture moves
 * left. -1: the other way. */
void anim_go(int dir) {
  if (cfg.animation_ms <= 0 || !strip_rows || anim_from == cur_ws) return;
  anim_to = cur_ws;
  anim_dir = dir;
  anim_len = cfg.animation_ms;
  anim_t0 = now_ms();
  /* Turning back in the middle of a slide: carry on from where the picture is instead of
   * jumping. The eased position must start at 1 - cut_pos. Find the time p that gives it by
   * bisection (once per turn-back, and it keeps the maths library out of this program). */
  if (cut_on && cut_from == anim_to && cut_to == anim_from && cut_pos > 0 && cut_pos < 1) {
    double want = 1 - cut_pos, lo = 0, hi = 1;
    for (int i = 0; i < 30; i++) {
      double p = (lo + hi) / 2, q = 1 - p;
      if (1 - q * q * q < want) lo = p; else hi = p;
    }
    anim_t0 -= lo * anim_len;
  }
  anim_on = 1;
  damage_all();
}
static int bar_shown_on(int ws) {
  int keep = cur_ws, shown = 0;
  cur_ws = ws;
  for (int i = 0; i < nclients && !shown; i++) shown = order[i]->role == UI_BAR && visible(order[i]);
  cur_ws = keep;
  return shown;
}
/* One frame of the slide. Returns 0 when the slide is over (the caller then paints normally). */
static int compose_anim(void) {
  double pos = anim_pos();
  if (pos >= 1 || cur_ws != anim_to) { anim_end(); return 0; }
  double t0 = now_ms();
  nsolid = 0;
  int off = (int)(pos * scr_w); /* columns of the new workspace on screen so far */
  if (off > scr_w) off = scr_w;
  int rest = scr_w - off;
  /* Moving right: the old workspace keeps the left `rest` columns and has moved left by off;
   * the new one has the right `off` columns. Moving left is the mirror image. */
  rect old_clip, new_clip;
  int old_dx, new_dx, shift;
  if (anim_dir > 0) {
    old_clip = (rect){0, 0, rest, scr_h}; old_dx = -off;
    new_clip = (rect){rest, 0, off, scr_h}; new_dx = rest;
    shift = off;
  } else {
    new_clip = (rect){0, 0, off, scr_h}; new_dx = -rest;
    old_clip = (rect){off, 0, rest, scr_h}; old_dx = off;
    shift = rest;
  }
  int fixed_bar = bar_shown_on(anim_from) && bar_shown_on(anim_to);
  double paint_ms = 0, put_ms = 0;
  sliding = 1;
  for (int y0 = 0; y0 < scr_h; y0 += strip_rows) { /* one band of the strip at a time */
    rect band = {0, y0, scr_w, scr_h - y0 < strip_rows ? scr_h - y0 : strip_rows};
    rect oc = old_clip, nc = new_clip;
    oc.y = nc.y = band.y; oc.h = nc.h = band.h;
    strip_y0 = y0;
    nsolid = 0;
    /* Two rounds over the same windows: first list what they will cover, then, once the
     * wallpaper is down everywhere else, draw them. */
    for (int round = 0; round < 2; round++) {
      if (oc.w > 0) draw_windows(anim_from, old_dx, &oc, !fixed_bar, !round);
      if (nc.w > 0) draw_windows(anim_to, new_dx, &nc, !fixed_bar, !round);
      if (fixed_bar)
        for (int i = 0; i < nclients; i++) {
          if (order[i]->role != UI_BAR || !visible(order[i])) continue;
          if (!round) add_solid(order[i], &band); else draw_window(order[i], &band);
        }
      for (int i = 0; i < nclients; i++) /* sticky windows stay where they are, on top */
        if (order[i]->sticky && visible(order[i])) { if (!round) add_solid(order[i], &band); else draw_window(order[i], &band); }
      /* The wallpaper is the same on both workspaces, so panning it is a rotation of each row. */
      if (!round) for (int y = band.y; y < band.y + band.h; y++) wall_row(y, shift);
    }
    draw_cursor(&band);
    double t1 = now_ms();
    put(&band);
    double t2 = now_ms();
    paint_ms += t1 - t0; put_ms += t2 - t1;
    t0 = t2;
  }
  sliding = 0;
  stats_frames++;
  stats_compose_ms += paint_ms;
  write_ms += put_ms;
  damage_all(); /* keep the frames coming */
  return 1;
}
void compose(void) {
  if (anim_on && compose_anim()) return;
  double paint_ms = 0, put_ms = 0, t0 = now_ms();
  for (int i = 0; i < ndmg; i++) {
    rect D = dmg[i];
    /* a wide rectangle is painted edge to edge, so that each band goes out in one write */
    if (D.w * 2 >= scr_w) { D.x = 0; D.w = scr_w; }
    for (int y0 = D.y; y0 < D.y + D.h; y0 += strip_rows) {
      rect band = {D.x, y0, D.w, D.y + D.h - y0 < strip_rows ? D.y + D.h - y0 : strip_rows};
      strip_y0 = y0;
      paint(&band);
      double t1 = now_ms();
      put(&band);
      double t2 = now_ms();
      paint_ms += t1 - t0; put_ms += t2 - t1;
      t0 = t2;
    }
  }
  ndmg = 0;
  stats_frames++;
  stats_compose_ms += paint_ms;
  write_ms += put_ms;
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

/* Even-odd polygon fill with 4 sub-rows per pixel and exact horizontal coverage, blended into
 * the logo picture (screen coordinates; anything outside the picture is left out). */
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
  if (bx0 < logo_x) bx0 = logo_x;
  if (by0 < logo_y) by0 = logo_y;
  if (bx1 > logo_x + logo_w) bx1 = logo_x + logo_w;
  if (by1 > logo_y + logo_h) by1 = logo_y + logo_h;
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
      if (a > 0.002f) { unsigned *p = logo + (size_t)(py - logo_y) * logo_w + (ix - logo_x); *p = gfx_blend_px(*p, color, (unsigned)(a * 255 + 0.5f)); }
    }
  }
  free(acc);
}
static void logo_dot(float cx, float cy, float r, float s, int ox, int oy, unsigned color, float alpha) {
  float x = (cx - LX0) * s + ox, y = (cy - LY0) * s + oy, rr = r * s;
  for (int py = (int)(y - rr) - 1; py <= (int)(y + rr) + 1; py++)
    for (int px = (int)(x - rr) - 1; px <= (int)(x + rr) + 1; px++) {
      if (px < logo_x || py < logo_y || px >= logo_x + logo_w || py >= logo_y + logo_h) continue;
      float dx = px + 0.5f - x, dy = py + 0.5f - y, c = rr - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f;
      if (c <= 0) continue;
      if (c > 1) c = 1;
      unsigned *p = logo + (size_t)(py - logo_y) * logo_w + (px - logo_x);
      *p = gfx_blend_px(*p, color, (unsigned)(c * alpha * 255 + 0.5f));
    }
}
void render_wallpaper(void) {
  wall_dot = gfx_blend_px(theme.desktop, theme.fg, 16);
  free(wall_rows); /* first: wasm memory never shrinks */
  wall_rows = NULL;
  unsigned *rows = malloc((size_t)scr_w * 2 * 4);
  if (rows) { /* the slow path of wall_make fills them, then every row is a copy */
    wall_make(rows, 0, 0, scr_w);
    wall_make(rows + scr_w, 12, 0, scr_w);
    wall_rows = rows;
  }
  /* the logo, about a quarter of the screen high, in the bottom-right corner */
  float lh = scr_h * 0.24f, s = lh / 102.64f;
  int ox = scr_w - (int)(193.21f * s) - 4 * G_gap - 24, oy = scr_h - (int)lh - 4 * G_gap - 24;
  /* its picture: every pixel the shapes below can touch, kept on the screen */
  int x0 = ox - 2, y0 = oy - 2, x1 = ox + (int)(193.21f * s) + 3, y1 = oy + (int)lh + 3;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > scr_w) x1 = scr_w;
  if (y1 > scr_h) y1 = scr_h;
  free(logo); /* first: wasm memory never shrinks, so the old and new picture must not coexist */
  logo = NULL;
  logo_w = logo_h = 0;
  if (x0 >= x1 || y0 >= y1) return;
  unsigned *pic = malloc((size_t)(x1 - x0) * (y1 - y0) * 4);
  if (!pic) return; /* no memory: a plain wallpaper */
  for (int y = y0; y < y1; y++) wall_make(pic + (size_t)(y - y0) * (x1 - x0), y, x0, x1 - x0); /* colour and dots */
  logo = pic; logo_x = x0; logo_y = y0; logo_w = x1 - x0; logo_h = y1 - y0;
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
