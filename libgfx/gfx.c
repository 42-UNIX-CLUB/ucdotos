// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gfx.c - 2D drawing on 32-bit RGBA surfaces. See gfx.h.
 */
#include "gfx.h"
#include <stddef.h>

void *memmove(void *dst, const void *src, size_t n);

typedef unsigned int u32;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* Shapes are clamped to +-GFX_LIM so that the integer arithmetic in the
 * drawing code can never overflow. Nothing visible on a real surface
 * changes. */
#define GFX_LIM (1 << 28)

static int clampi(long long v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : (int)v);
}

/* Clamp the edges of rect (x,y,w,h) into [-GFX_LIM, GFX_LIM]. */
static void norm_rect(int *x, int *y, int *w, int *h)
{
    long long x0 = *x, y0 = *y, x1 = x0 + *w, y1 = y0 + *h;
    int a = clampi(x0, -GFX_LIM, GFX_LIM), b = clampi(y0, -GFX_LIM, GFX_LIM);
    int c = clampi(x1, -GFX_LIM, GFX_LIM), d = clampi(y1, -GFX_LIM, GFX_LIM);
    *x = a;
    *y = b;
    *w = c - a;
    *h = d - b;
}

/* ------------------------------------------------------------------ */
/* Surface and clip                                                    */

void gfx_init(gfx_surface *s, unsigned int *px, int w, int h, int stride)
{
    if (!px || w < 0 || h < 0) w = h = 0;
    if (stride < w) stride = w;
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    gfx_reset_clip(s);
}

void gfx_reset_clip(gfx_surface *s)
{
    s->clip_x0 = 0;
    s->clip_y0 = 0;
    s->clip_x1 = s->w;
    s->clip_y1 = s->h;
}

/* Intersect [x, x+w) with [lo, hi), using 64-bit math so huge values
 * cannot overflow. Returns 0 if empty. */
static int clip_span(long long x, long long w, int lo, int hi, int *a, int *b)
{
    long long x1 = x + w;
    if (w <= 0) return 0;
    if (x < lo) x = lo;
    if (x1 > hi) x1 = hi;
    if (x >= x1) return 0;
    *a = (int)x;
    *b = (int)x1;
    return 1;
}

void gfx_set_clip(gfx_surface *s, int x, int y, int w, int h)
{
    if (!clip_span(x, w, 0, s->w, &s->clip_x0, &s->clip_x1) ||
        !clip_span(y, h, 0, s->h, &s->clip_y0, &s->clip_y1)) {
        s->clip_x0 = s->clip_y0 = s->clip_x1 = s->clip_y1 = 0;
    }
}

void gfx_sub(gfx_surface *sub, const gfx_surface *s, int x, int y, int w, int h)
{
    int x0, x1, y0, y1;
    if (!clip_span(x, w, 0, s->w, &x0, &x1) || !clip_span(y, h, 0, s->h, &y0, &y1)) {
        gfx_init(sub, s->px, 0, 0, s->stride);
        return;
    }
    sub->px = s->px + (long)y0 * s->stride + x0;
    sub->w = x1 - x0;
    sub->h = y1 - y0;
    sub->stride = s->stride;
    sub->clip_x0 = imax(s->clip_x0 - x0, 0);
    sub->clip_y0 = imax(s->clip_y0 - y0, 0);
    sub->clip_x1 = imin(s->clip_x1 - x0, sub->w);
    sub->clip_y1 = imin(s->clip_y1 - y0, sub->h);
    if (sub->clip_x1 < sub->clip_x0) sub->clip_x1 = sub->clip_x0;
    if (sub->clip_y1 < sub->clip_y0) sub->clip_y1 = sub->clip_y0;
}

/* Clip rect (x,y,w,h) to the clip rectangle. */
static int clip_rect(const gfx_surface *s, int x, int y, int w, int h,
                     int *x0, int *y0, int *x1, int *y1)
{
    if (!s || !s->px) return 0;
    return clip_span(x, w, s->clip_x0, s->clip_x1, x0, x1) &&
           clip_span(y, h, s->clip_y0, s->clip_y1, y0, y1);
}

static int in_clip(const gfx_surface *s, int x, int y)
{
    return x >= s->clip_x0 && x < s->clip_x1 && y >= s->clip_y0 && y < s->clip_y1;
}

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */

/* Blend colour c with alpha a (0..255) over *p. */
static inline void put_a(u32 *p, u32 c, u32 a)
{
    if (a == 0) return;
    if (a >= 255) *p = c | 0xFF000000u;
    else *p = gfx_blend_px(*p, c, a);
}

/* Combine 0..255 coverage with the colour's alpha. */
static inline u32 mul255(u32 a, u32 b)
{
    u32 t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

static inline void plot_a(gfx_surface *s, int x, int y, u32 c, u32 a)
{
    if (in_clip(s, x, y)) put_a(s->px + (long)y * s->stride + x, c, a);
}

static void hspan_fill(u32 *p, int n, u32 c)
{
    int i;
    for (i = 0; i < n; i++) p[i] = c;
}

static void hspan_blend(u32 *p, int n, u32 c, u32 a)
{
    int i;
    if (a == 0) return;
    if (a >= 255) {
        hspan_fill(p, n, c | 0xFF000000u);
        return;
    }
    {
        /* hoist the colour terms out of the loop */
        u32 ia = 255u - a, s = c | 0xFF000000u;
        u32 crb = (s & 0x00FF00FFu) * a + 0x00800080u;
        u32 cga = ((s >> 8) & 0x00FF00FFu) * a + 0x00800080u;
        for (i = 0; i < n; i++) {
            u32 d = p[i];
            u32 rb = crb + (d & 0x00FF00FFu) * ia;
            u32 ga = cga + ((d >> 8) & 0x00FF00FFu) * ia;
            rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
            ga = (ga + ((ga >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
            p[i] = rb | ga;
        }
    }
}

/* Span [xa, xb) of row y, clipped, with colour c at alpha a. */
static void span_a(gfx_surface *s, int y, int xa, int xb, u32 c, u32 a)
{
    if (y < s->clip_y0 || y >= s->clip_y1) return;
    if (xa < s->clip_x0) xa = s->clip_x0;
    if (xb > s->clip_x1) xb = s->clip_x1;
    if (xa >= xb) return;
    hspan_blend(s->px + (long)y * s->stride + xa, xb - xa, c, a);
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */

void gfx_fill_rect(gfx_surface *s, int x, int y, int w, int h, unsigned int color)
{
    int x0, y0, x1, y1, j;
    if (!clip_rect(s, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) hspan_fill(s->px + (long)j * s->stride + x0, x1 - x0, color);
}

void gfx_clear(gfx_surface *s, unsigned int color)
{
    gfx_fill_rect(s, s->clip_x0, s->clip_y0, s->clip_x1 - s->clip_x0,
                  s->clip_y1 - s->clip_y0, color);
}

void gfx_blend_rect(gfx_surface *s, int x, int y, int w, int h, unsigned int color)
{
    int x0, y0, x1, y1, j;
    u32 a = GFX_A(color);
    if (a == 0 || !clip_rect(s, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++)
        hspan_blend(s->px + (long)j * s->stride + x0, x1 - x0, color, a);
}

/* Signed distance from (px,py) to a rounded box centred at (cx,cy) with
 * half extents (hx,hy) and corner radius r. Negative inside. */
static float rr_sdf(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = __builtin_fabsf(px - cx) - (hx - r);
    float qy = __builtin_fabsf(py - cy) - (hy - r);
    float mx = qx > 0.0f ? qx : 0.0f, my = qy > 0.0f ? qy : 0.0f;
    float in = qx > qy ? qx : qy;
    if (in > 0.0f) in = 0.0f;
    return __builtin_sqrtf(mx * mx + my * my) + in - r;
}

static float cov_of(float sd)
{
    float c = 0.5f - sd;
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

void gfx_round_rect_fill(gfx_surface *s, int x, int y, int w, int h, int radius, unsigned int color)
{
    int x0, y0, x1, y1, j, i, r;
    float cx, cy, hx, hy;
    u32 ca = GFX_A(color);
    norm_rect(&x, &y, &w, &h);
    if (ca == 0 || !clip_rect(s, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        gfx_blend_rect(s, x, y, w, h, color);
        return;
    }
    cx = (float)x + 0.5f * (float)w;
    cy = (float)y + 0.5f * (float)h;
    hx = 0.5f * (float)w;
    hy = 0.5f * (float)h;
    for (j = y0; j < y1; j++) {
        if (j >= y + r && j < y + h - r) {
            span_a(s, j, x0, x1, color, ca);
            continue;
        }
        /* corner row: AA on the two corner column ranges, solid between */
        for (i = imax(x, x0); i < imin(x + r, x1); i++)
            put_a(s->px + (long)j * s->stride + i, color,
                  mul255(ca, (u32)(cov_of(rr_sdf(i + 0.5f, j + 0.5f, cx, cy, hx, hy, (float)r)) * 255.0f + 0.5f)));
        span_a(s, j, x + r, x + w - r, color, ca);
        for (i = imax(x + w - r, x0); i < imin(x + w, x1); i++)
            put_a(s->px + (long)j * s->stride + i, color,
                  mul255(ca, (u32)(cov_of(rr_sdf(i + 0.5f, j + 0.5f, cx, cy, hx, hy, (float)r)) * 255.0f + 0.5f)));
    }
}

void gfx_round_rect_stroke(gfx_surface *s, int x, int y, int w, int h, int radius,
                           int t, unsigned int color)
{
    int x0, y0, x1, y1, j, i, r, k, ri;
    float cx, cy, hx, hy;
    u32 ca = GFX_A(color);
    norm_rect(&x, &y, &w, &h);
    if (t > GFX_LIM) t = GFX_LIM;
    if (ca == 0 || t <= 0 || !clip_rect(s, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    if (2 * t >= w || 2 * t >= h) {
        gfx_round_rect_fill(s, x, y, w, h, r, color);
        return;
    }
    ri = r - t > 0 ? r - t : 0;
    k = r > t ? r : t;              /* size of the corner band */
    cx = (float)x + 0.5f * (float)w;
    cy = (float)y + 0.5f * (float)h;
    hx = 0.5f * (float)w;
    hy = 0.5f * (float)h;
    for (j = y0; j < y1; j++) {
        int top = j < y + t || j >= y + h - t;   /* fully inside a horizontal bar */
        if (j >= y + k && j < y + h - k) {
            span_a(s, j, x, x + t, color, ca);
            span_a(s, j, x + w - t, x + w, color, ca);
            continue;
        }
        if (top) span_a(s, j, x + k, x + w - k, color, ca);
        for (i = imax(x, x0); i < x1; i++) {
            float po, pi, c;
            if (i >= x + k && i < x + w - k) {   /* middle part done above */
                i = x + w - k - 1;
                continue;
            }
            po = cov_of(rr_sdf(i + 0.5f, j + 0.5f, cx, cy, hx, hy, (float)r));
            pi = cov_of(rr_sdf(i + 0.5f, j + 0.5f, cx, cy, hx - t, hy - t, (float)ri));
            c = po - pi;
            if (c <= 0.0f) continue;
            put_a(s->px + (long)j * s->stride + i, color, mul255(ca, (u32)(c * 255.0f + 0.5f)));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lines and circles                                                   */

static int outcode(const gfx_surface *s, long long x, long long y)
{
    int c = 0;
    if (x < s->clip_x0) c |= 1;
    else if (x >= s->clip_x1) c |= 2;
    if (y < s->clip_y0) c |= 4;
    else if (y >= s->clip_y1) c |= 8;
    return c;
}

void gfx_line(gfx_surface *s, int ix0, int iy0, int ix1, int iy1, unsigned int color)
{
    long long x0 = ix0, y0 = iy0, x1 = ix1, y1 = iy1;
    long long dx, dy, err, sx, sy, n;
    u32 ca = GFX_A(color);
    int c0, c1, guard;
    if (!s || !s->px || ca == 0 || s->clip_x0 >= s->clip_x1 || s->clip_y0 >= s->clip_y1) return;

    /* Cohen-Sutherland against the clip rect (inclusive bounds) */
    c0 = outcode(s, x0, y0);
    c1 = outcode(s, x1, y1);
    for (guard = 0; guard < 8 && (c0 | c1); guard++) {
        long long x = 0, y = 0, xmin = s->clip_x0, xmax = s->clip_x1 - 1;
        long long ymin = s->clip_y0, ymax = s->clip_y1 - 1;
        int c = c0 ? c0 : c1;
        if (c0 & c1) return;
        /* double: the products of two 33-bit differences overflow 64 bits */
        if (c & 8) { y = ymax; x = x0 + (long long)((double)(x1 - x0) * (double)(ymax - y0) / (double)(y1 - y0)); }
        else if (c & 4) { y = ymin; x = x0 + (long long)((double)(x1 - x0) * (double)(ymin - y0) / (double)(y1 - y0)); }
        else if (c & 2) { x = xmax; y = y0 + (long long)((double)(y1 - y0) * (double)(xmax - x0) / (double)(x1 - x0)); }
        else { x = xmin; y = y0 + (long long)((double)(y1 - y0) * (double)(xmin - x0) / (double)(x1 - x0)); }
        if (c == c0) { x0 = x; y0 = y; c0 = outcode(s, x0, y0); }
        else { x1 = x; y1 = y; c1 = outcode(s, x1, y1); }
    }
    if (c0 | c1) return;

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;   /* -|dy| */
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;
    for (n = 0; n < 1000000; n++) {
        plot_a(s, (int)x0, (int)y0, color, ca);
        if (x0 == x1 && y0 == y1) break;
        {
            long long e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static float fpart(float v) { return v - __builtin_floorf(v); }

void gfx_line_aa(gfx_surface *s, float x0, float y0, float x1, float y1, unsigned int color)
{
    int steep, i, i0, i1, lo, hi;
    float dx, dy, grad;
    u32 ca = GFX_A(color);
    if (!s || !s->px || ca == 0) return;
    if (!(x0 > -1e6f && x0 < 1e6f && y0 > -1e6f && y0 < 1e6f &&
          x1 > -1e6f && x1 < 1e6f && y1 > -1e6f && y1 < 1e6f)) return;
    /* pixel centres are at integer + 0.5; Wu works on integer centres */
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    steep = __builtin_fabsf(y1 - y0) > __builtin_fabsf(x1 - x0);
    if (steep) { float t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    dx = x1 - x0;
    dy = y1 - y0;
    grad = dx > 0.0f ? dy / dx : 1.0f;
    i0 = (int)__builtin_floorf(x0 + 0.5f);
    i1 = (int)__builtin_floorf(x1 + 0.5f);
    /* restrict the major-axis loop to the clip range */
    lo = steep ? s->clip_y0 : s->clip_x0;
    hi = steep ? s->clip_y1 - 1 : s->clip_x1 - 1;
    for (i = imax(i0, lo); i <= imin(i1, hi); i++) {
        float yy = y0 + grad * ((float)i - x0), w = 1.0f, f;
        int yi = (int)__builtin_floorf(yy);
        u32 a1, a2;
        if (i == i0) w = 1.0f - fpart(x0 + 0.5f);
        if (i == i1) w = (i0 == i1) ? (x1 - x0) : fpart(x1 + 0.5f);
        f = fpart(yy);
        a1 = mul255(ca, (u32)((1.0f - f) * w * 255.0f + 0.5f));
        a2 = mul255(ca, (u32)(f * w * 255.0f + 0.5f));
        if (steep) { plot_a(s, yi, i, color, a1); plot_a(s, yi + 1, i, color, a2); }
        else { plot_a(s, i, yi, color, a1); plot_a(s, i, yi + 1, color, a2); }
    }
}

void gfx_circle_fill(gfx_surface *s, int cx, int cy, int r, unsigned int color)
{
    int j, x0, y0, x1, y1;
    u32 ca = GFX_A(color);
    float fr = (float)r, rin2 = (fr - 0.5f) * (fr - 0.5f), rout2 = (fr + 0.5f) * (fr + 0.5f);
    if (ca == 0 || r <= 0) return;
    cx = clampi(cx, -GFX_LIM, GFX_LIM);
    cy = clampi(cy, -GFX_LIM, GFX_LIM);
    if (r > GFX_LIM) r = GFX_LIM;
    fr = (float)r;
    rin2 = (fr - 0.5f) * (fr - 0.5f);
    rout2 = (fr + 0.5f) * (fr + 0.5f);
    if (!clip_rect(s, cx - r, cy - r, 2 * r, 2 * r, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        float py = (float)j + 0.5f - (float)cy, py2 = py * py;
        int a = x0, b = x1;   /* [a, b) still to do */
        u32 *row = s->px + (long)j * s->stride;
        /* edge pixels from the left until fully covered */
        for (; a < b; a++) {
            float px = (float)a + 0.5f - (float)cx, d2 = px * px + py2;
            if (d2 <= rin2) break;
            if (d2 < rout2) {
                float c = fr - __builtin_sqrtf(d2) + 0.5f;
                put_a(row + a, color, mul255(ca, (u32)(c * 255.0f + 0.5f)));
            }
        }
        for (; b > a; b--) {
            float px = (float)(b - 1) + 0.5f - (float)cx, d2 = px * px + py2;
            if (d2 <= rin2) break;
            if (d2 < rout2) {
                float c = fr - __builtin_sqrtf(d2) + 0.5f;
                put_a(row + b - 1, color, mul255(ca, (u32)(c * 255.0f + 0.5f)));
            }
        }
        if (a < b) hspan_blend(row + a, b - a, color, ca);
    }
}

/* ------------------------------------------------------------------ */
/* Blits                                                               */

static int clip_blit(gfx_surface *dst, int *dx, int *dy, const gfx_surface *src,
                     int *sx, int *sy, int *w, int *h)
{
    long long ddx = *dx, ddy = *dy, ssx = *sx, ssy = *sy, ww = *w, hh = *h, d;
    if (!dst || !src || !dst->px || !src->px || ww <= 0 || hh <= 0) return 0;
    /* source bounds */
    if (ssx < 0) { ddx -= ssx; ww += ssx; ssx = 0; }
    if (ssy < 0) { ddy -= ssy; hh += ssy; ssy = 0; }
    if (ssx + ww > src->w) ww = src->w - ssx;
    if (ssy + hh > src->h) hh = src->h - ssy;
    /* destination clip */
    if (ddx < dst->clip_x0) { d = dst->clip_x0 - ddx; ssx += d; ww -= d; ddx = dst->clip_x0; }
    if (ddy < dst->clip_y0) { d = dst->clip_y0 - ddy; ssy += d; hh -= d; ddy = dst->clip_y0; }
    if (ddx + ww > dst->clip_x1) ww = dst->clip_x1 - ddx;
    if (ddy + hh > dst->clip_y1) hh = dst->clip_y1 - ddy;
    if (ww <= 0 || hh <= 0) return 0;
    *dx = (int)ddx; *dy = (int)ddy; *sx = (int)ssx; *sy = (int)ssy; *w = (int)ww; *h = (int)hh;
    return 1;
}

void gfx_blit(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h)
{
    int j;
    u32 *dp;
    const u32 *sp;
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    dp = dst->px + (long)dy * dst->stride + dx;
    sp = src->px + (long)sy * src->stride + sx;
    if (dp > sp) {   /* possible overlap in the same buffer: go bottom-up */
        for (j = h - 1; j >= 0; j--)
            memmove(dp + (long)j * dst->stride, sp + (long)j * src->stride, (size_t)w * 4);
    } else if (dp != sp) {
        for (j = 0; j < h; j++)
            memmove(dp + (long)j * dst->stride, sp + (long)j * src->stride, (size_t)w * 4);
    }
}

void gfx_blit_alpha(gfx_surface *dst, int dx, int dy, const gfx_surface *src, int sx, int sy, int w, int h)
{
    int j, i;
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    for (j = 0; j < h; j++) {
        u32 *dp = dst->px + (long)(dy + j) * dst->stride + dx;
        const u32 *sp = src->px + (long)(sy + j) * src->stride + sx;
        for (i = 0; i < w; i++) {
            u32 c = sp[i], a = c >> 24;
            if (a == 255) dp[i] = c;
            else if (a) dp[i] = gfx_blend_px(dp[i], c, a);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */

void gfx_glyph(gfx_surface *s, int pen_x, int baseline_y, const ttf_glyph *g, unsigned int color)
{
    int gx, gy, x0, y0, x1, y1, j, i, n;
    u32 ca = GFX_A(color), opaque = color | 0xFF000000u;
    if (!g || !g->cov || g->w <= 0 || g->h <= 0 || ca == 0) return;
    {
        long long lx = (long long)pen_x + g->left, ly = (long long)baseline_y - g->top;
        if (lx < -GFX_LIM || lx > GFX_LIM || ly < -GFX_LIM || ly > GFX_LIM) return;
        gx = (int)lx;
        gy = (int)ly;
    }
    if (!clip_rect(s, gx, gy, g->w, g->h, &x0, &y0, &x1, &y1)) return;
    n = x1 - x0;
    {
        /* colour terms hoisted out of the pixel loop */
        u32 crb = opaque & 0x00FF00FFu, cga = (opaque >> 8) & 0x00FF00FFu;
        for (j = y0; j < y1; j++) {
            u32 *dp = s->px + (long)j * s->stride + x0;
            const unsigned char *cp = g->cov + (long)(j - gy) * g->w + (x0 - gx);
            for (i = 0; i < n; i++) {
                u32 a = cp[i], ia, d, rb, ga;
                if (a == 0) continue;
                if (ca != 255) a = mul255(a, ca);
                if (a == 255) { dp[i] = opaque; continue; }
                ia = 255u - a;
                d = dp[i];
                rb = crb * a + (d & 0x00FF00FFu) * ia + 0x00800080u;
                ga = cga * a + ((d >> 8) & 0x00FF00FFu) * ia + 0x00800080u;
                rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
                ga = (ga + ((ga >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
                dp[i] = rb | ga;
            }
        }
    }
}

unsigned gfx_utf8_next(const char **pp, const char *end)
{
    const unsigned char *p = (const unsigned char *)*pp, *e = (const unsigned char *)end;
    unsigned c, n, i, min;
    if (p >= e) return 0xFFFD;
    c = *p;
    if (c < 0x80) { *pp = (const char *)(p + 1); return c; }
    if (c >= 0xC2 && c <= 0xDF) { n = 1; c &= 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; c &= 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; c &= 0x07; min = 0x10000; }
    else { *pp = (const char *)(p + 1); return 0xFFFD; }
    if ((unsigned long)(e - p) <= n) { *pp = (const char *)(p + 1); return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = (const char *)(p + 1); return 0xFFFD; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *pp = (const char *)(p + 1);
        return 0xFFFD;
    }
    *pp = (const char *)(p + n + 1);
    return c;
}

static const char *text_end(const char *s, int len)
{
    if (len >= 0) return s + len;
    while (*s) s++;
    return s;
}

int gfx_text(gfx_surface *s, ttf_cache *c, int x, int baseline_y, const char *utf8, int len,
             unsigned int color)
{
    const char *p = utf8, *e;
    if (!c || !utf8) return x;
    e = text_end(utf8, len);
    while (p < e) {
        unsigned cp = gfx_utf8_next(&p, e);
        const ttf_glyph *g = ttf_cache_get(c, cp);
        if (!g) break;
        if (s) gfx_glyph(s, x, baseline_y, g, color);
        x = clampi((long long)x + g->advance, -0x7fffffff, 0x7fffffff);
    }
    return x;
}

int gfx_text_width(ttf_cache *c, const char *utf8, int len)
{
    return gfx_text(0, c, 0, 0, utf8, len, 0);
}
