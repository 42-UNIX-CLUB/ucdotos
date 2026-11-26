// SPDX-License-Identifier: GPL-2.0-or-later
/* fetch: system information beside a software-rendered 3D logo (fastfetch style).
 *   fetch                 print once, static logo
 *   fetch -a, --animate   rotate the logo until a key is pressed (~30 fps)
 *   fetch --logo mark|cube|torus|text
 *   fetch --bench [N]     render N frames off-screen and print the cost per frame
 * The logo is the club's prompt mark (three dots, chevron, 42, cursor bar) built from
 * hand-written polygons, extruded into prisms, rasterized with a z-buffer and Lambert
 * shading, and drawn with U+2580 half blocks (two pixels per cell, 24-bit colour). */
#include "term.h"
#include "info.h"
#include "uconf.h"
#include <strings.h>

/* ---------- small math (no libm here) ---------- */
#define PI 3.14159265f
static float fsin(float x) {
  x -= 2 * PI * (float)(int)(x / (2 * PI));
  if (x > PI) x -= 2 * PI; else if (x < -PI) x += 2 * PI;
  if (x > PI / 2) x = PI - x; else if (x < -PI / 2) x = -PI - x;
  float x2 = x * x;
  return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float fcos(float x) { return fsin(x + PI / 2); }
static float fsqrt(float x) { return __builtin_sqrtf(x); }
static float fabsf_(float x) { return x < 0 ? -x : x; }

/* ---------- mesh ---------- */
typedef struct { float x, y, z; } v3;
typedef struct { int a, b, c; v3 n; unsigned col; } tri_t;
#define MAXV 4096
#define MAXT 8192
static v3 vert[MAXV];
static tri_t tri[MAXT];
static int nv, nt;
static float half_w = 1, half_h = 1; /* extent of the shape, to fit the canvas */

static int addv(float x, float y, float z) { vert[nv] = (v3){x, y, z}; return nv++; }
static void addt(int a, int b, int c, v3 n, unsigned col) {
  if (nt < MAXT) tri[nt++] = (tri_t){a, b, c, n, col};
}
static v3 norm3(v3 v) {
  float l = fsqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l > 0 ? (v3){v.x / l, v.y / l, v.z / l} : v;
}

/* extrude a simple polygon (any orientation, may be concave) into a prism from z0 to z1 */
static void extrude(const float *xy, int n, float z0, float z1, unsigned col) {
  float p[64][2];
  float area = 0;
  for (int i = 0; i < n; i++) {
    p[i][0] = xy[2 * i]; p[i][1] = xy[2 * i + 1];
    int j = (i + 1) % n;
    area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
  }
  if (area < 0) /* make it counter-clockwise */
    for (int i = 0; i < n / 2; i++) {
      float t0 = p[i][0], t1 = p[i][1];
      p[i][0] = p[n - 1 - i][0]; p[i][1] = p[n - 1 - i][1];
      p[n - 1 - i][0] = t0; p[n - 1 - i][1] = t1;
    }
  if (nv + 2 * n > MAXV) return;
  int f = nv;
  for (int i = 0; i < n; i++) addv(p[i][0], p[i][1], z1);
  for (int i = 0; i < n; i++) addv(p[i][0], p[i][1], z0);
  int b = f + n;
  /* caps by ear clipping */
  int idx[64], m = n;
  for (int i = 0; i < n; i++) idx[i] = i;
  while (m > 3) {
    int found = 0;
    for (int i = 0; i < m && !found; i++) {
      int ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
      float ax = p[ia][0], ay = p[ia][1], bx = p[ib][0], by = p[ib][1], cx = p[ic][0], cy = p[ic][1];
      float cr = (bx - ax) * (cy - by) - (by - ay) * (cx - bx);
      if (cr <= 1e-7f) continue;
      int inside = 0;
      for (int k = 0; k < m && !inside; k++) {
        int q = idx[k];
        if (q == ia || q == ib || q == ic) continue;
        float px = p[q][0], py = p[q][1];
        float d1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        float d2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
        float d3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
        inside = d1 > 0 && d2 > 0 && d3 > 0;
      }
      if (inside) continue;
      addt(f + ia, f + ib, f + ic, (v3){0, 0, 1}, col);
      addt(b + ic, b + ib, b + ia, (v3){0, 0, -1}, col);
      for (int k = i; k < m - 1; k++) idx[k] = idx[k + 1];
      m--;
      found = 1;
    }
    if (!found) break;
  }
  for (int i = 1; i + 1 < m; i++) { /* the last triangle (or a fan if clipping stalled) */
    addt(f + idx[0], f + idx[i], f + idx[i + 1], (v3){0, 0, 1}, col);
    addt(b + idx[i + 1], b + idx[i], b + idx[0], (v3){0, 0, -1}, col);
  }
  for (int i = 0; i < n; i++) { /* sides: outward normal is to the right of each CCW edge */
    int j = (i + 1) % n;
    v3 nn = norm3((v3){p[j][1] - p[i][1], -(p[j][0] - p[i][0]), 0});
    addt(f + i, f + j, b + j, nn, col);
    addt(f + i, b + j, b + i, nn, col);
  }
}

/* The mark, in the coordinates of unix-club-logo/svg/v1_main_full_color.svg (y down). */
static const float chevron[] = {1412.9f, 284.8f, 1441.3f, 313.2f, 1412.9f, 341.5f, 1431.8f, 341.5f, 1460.2f, 313.2f, 1431.8f, 284.8f};
static const float four[] = {1533.1f, 313.2f, 1495.3f, 313.2f, 1533.1f, 275.4f, 1514.2f, 275.4f, 1476.4f, 313.2f,
                             1476.4f, 328.6f, 1514.2f, 328.6f, 1514.2f, 347.5f, 1533.1f, 347.5f};
static const float two_a[] = {1560.2f, 332.2f, 1579.1f, 332.2f, 1579.1f, 313.2f};
static const float two_b[] = {1579.1f, 275.4f, 1560.2f, 275.4f, 1560.2f, 294.3f, 1541.3f, 313.2f,
                              1541.3f, 332.2f, 1560.2f, 332.2f, 1560.2f, 313.2f, 1579.1f, 294.3f};
static const float two_c[] = {1560.2f, 275.4f, 1541.3f, 275.4f, 1541.3f, 294.3f};
static const float bar[] = {1587.2f, 275.4f, 1606.1f, 275.4f, 1606.1f, 351.0f, 1587.2f, 351.0f};

static void svg_extrude(const float *pts, int n, float depth, unsigned col) {
  float q[64];
  for (int i = 0; i < n; i++) {
    q[2 * i] = (pts[2 * i] - 1509.5f) / 96.6f;
    q[2 * i + 1] = -(pts[2 * i + 1] - 299.7f) / 96.6f;
  }
  extrude(q, n, -depth, depth, col);
}
static void build_mark(void) {
  const float d = 0.11f;
  svg_extrude(chevron, 6, d, th->aqua);
  svg_extrude(four, 9, d, th->fg);
  svg_extrude(two_a, 3, d, th->fg);
  svg_extrude(two_b, 8, d, th->fg);
  svg_extrude(two_c, 3, d, th->fg);
  svg_extrude(bar, 4, d, th->orange);
  const float cx[3] = {1441.3f, 1468.3f, 1495.3f};
  const unsigned cc[3] = {th->red, th->yellow, th->green};
  for (int k = 0; k < 3; k++) { /* the three dots: 16-sided discs */
    float c[32];
    for (int i = 0; i < 16; i++) {
      c[2 * i] = cx[k] + 9.45f * fcos(i * PI / 8);
      c[2 * i + 1] = 257.8f + 9.45f * fsin(i * PI / 8);
    }
    svg_extrude(c, 16, d * 0.8f, cc[k]);
  }
  half_w = 1.03f; half_h = 0.62f;
}
static void build_cube(void) {
  static const int face[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  static const v3 fn[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  unsigned col[6] = {th->red, th->yellow, th->green, th->aqua, th->orange, th->purple};
  const float s = 0.6f;
  for (int i = 0; i < 8; i++) addv(i & 4 ? s : -s, i & 2 ? s : -s, i & 1 ? s : -s);
  for (int f = 0; f < 6; f++) {
    addt(face[f][0], face[f][1], face[f][2], fn[f], col[f]);
    addt(face[f][0], face[f][2], face[f][3], fn[f], col[f]);
  }
  half_w = half_h = 1.05f;
}
static void build_torus(void) {
  const int nu = 32, nvv = 14;
  const float R = 0.68f, r = 0.3f;
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nvv; j++) {
      float u = 2 * PI * i / nu, v = 2 * PI * j / nvv;
      addv((R + r * fcos(v)) * fcos(u), r * fsin(v), (R + r * fcos(v)) * fsin(u));
    }
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nvv; j++) {
      int i2 = (i + 1) % nu, j2 = (j + 1) % nvv;
      float u = 2 * PI * (i + 0.5f) / nu, v = 2 * PI * (j + 0.5f) / nvv;
      v3 n = {fcos(v) * fcos(u), fsin(v), fcos(v) * fsin(u)};
      unsigned col = ((i / 4) & 1) ? th->orange : th->yellow;
      addt(i * nvv + j, i2 * nvv + j, i2 * nvv + j2, n, col);
      addt(i * nvv + j, i2 * nvv + j2, i * nvv + j2, n, col);
    }
  half_w = half_h = 1.0f;
}

/* ---------- rasterizer ---------- */
#define SS 2 /* 2x2 supersampling */
static int FW, FH;          /* canvas in pixels: FW columns, FH = 2 * rows */
static float *zbuf;
static unsigned *cbuf;      /* supersampled colour; bit 24 = covered */
static unsigned *pix;       /* final pixels; bit 24 = covered */

static void canvas_init(int cols, int rows) {
  FW = cols; FH = rows * 2;
  zbuf = malloc(sizeof(float) * FW * FH * SS * SS);
  cbuf = malloc(sizeof(unsigned) * FW * FH * SS * SS);
  pix = malloc(sizeof(unsigned) * FW * FH);
}
static unsigned shade(unsigned c, float k) {
  if (k > 1) k = 1;
  unsigned r = (unsigned)(((c >> 16) & 255) * k), g = (unsigned)(((c >> 8) & 255) * k), b = (unsigned)((c & 255) * k);
  return r << 16 | g << 8 | b;
}
static void render(float yaw, float pitch, float roll) {
  const int W = FW * SS, H = FH * SS;
  const float dist = 4.5f;
  for (int i = 0; i < W * H; i++) { zbuf[i] = 0; cbuf[i] = 0; }
  float cy = fcos(yaw), sy = fsin(yaw), cp = fcos(pitch), sp = fsin(pitch), cr = fcos(roll), sr = fsin(roll);
  /* R = Rx(pitch) * Rz(roll) * Ry(yaw) */
  float m[9];
  float a[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
  float z[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
  float x[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
  float t[9];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i * 3 + j] = z[i * 3] * a[j] + z[i * 3 + 1] * a[3 + j] + z[i * 3 + 2] * a[6 + j];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i * 3 + j] = x[i * 3] * t[j] + x[i * 3 + 1] * t[3 + j] + x[i * 3 + 2] * t[6 + j];
  float fx = (W / 2.0f) * 0.94f / half_w, fy = (H / 2.0f) * 0.94f / half_h;
  float f = (fx < fy ? fx : fy) * (dist - 0.4f);
  static float sx[MAXV], syy[MAXV], iz[MAXV];
  static v3 pv[MAXV];
  for (int i = 0; i < nv; i++) {
    v3 v = vert[i], p = {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
    float w = dist - p.z;
    pv[i] = p; iz[i] = 1 / w;
    sx[i] = W / 2.0f + p.x * f / w;
    syy[i] = H / 2.0f - p.y * f / w;
  }
  const v3 L = norm3((v3){-0.45f, 0.6f, 0.75f});
  for (int k = 0; k < nt; k++) {
    tri_t *T = &tri[k];
    v3 n = T->n, q = {m[0] * n.x + m[1] * n.y + m[2] * n.z, m[3] * n.x + m[4] * n.y + m[5] * n.z, m[6] * n.x + m[7] * n.y + m[8] * n.z};
    v3 pa = pv[T->a];
    if (q.x * -pa.x + q.y * -pa.y + q.z * (dist - pa.z) <= 0) continue; /* back face */
    float d = q.x * L.x + q.y * L.y + q.z * L.z;
    unsigned col = shade(T->col, 0.38f + 0.72f * (d > 0 ? d : 0));
    float x0 = sx[T->a], y0 = syy[T->a], x1 = sx[T->b], y1 = syy[T->b], x2 = sx[T->c], y2 = syy[T->c];
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (fabsf_(area) < 1e-6f) continue;
    float inv = 1 / area;
    int minx = (int)(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int maxx = (int)(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) + 1;
    int miny = (int)(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int maxy = (int)(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > W) maxx = W;
    if (maxy > H) maxy = H;
    float z0 = iz[T->a], z1 = iz[T->b], z2 = iz[T->c];
    for (int py = miny; py < maxy; py++) {
      float yy = py + 0.5f;
      for (int px = minx; px < maxx; px++) {
        float xx = px + 0.5f;
        float w0 = ((x2 - x1) * (yy - y1) - (y2 - y1) * (xx - x1)) * inv;
        float w1 = ((x0 - x2) * (yy - y2) - (y0 - y2) * (xx - x2)) * inv;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float zz = w0 * z0 + w1 * z1 + w2 * z2;
        int i = py * W + px;
        if (zz > zbuf[i]) { zbuf[i] = zz; cbuf[i] = col | 0x1000000; }
      }
    }
  }
  for (int y = 0; y < FH; y++) /* resolve: a pixel is drawn when at least half its samples are */
    for (int x = 0; x < FW; x++) {
      unsigned r = 0, g = 0, b = 0, cnt = 0;
      for (int j = 0; j < SS; j++)
        for (int i = 0; i < SS; i++) {
          unsigned c = cbuf[(y * SS + j) * W + x * SS + i];
          if (!(c & 0x1000000)) continue;
          r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; cnt++;
        }
      pix[y * FW + x] = cnt * 2 >= SS * SS ? 0x1000000 | (r / cnt) << 16 | (g / cnt) << 8 | (b / cnt) : 0;
    }
}

/* ---------- half-block encoder with change tracking ---------- */
static unsigned *prev_top, *prev_bot;
static int cur_fg = -1, cur_bg = -1;   /* -1: terminal default */
static int crow, ccol;                 /* cursor position inside the printed block */
static void set_colors(int fg, int bg) {
  if (fg != cur_fg) { if (fg < 0) ob_puts("\033[39m"); else ob_fg(fg); cur_fg = fg; }
  if (bg != cur_bg) { if (bg < 0) ob_puts("\033[49m"); else ob_bg(bg); cur_bg = bg; }
}
static void emit_cell(unsigned top, unsigned bot) {
  int t = top & 0x1000000, b = bot & 0x1000000;
  top &= 0xffffff; bot &= 0xffffff;
  if (!t && !b) { set_colors(cur_fg, -1); ob_put(" ", 1); }
  else if (t && !b) { set_colors(top, -1); ob_puts("\xe2\x96\x80"); }
  else if (!t && b) { set_colors(bot, -1); ob_puts("\xe2\x96\x84"); }
  else if (top == bot) { set_colors(top, -1); ob_puts("\xe2\x96\x88"); }
  else { set_colors(top, bot); ob_puts("\xe2\x96\x80"); }
}
static void move_to(int r, int c) {
  if (r < crow) ob_printf("\033[%dA", crow - r);
  else if (r > crow) ob_printf("\033[%dB", r - crow);
  crow = r;
  if (c != ccol) {
    if (c > ccol) ob_printf("\033[%dC", c - ccol);
    else if (c == 0) ob_puts("\r");
    else ob_printf("\033[%dD", ccol - c);
    ccol = c;
  }
}
/* emit canvas row r, all cells (full) or only those that changed; returns cells written */
static int emit_row(int r, int full, int block_top) {
  int n = 0;
  for (int c = 0; c < FW; c++) {
    unsigned top = pix[(2 * r) * FW + c], bot = pix[(2 * r + 1) * FW + c];
    int i = r * FW + c;
    if (!full && prev_top[i] == top && prev_bot[i] == bot) continue;
    if (!full) move_to(block_top + r, c);
    emit_cell(top, bot);
    if (!full) ccol++;
    prev_top[i] = top; prev_bot[i] = bot;
    n++;
  }
  return n;
}

/* ---------- system information ---------- */
#define NINFO 16
static char info_key[NINFO][24], info_val[NINFO][160];
static int ninfo;
static char font_name[64] = "Fira Code", logo_name[16] = "mark";
static void add_info(const char *k, const char *fmt, ...) {
  if (ninfo >= NINFO) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(info_val[ninfo], sizeof info_val[0], fmt, ap);
  va_end(ap);
  snprintf(info_key[ninfo], sizeof info_key[0], "%s", k);
  ninfo++;
}
/* ~/.config/fetch/fetch.uconf (or $FETCH_CONFIG), in the .uconf format (libuconf/uconf.h):
 *   [fetch]  logo = mark|cube|torus|text, font = what the Font line says
 *   [info]   line = NAME[, LABEL] repeated: which lines, in which order, under which label;
 *            palette = true|false: the colour blocks under the lines */
static char line_name[NINFO][24], line_label[NINFO][24];
static int nlines, show_palette = 1;
static void load_config(void) {
  const char *home = getenv("HOME"), *env = getenv("FETCH_CONFIG");
  char path[256];
  if (env) snprintf(path, sizeof path, "%s", env);
  else snprintf(path, sizeof path, "%s/.config/fetch/fetch.uconf", home && *home ? home : "/home/user");
  uconf c;
  uconf_load(&c, path); /* no file: the built-in defaults */
  const char *f = uconf_str(&c, "fetch.font", NULL);
  if (f) snprintf(font_name, sizeof font_name, "%s", f);
  else { /* the active theme's [font] name */
    char tpath[96];
    snprintf(tpath, sizeof tpath, "/usr/share/themes/%s.uconf", th->name);
    uconf t;
    if (uconf_load(&t, tpath) == 0) snprintf(font_name, sizeof font_name, "%s", uconf_str(&t, "font.name", font_name));
    uconf_free(&t);
  }
  snprintf(logo_name, sizeof logo_name, "%s", uconf_str(&c, "fetch.logo", logo_name));
  show_palette = uconf_bool(&c, "info.palette", 1);
  for (int i = uconf_next(&c, "info.line", -1); i >= 0 && nlines < NINFO; i = uconf_next(&c, "info.line", i)) {
    char v[96], *f[2];
    snprintf(v, sizeof v, "%s", c.e[i].value);
    int n = uconf_split(v, f, 2);
    snprintf(line_name[nlines], sizeof line_name[0], "%s", f[0]);
    snprintf(line_label[nlines], sizeof line_label[0], "%s", n == 2 && *f[1] ? f[1] : f[0]);
    nlines++;
  }
  uconf_warn_unused(&c);
  uconf_free(&c);
}
static volatile long sink;
__attribute__((noinline)) static void getppid_batch(long n) { for (long i = 0; i < n; i++) sink += getppid(); }
/* Warm getppid cost within about 40 ms, adapted to the clock's resolution (browsers coarsen it).
 * *approx is set when only one long batch fits; returns 0 when the clock is too coarse. */
static double measure_getppid(int *approx) {
  double start = now_ms(), res = clock_res_ms(now_ms, 2);
  double min_batch = 200 * res > 0.2 ? 200 * res : 0.2; /* ms */
  *approx = 0;
  if (min_batch <= 12) {
    long n = 256;
    double t = 0, best = 0;
    for (;;) { /* grow the batch until it lasts min_batch */
      double t0 = now_ms();
      getppid_batch(n);
      t = now_ms() - t0;
      if (t >= min_batch || now_ms() - start > 30) break;
      n = t > 0 && min_batch / t < 8 ? (long)(n * min_batch / t * 1.1) + 1 : n * 4;
    }
    if (t >= min_batch) best = t * 1e6 / n;
    while (now_ms() - start < 38 - t) { /* repeat while time remains; keep the best batch */
      double t0 = now_ms();
      getppid_batch(n);
      double d = now_ms() - t0;
      if (d >= min_batch && (best == 0 || d * 1e6 / n < best)) best = d * 1e6 / n;
    }
    if (best > 0) return best;
  }
  double left = 40 - (now_ms() - start); /* one long batch: good to about res / left */
  if (left < 20 * res || left < 5) return 0;
  long n = 0;
  double t0 = now_ms(), t;
  while ((t = now_ms() - t0) < left) { getppid_batch(1000); n += 1000; }
  *approx = 1;
  return t > 0 && n ? t * 1e6 / n : 0;
}
static int theme_idx = -1, mem_idx = -1;
/* Keep only the lines fetch.uconf lists, in its order, under its labels. Without line
 * settings every line shows. The live Theme and Memory lines follow their new places. */
static void choose_lines(void) {
  if (!nlines) return;
  char key[NINFO][24], val[NINFO][160];
  int n = 0, nt = -1, nm = -1;
  for (int l = 0; l < nlines && n < NINFO; l++) {
    int k = 0;
    while (k < ninfo && strcasecmp(info_key[k], line_name[l])) k++;
    if (k == ninfo) { fprintf(stderr, "fetch: no line called '%s'\n", line_name[l]); continue; }
    snprintf(key[n], sizeof key[0], "%s", line_label[l]);
    memcpy(val[n], info_val[k], sizeof val[0]);
    if (k == theme_idx) nt = n;
    if (k == mem_idx) nm = n;
    n++;
  }
  memcpy(info_key, key, sizeof key);
  memcpy(info_val, val, sizeof val);
  ninfo = n; theme_idx = nt; mem_idx = nm;
}
static void set_theme_info(void) {
  if (theme_idx >= 0) snprintf(info_val[theme_idx], sizeof info_val[0], "%s (%s)", th->name, th->desc);
}
static void fmt_size(char *o, size_t cap, double b) {
  if (b >= 1024.0 * 1048576) snprintf(o, cap, "%.1f GiB", b / (1024.0 * 1048576));
  else snprintf(o, cap, "%.1f MiB", b / 1048576);
}
static void gather(char *userhost, size_t cap) {
  struct sys_uname u;
  struct sys_stats s;
  info_uname(&u);
  info_stats(&s);
  const char *user = getenv("USER");
  snprintf(userhost, cap, "%s@%s", user ? user : "user", u.nodename);
  add_info("OS", "%s %s %s", u.sysname, u.release, u.machine);
  add_info("Kernel", "%s %s", u.release, u.version);
  long up = s.uptime;
  if (up >= 86400) add_info("Uptime", "%ldd %ldh %ldm", up / 86400, up / 3600 % 24, up / 60 % 60);
  else if (up >= 3600) add_info("Uptime", "%ldh %ldm", up / 3600, up / 60 % 60);
  else add_info("Uptime", "%ldm %lds", up / 60, up % 60);
  const char *sh = getenv("SHELL");
  if (sh && strrchr(sh, '/')) sh = strrchr(sh, '/') + 1;
  add_info("Shell", "%s", sh && *sh ? sh : "sh");
  const char *term = getenv("TERM");
  add_info("Terminal", "%s", term ? term : "unknown");
  theme_idx = ninfo;
  add_info("Theme", "");
  set_theme_info();
  const char *font = getenv("FONT");
  add_info("Font", "%s", font ? font : font_name);
  char used[32], total[32];
  fmt_size(used, sizeof used, s.used);
  fmt_size(total, sizeof total, s.total);
  mem_idx = ninfo;
  add_info("Memory", "%s used of %s", used, total);
  add_info("Processes", "%d", s.procs);
  int approx;
  double ns = measure_getppid(&approx);
  if (ns > 0) add_info("Syscall", approx ? "getppid ~%.0f ns (warm)" : ns < 100 ? "getppid %.1f ns (warm)" : "getppid %.0f ns (warm)", ns);
}

/* ---------- layout and output ---------- */
enum { LAY_SIDE, LAY_BELOW, LAY_INFO };
static int layout = LAY_SIDE, val_w = 200, info_w = 200, pal_block = 3;
static int LOGO_COLS = 48, LOGO_ROWS = 16;
static char userhost[200];
static const unsigned *palette(void) {
  static unsigned p[8];
  unsigned q[8] = {th->bg1, th->red, th->green, th->yellow, th->blue, th->purple, th->aqua, th->fg};
  memcpy(p, q, sizeof p);
  return p;
}
static int pal_gap = 1, pal_half = 1; /* blank line before the palette, its half-height second row */
/* s cut to w columns with an ellipsis when it does not fit */
static void ob_fit(const char *s, int w) {
  if (w <= 0) return;
  if (u8_width(s) <= w) { ob_puts(s); return; }
  ob_padded(s, w - 1, 0);
  ob_puts("\xe2\x80\xa6");
}
/* info line i, coloured, never wider than info_w; returns the columns used */
static int emit_info(int i) {
  const int key_w = 11;
  if (i == 0) {
    char *at = strchr(userhost, '@');
    int w = u8_width(userhost);
    if (w > info_w) { ob_puts("\033[1m"); ob_fg(th->yellow); ob_fit(userhost, info_w); ob_puts("\033[0m"); return info_w; }
    ob_puts("\033[1m"); ob_fg(th->yellow); ob_put(userhost, at - userhost);
    ob_fg(th->fg); ob_puts("@"); ob_fg(th->yellow); ob_puts(at + 1); ob_puts("\033[0m");
    return w;
  } else if (i == 1) {
    ob_fg(th->grey);
    int w = u8_width(userhost);
    if (w > info_w) w = info_w;
    for (int k = w; k > 0; k--) ob_puts("\xe2\x94\x80");
    ob_puts("\033[0m");
    return w;
  } else if (i - 2 < ninfo) {
    int k = i - 2;
    ob_puts("\033[1m"); ob_fg(th->orange); ob_padded(info_key[k], key_w, 0);
    ob_puts("\033[22m"); ob_fg(th->fg);
    ob_fit(info_val[k], val_w);
    ob_puts("\033[0m");
    int vw = u8_width(info_val[k]);
    return key_w + (vw < val_w ? vw : val_w);
  } else if ((i == ninfo + 2 + pal_gap || i == ninfo + 3 + pal_gap) && pal_block && show_palette) {
    const unsigned *p = palette();
    const char *b = i == ninfo + 2 + pal_gap ? "\xe2\x96\x88" : "\xe2\x96\x80";
    for (int k = 0; k < 8; k++) { ob_fg(p[k]); for (int j = 0; j < pal_block; j++) ob_puts(b); }
    ob_puts("\033[0m");
    return 8 * pal_block;
  }
  return 0;
}
static int info_lines(void) { return ninfo + 2 + (show_palette ? 1 + pal_gap + pal_half : 0); }
static int max_val_width(void) {
  int w = 0;
  for (int k = 0; k < ninfo; k++) if (u8_width(info_val[k]) > w) w = u8_width(info_val[k]);
  return w;
}
/* pick the layout for a terminal of tcols columns: logo beside the info when it fits, a smaller
 * logo or shortened values when it nearly fits, the info below the logo, or the info alone */
static void choose_layout(int tcols, int textlogo, int text_w) {
  int need = 11 + max_val_width();
  if (u8_width(userhost) > need) need = u8_width(userhost);
  if (need < 24) need = 24;
  int big = textlogo ? text_w : 48;
  if (tcols >= big + 2 + need) { layout = LAY_SIDE; LOGO_COLS = big; info_w = tcols - big - 2; }
  else if (tcols >= big + 2 + 11 + 18) { layout = LAY_SIDE; LOGO_COLS = big; info_w = tcols - big - 2; }
  else if (!textlogo && tcols >= 26 + 2 + 11 + 18) { layout = LAY_SIDE; LOGO_COLS = 26; LOGO_ROWS = 9; info_w = tcols - 28; }
  else if (tcols >= 24) {
    layout = LAY_BELOW;
    LOGO_COLS = textlogo ? text_w : (tcols - 2 < 40 ? tcols - 2 : 40);
    LOGO_ROWS = LOGO_COLS * 10 / 30;
    if (LOGO_ROWS < 6) LOGO_ROWS = 6;
    if (textlogo && text_w > tcols) layout = LAY_INFO;
    info_w = tcols - 1;
  } else { layout = LAY_INFO; info_w = tcols - 1; }
  if (info_w < 12) info_w = 12;
  val_w = info_w - 11;
  pal_block = info_w >= 24 ? 3 : info_w >= 16 ? 2 : info_w >= 8 ? 1 : 0;
}

static void print_plain(void) {
  printf("%s\n", userhost);
  for (int k = u8_width(userhost); k > 0; k--) putchar('-');
  putchar('\n');
  for (int k = 0; k < ninfo; k++) printf("%-11s%s\n", info_key[k], info_val[k]);
}

/* text logo from /usr/share/fetch/logo.txt: $1..$6 switch colour */
static char text_logo[24][256];
static int text_logo_n, text_logo_w;
static int load_text_logo(void) {
  const char *path = getenv("FETCH_LOGO");
  FILE *f = fopen(path ? path : "/usr/share/fetch/logo.txt", "r");
  if (!f) return -1;
  while (text_logo_n < 24 && fgets(text_logo[text_logo_n], 256, f)) {
    char *l = text_logo[text_logo_n];
    l[strcspn(l, "\r\n")] = 0;
    int w = 0;
    for (char *p = l; *p; p++) {
      if (*p == '$' && p[1] >= '1' && p[1] <= '6') { p++; continue; }
      if (!u8_cont((unsigned char)*p)) w++;
    }
    if (w > text_logo_w) text_logo_w = w;
    text_logo_n++;
  }
  fclose(f);
  return text_logo_n ? 0 : -1;
}
static void emit_text_logo_row(int r) {
  const unsigned cols[7] = {th->fg, th->red, th->yellow, th->green, th->aqua, th->fg, th->orange};
  int w = 0;
  ob_fg(cols[0]);
  if (r < text_logo_n)
    for (char *p = text_logo[r]; *p; p++) {
      if (*p == '$' && p[1] >= '1' && p[1] <= '6') { ob_fg(cols[p[1] - '0']); p++; continue; }
      ob_put(p, 1);
      if (!u8_cont((unsigned char)*p)) w++;
    }
  ob_puts("\033[0m");
  for (; w < text_logo_w; w++) ob_put(" ", 1);
}

static void build_shape(const char *name) {
  if (!strcmp(name, "cube")) build_cube();
  else if (!strcmp(name, "torus")) build_torus();
  else build_mark();
}
static int angle_set;
static float angle_deg;
static void pose(const char *name, float t, float *yaw, float *pitch, float *roll) {
  if (!strcmp(name, "cube")) { *yaw = 0.7f + t * 0.9f; *pitch = 0.5f + 0.25f * fsin(t * 0.6f); *roll = 0.2f * fsin(t * 0.4f); }
  else if (!strcmp(name, "torus")) { *yaw = t * 0.8f; *pitch = 0.9f + 0.5f * fsin(t * 0.5f); *roll = 0.3f * fsin(t * 0.3f); }
  else { *yaw = -0.28f + t * 0.75f; *pitch = -0.18f + 0.1f * fsin(t * 0.7f); *roll = 0; }
  if (angle_set) *yaw = angle_deg * PI / 180 + t * 0.75f;
}

static void usage(void) {
  fprintf(stderr, "usage: fetch [-a|--animate] [--logo mark|cube|torus|text] [--angle degrees] [--bench [frames]]\n");
}
static void on_exit_restore(void) { tty_restore(); }

static int textlogo, logo_top, block_lines, tcols_g = 80, info_top_g = 1;
static void line_end(int used) { ob_puts("\033[0m"); if (used < tcols_g) ob_puts("\033[K"); ob_puts("\n"); }
/* write every line of the block (logo rows at full, info) starting where the cursor is */
static void print_block(void) {
  int lines = info_lines();
  int lrows = textlogo ? text_logo_n : LOGO_ROWS;
  if (layout == LAY_SIDE) {
    int info_top = textlogo ? 0 : info_top_g;
    block_lines = lines + info_top > lrows ? lines + info_top : lrows;
    logo_top = 0;
    for (int i = 0; i < block_lines; i++) {
      int lw = textlogo ? text_logo_w : FW;
      if (textlogo) emit_text_logo_row(i);
      else if (i < LOGO_ROWS) { emit_row(i, 1, 0); set_colors(-1, -1); }
      else for (int c = 0; c < FW; c++) ob_put(" ", 1);
      ob_puts("  ");
      int used = lw + 2;
      if (i - info_top >= 0) used += emit_info(i - info_top);
      line_end(used);
    }
  } else {
    int nlogo = layout == LAY_BELOW ? lrows + 1 : 0;
    block_lines = nlogo + lines;
    logo_top = 0;
    for (int i = 0; i < nlogo - 1; i++) {
      if (textlogo) emit_text_logo_row(i);
      else { emit_row(i, 1, 0); set_colors(-1, -1); }
      line_end(textlogo ? text_logo_w : FW);
    }
    if (nlogo) line_end(0);
    for (int i = 0; i < lines; i++) line_end(emit_info(i));
  }
}

int main(int argc, char **argv) {
  int animate = 0, bench = 0;
  const char *logo = 0;
  theme_init();
  load_config();
  if (getenv("FETCH_CLOCK_RES")) clock_sim_ms = atof(getenv("FETCH_CLOCK_RES")) / 1000.0; /* hidden, in us */
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--animate")) animate = 1;
    else if (!strcmp(argv[i], "--logo") && i + 1 < argc) logo = argv[++i];
    else if (!strcmp(argv[i], "--angle") && i + 1 < argc) { angle_set = 1; angle_deg = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--bench")) { bench = 300; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') bench = atoi(argv[++i]); }
    else { usage(); return 2; }
  }
  if (!logo) logo = logo_name;
  if (strcmp(logo, "mark") && strcmp(logo, "cube") && strcmp(logo, "torus") && strcmp(logo, "text")) { usage(); return 2; }

  if (bench) { /* frame cost: render + encode every cell, nothing written */
    build_shape(logo);
    canvas_init(LOGO_COLS, LOGO_ROWS);
    prev_top = calloc(FW * FH / 2, sizeof(unsigned)); prev_bot = calloc(FW * FH / 2, sizeof(unsigned));
    double t0 = now_ms(), tr = 0;
    long bytes = 0;
    for (int k = 0; k < bench; k++) {
      float y, p, r;
      pose(logo, k / 30.0f, &y, &p, &r);
      double a = now_ms();
      render(y, p, r);
      tr += now_ms() - a;
      crow = LOGO_ROWS; ccol = 0;
      for (int row = 0; row < LOGO_ROWS; row++) emit_row(row, 0, 0);
      move_to(LOGO_ROWS, 0);
      bytes += ob_len;
      ob_len = 0;
    }
    double dt = now_ms() - t0;
    printf("fetch --bench: %d frames of %dx%d cells (%dx%d px, 2x2 supersampled), %d triangles\n", bench, LOGO_COLS, LOGO_ROWS, FW, FH, nt);
    printf("  %.3f ms per frame (render %.3f ms, encode %.3f ms), %ld bytes per frame on average\n", dt / bench, tr / bench, (dt - tr) / bench, bytes / bench);
    return 0;
  }

  gather(userhost, sizeof userhost);
  choose_lines();
  if (!tty_check(1)) { print_plain(); return 0; }

  textlogo = !strcmp(logo, "text");
  if (textlogo && load_text_logo()) textlogo = 0, logo = "mark";
  int trows = 24, tcols = 80;
  tty_size(1, &trows, &tcols);
  choose_layout(tcols, textlogo, text_logo_w);
  tcols_g = tcols;
  if (layout == LAY_SIDE && (LOGO_ROWS < 16 || trows < info_lines() + 3)) info_top_g = 0; /* short window: no spacer line */
  /* short window: drop the half palette row, then the blank line, so the block, the command
   * line and the next prompt fit without scrolling the user@host line away */
  int extra = 2 + (layout == LAY_BELOW ? LOGO_ROWS + 1 : 0);
  if (trows < info_lines() + info_top_g + extra) pal_half = 0;
  if (trows < info_lines() + info_top_g + extra) pal_gap = 0;
  if (layout == LAY_INFO) textlogo = 0;
  if (textlogo) animate = 0;
  if (layout == LAY_INFO) animate = 0;
  if (!textlogo && layout != LAY_INFO) {
    build_shape(logo);
    canvas_init(LOGO_COLS, LOGO_ROWS);
    prev_top = calloc(FW * FH / 2, sizeof(unsigned)); prev_bot = calloc(FW * FH / 2, sizeof(unsigned));
    float y, p, r;
    pose(logo, 0, &y, &p, &r);
    render(y, p, r);
  }
  int lrows = textlogo ? text_logo_n : LOGO_ROWS;
  int L = layout == LAY_SIDE ? (info_lines() + info_top_g > lrows ? info_lines() + info_top_g : lrows) : (layout == LAY_BELOW ? lrows + 1 : 0) + info_lines();
  if (animate && trows <= L + 1) animate = 0; /* the block must fit on screen to be redrawn in place */
  if (animate) { ob_puts("\033[?25l"); atexit(on_exit_restore); tty_raw(1); }
  print_block();
  ob_flush();
  if (!animate) return 0;

  crow = block_lines; ccol = 0;
  double start = now_ms(), next = start, theme_check = start;
  long frames = 0;
  double cost = 0;
  for (;;) {
    next += 1000.0 / 30;
    double wait = next - now_ms();
    if (wait < 0) { next = now_ms(); wait = 0; }
    if (key_wait((int)wait)) { key_read(0); break; }
    double a = now_ms();
    float y, p, r;
    pose(logo, (float)((a - start) / 1000.0), &y, &p, &r);
    if (a - theme_check >= 1000) { /* follow the desktop's theme switch */
      theme_check = a;
      if (theme_reload()) {
        nv = nt = 0;
        build_shape(logo);
        set_theme_info();
        render(y, p, r);
        move_to(0, 0);
        print_block();
        crow = block_lines; ccol = 0;
        ob_flush();
        continue;
      }
    }
    render(y, p, r);
    for (int row = 0; row < LOGO_ROWS; row++) emit_row(row, 0, logo_top);
    move_to(block_lines, 0);
    set_colors(-1, -1);
    cost += now_ms() - a;
    frames++;
    ob_flush();
  }
  ob_puts("\033[0m\033[?25h");
  ob_flush();
  tty_restore();
  if (getenv("FETCH_STATS")) fprintf(stderr, "fetch: %ld frames, %.3f ms per frame (render + encode)\n", frames, frames ? cost / frames : 0);
  return 0;
}
