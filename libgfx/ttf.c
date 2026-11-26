// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ttf.c - TrueType (glyf) font parser and anti-aliased rasterizer.
 *
 * Rasterization uses exact-area signed coverage accumulation (the approach
 * of font-rs and stb_truetype v2): every outline edge adds the signed area
 * it covers in each pixel cell to an accumulation buffer, and a running sum
 * along each row turns that into coverage. Quadratic Beziers are flattened
 * adaptively into line segments first.
 */
#include "ttf.h"

#include <stddef.h>

/* The only libc functions used; declared here so no libc headers are needed. */
void *memset(void *dst, int c, size_t n);
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void free(void *p);

/* Limits that keep hostile fonts from exhausting memory or time. */
#define MAX_POINTS      (1 << 17)   /* points in one (composite) glyph */
#define MAX_DEPTH       12          /* composite nesting */
#define MAX_COMPONENTS  4096        /* total components per glyph */
#define MAX_BITMAP_DIM  4096
#define MAX_SEGMENTS    (1 << 20)
#define FLATTEN_TOL     0.02f       /* max flattening error in pixels */

/* ------------------------------------------------------------------ */
/* Bounds-checked big-endian readers. Out-of-range reads return 0.     */

static int in_range(unsigned len, unsigned off, unsigned n)
{
    return off <= len && n <= len - off;
}

static unsigned rd8(const ttf_font *f, unsigned off)
{
    return off < f->len ? f->data[off] : 0;
}

static unsigned rd16(const ttf_font *f, unsigned off)
{
    if (!in_range(f->len, off, 2)) return 0;
    return ((unsigned)f->data[off] << 8) | f->data[off + 1];
}

static int rds16(const ttf_font *f, unsigned off)
{
    return (int)(short)(unsigned short)rd16(f, off);
}

static unsigned rd32(const ttf_font *f, unsigned off)
{
    if (!in_range(f->len, off, 4)) return 0;
    return ((unsigned)f->data[off] << 24) | ((unsigned)f->data[off + 1] << 16) |
           ((unsigned)f->data[off + 2] << 8) | f->data[off + 3];
}

static unsigned tag(const char *s)
{
    return ((unsigned)(unsigned char)s[0] << 24) | ((unsigned)(unsigned char)s[1] << 16) |
           ((unsigned)(unsigned char)s[2] << 8) | (unsigned)(unsigned char)s[3];
}

/* ------------------------------------------------------------------ */
/* Table directory and cmap selection                                  */

static int find_table(const ttf_font *f, unsigned base, const char *name,
                      unsigned *off, unsigned *len)
{
    unsigned n = rd16(f, base + 4), i, t = tag(name);
    if (!in_range(f->len, base + 12, n * 16)) return 0;
    for (i = 0; i < n; i++) {
        unsigned rec = base + 12 + i * 16;
        if (rd32(f, rec) == t) {
            unsigned o = rd32(f, rec + 8), l = rd32(f, rec + 12);
            if (!in_range(f->len, o, l)) return 0;
            *off = o;
            *len = l;
            return 1;
        }
    }
    return 0;
}

/* Rank a cmap subtable: higher is better, 0 = unusable. */
static int cmap_rank(unsigned plat, unsigned enc, unsigned fmt)
{
    if (fmt == 12) {
        if (plat == 3 && enc == 10) return 6;
        if (plat == 0 && (enc == 4 || enc == 6)) return 5;
        if (plat == 0) return 4;
        return 0;
    }
    if (fmt == 4) {
        if (plat == 3 && enc == 1) return 3;
        if (plat == 0) return 2;
        if (plat == 3 && enc == 0) return 1;   /* symbol */
    }
    return 0;
}

static void select_cmap(ttf_font *f, unsigned cmap, unsigned cmap_len)
{
    unsigned n, i, best = 0;
    f->cmap_format = 0;
    if (cmap_len < 4) return;
    n = rd16(f, cmap + 2);
    if (4 + n * 8 > cmap_len) n = (cmap_len - 4) / 8;
    for (i = 0; i < n; i++) {
        unsigned rec = cmap + 4 + i * 8;
        unsigned plat = rd16(f, rec), enc = rd16(f, rec + 2);
        unsigned so = rd32(f, rec + 4), sub, fmt, slen, rank;
        if (so >= cmap_len) continue;
        sub = cmap + so;
        fmt = rd16(f, sub);
        if (fmt == 4) {
            slen = rd16(f, sub + 2);
        } else if (fmt == 12) {
            slen = rd32(f, sub + 4);
        } else {
            continue;
        }
        /* clamp the stated length to what is really there */
        if (slen > cmap_len - so) slen = cmap_len - so;
        if ((fmt == 4 && slen < 16) || (fmt == 12 && slen < 16)) continue;
        rank = (unsigned)cmap_rank(plat, enc, fmt);
        if (rank > best) {
            best = rank;
            f->cmap_sub = sub;
            f->cmap_sub_len = slen;
            f->cmap_format = (int)fmt;
        }
    }
}

int ttf_open(ttf_font *f, const unsigned char *data, unsigned len)
{
    unsigned base = 0, ver, head, head_len, hhea, hhea_len, maxp, maxp_len;
    unsigned cmap, cmap_len, os2, os2_len;
    int gm;

    if (!f) return TTF_ERR_ARG;
    memset(f, 0, sizeof *f);
    if (!data || len < 12) return TTF_ERR_FORMAT;
    f->data = data;
    f->len = len;

    ver = rd32(f, 0);
    if (ver == tag("ttcf")) {
        if (rd32(f, 8) < 1) return TTF_ERR_FORMAT;
        base = rd32(f, 12);
        if (!in_range(len, base, 12)) return TTF_ERR_FORMAT;
        ver = rd32(f, base);
    }
    if (ver != 0x00010000u && ver != tag("true")) return TTF_ERR_FORMAT;

    if (!find_table(f, base, "head", &head, &head_len) || head_len < 54) return TTF_ERR_TABLE;
    if (!find_table(f, base, "hhea", &hhea, &hhea_len) || hhea_len < 36) return TTF_ERR_TABLE;
    if (!find_table(f, base, "maxp", &maxp, &maxp_len) || maxp_len < 6) return TTF_ERR_TABLE;
    if (!find_table(f, base, "hmtx", &f->hmtx, &f->hmtx_len)) return TTF_ERR_TABLE;
    if (!find_table(f, base, "loca", &f->loca, &f->loca_len)) return TTF_ERR_TABLE;
    if (!find_table(f, base, "glyf", &f->glyf, &f->glyf_len)) return TTF_ERR_TABLE;

    f->units_per_em = (int)rd16(f, head + 18);
    if (f->units_per_em < 16 || f->units_per_em > 16384) return TTF_ERR_FORMAT;
    f->x_min = rds16(f, head + 36);
    f->y_min = rds16(f, head + 38);
    f->x_max = rds16(f, head + 40);
    f->y_max = rds16(f, head + 42);
    f->loca_long = rds16(f, head + 50) != 0;

    f->num_glyphs = (int)rd16(f, maxp + 4);
    if (f->num_glyphs < 1) return TTF_ERR_FORMAT;
    /* never trust num_glyphs beyond what loca can describe */
    gm = (int)(f->loca_len / (f->loca_long ? 4u : 2u)) - 1;
    if (gm < 1) return TTF_ERR_TABLE;
    if (f->num_glyphs > gm) f->num_glyphs = gm;

    f->ascender = rds16(f, hhea + 4);
    f->descender = rds16(f, hhea + 6);
    f->line_gap = rds16(f, hhea + 8);
    f->num_hmetrics = (int)rd16(f, hhea + 34);
    if ((unsigned)f->num_hmetrics * 4u > f->hmtx_len) f->num_hmetrics = (int)(f->hmtx_len / 4);
    if (f->num_hmetrics < 1) return TTF_ERR_TABLE;

    if (f->ascender == 0 && f->descender == 0 &&
        find_table(f, base, "OS/2", &os2, &os2_len) && os2_len >= 74) {
        f->ascender = rds16(f, os2 + 68);
        f->descender = rds16(f, os2 + 70);
        f->line_gap = rds16(f, os2 + 72);
    }

    if (find_table(f, base, "cmap", &cmap, &cmap_len)) select_cmap(f, cmap, cmap_len);

    f->advance_m = ttf_glyph_advance_units(f, ttf_glyph_index(f, 'M'));
    if (f->advance_m <= 0) f->advance_m = ttf_glyph_advance_units(f, 0);
    if (f->advance_m <= 0) f->advance_m = f->units_per_em / 2;
    return TTF_OK;
}

/* ------------------------------------------------------------------ */
/* cmap lookup                                                         */

static unsigned cmap4(const ttf_font *f, unsigned cp)
{
    unsigned sub = f->cmap_sub, slen = f->cmap_sub_len;
    unsigned segx2, seg, lo, hi, ends, starts, deltas, ranges, i;
    unsigned start, delta, ro, g;

    if (cp > 0xFFFF) return 0;
    segx2 = rd16(f, sub + 6) & ~1u;
    seg = segx2 / 2;
    if (seg == 0 || 16 + 4 * segx2 > slen) return 0;
    ends = sub + 14;
    starts = sub + 16 + segx2;
    deltas = starts + segx2;
    ranges = deltas + segx2;

    /* binary search: first segment whose endCode >= cp */
    lo = 0;
    hi = seg;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rd16(f, ends + mid * 2) < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= seg) return 0;
    i = lo;
    start = rd16(f, starts + i * 2);
    if (start > cp) return 0;
    delta = rd16(f, deltas + i * 2);
    ro = rd16(f, ranges + i * 2);
    if (ro == 0) return (cp + delta) & 0xFFFF;
    {
        unsigned addr = ranges + i * 2 + ro + 2 * (cp - start);
        /* stay inside the subtable */
        if (addr < sub || addr + 2 > sub + slen) return 0;
        g = rd16(f, addr);
        if (g == 0) return 0;
        return (g + delta) & 0xFFFF;
    }
}

static unsigned cmap12(const ttf_font *f, unsigned cp)
{
    unsigned sub = f->cmap_sub, slen = f->cmap_sub_len;
    unsigned n = rd32(f, sub + 12), lo = 0, hi;
    if (n > (slen - 16) / 12) n = (slen - 16) / 12;
    hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2, g = sub + 16 + mid * 12;
        unsigned s = rd32(f, g), e = rd32(f, g + 4);
        if (cp < s) hi = mid;
        else if (cp > e) lo = mid + 1;
        else return rd32(f, g + 8) + (cp - s);
    }
    return 0;
}

int ttf_glyph_index(const ttf_font *f, unsigned cp)
{
    unsigned g = 0;
    if (!f || !f->data) return 0;
    if (f->cmap_format == 4) g = cmap4(f, cp);
    else if (f->cmap_format == 12) g = cmap12(f, cp);
    if (g >= (unsigned)f->num_glyphs) return 0;
    return (int)g;
}

int ttf_glyph_advance_units(const ttf_font *f, int glyph)
{
    int i;
    if (!f || !f->data || glyph < 0 || glyph >= f->num_glyphs) return 0;
    i = glyph < f->num_hmetrics ? glyph : f->num_hmetrics - 1;
    return (int)rd16(f, f->hmtx + (unsigned)i * 4);
}

float ttf_scale(const ttf_font *f, float px)
{
    if (!f || f->units_per_em <= 0) return 0.0f;
    return px / (float)f->units_per_em;
}

static int iround(float v) { return (int)__builtin_floorf(v + 0.5f); }

void ttf_metrics_px(const ttf_font *f, float px, ttf_metrics *m)
{
    float s = ttf_scale(f, px);
    if (!m) return;
    if (!(s > 0.0f) || s > 1000.0f) {
        memset(m, 0, sizeof *m);
        return;
    }
    m->ascent = (int)__builtin_ceilf((float)f->ascender * s);
    m->descent = (int)__builtin_ceilf(-(float)f->descender * s);
    m->line_gap = iround((float)f->line_gap * s);
    m->line_height = m->ascent + m->descent + m->line_gap;
    m->advance = iround((float)f->advance_m * s);
}

/* ------------------------------------------------------------------ */
/* Outline loading                                                     */

typedef struct { float x, y; unsigned char on; } opt;

typedef struct {
    opt *pts;
    int npts, cap;
    int *ends;          /* index of last point of each contour */
    int nends, capends;
    int components;
} outline;

static int ol_reserve(outline *o, int n)
{
    if (n > MAX_POINTS) return TTF_ERR_LIMIT;
    if (n > o->cap) {
        int nc = o->cap ? o->cap : 64;
        opt *p;
        while (nc < n) nc *= 2;
        p = (opt *)realloc(o->pts, (size_t)nc * sizeof(opt));
        if (!p) return TTF_ERR_NOMEM;
        o->pts = p;
        o->cap = nc;
    }
    return 0;
}

static int ol_add_end(outline *o, int e)
{
    if (o->nends >= o->capends) {
        int nc = o->capends ? o->capends * 2 : 16;
        int *p = (int *)realloc(o->ends, (size_t)nc * sizeof(int));
        if (!p) return TTF_ERR_NOMEM;
        o->ends = p;
        o->capends = nc;
    }
    o->ends[o->nends++] = e;
    return 0;
}

/* Byte range of a glyph inside the file. 0 = ok (len may be 0). */
static int glyph_range(const ttf_font *f, int g, unsigned *off, unsigned *len)
{
    unsigned a, b;
    if (g < 0 || g >= f->num_glyphs) return TTF_ERR_ARG;
    if (f->loca_long) {
        a = rd32(f, f->loca + (unsigned)g * 4);
        b = rd32(f, f->loca + (unsigned)g * 4 + 4);
    } else {
        a = rd16(f, f->loca + (unsigned)g * 2) * 2;
        b = rd16(f, f->loca + (unsigned)g * 2 + 2) * 2;
    }
    if (a == b) { *off = 0; *len = 0; return 0; }
    if (a > b || b > f->glyf_len) return TTF_ERR_GLYPH;
    *off = f->glyf + a;
    *len = b - a;
    return 0;
}

static int load_glyph(const ttf_font *f, int g, outline *o, int depth);

static int load_simple(const ttf_font *f, unsigned p, unsigned end, int ncont, outline *o)
{
    int base = o->npts, npts, i, prev = -1, r;
    unsigned q, inslen;
    long long acc;

    if (ncont == 0) return 0;
    if (!in_range(end, p + 10, (unsigned)ncont * 2 + 2)) return TTF_ERR_GLYPH;
    for (i = 0; i < ncont; i++) {
        int e = (int)rd16(f, p + 10 + (unsigned)i * 2);
        if (e <= prev && i > 0) return TTF_ERR_GLYPH;
        prev = e;
    }
    npts = prev + 1;
    inslen = rd16(f, p + 10 + (unsigned)ncont * 2);
    q = p + 10 + (unsigned)ncont * 2 + 2 + inslen;
    if (q > end) return TTF_ERR_GLYPH;
    if ((r = ol_reserve(o, base + npts)) != 0) return r;

    /* flags (kept temporarily in .on) */
    for (i = 0; i < npts;) {
        unsigned fl, rep = 0;
        if (q >= end) return TTF_ERR_GLYPH;
        fl = rd8(f, q++);
        if (fl & 8) {
            if (q >= end) return TTF_ERR_GLYPH;
            rep = rd8(f, q++);
        }
        o->pts[base + i++].on = (unsigned char)fl;
        while (rep-- && i < npts) o->pts[base + i++].on = (unsigned char)fl;
    }
    /* x coordinates */
    acc = 0;
    for (i = 0; i < npts; i++) {
        unsigned fl = o->pts[base + i].on;
        if (fl & 2) {
            if (q >= end) return TTF_ERR_GLYPH;
            acc += (fl & 16) ? (int)rd8(f, q) : -(int)rd8(f, q);
            q++;
        } else if (!(fl & 16)) {
            if (q + 2 > end) return TTF_ERR_GLYPH;
            acc += rds16(f, q);
            q += 2;
        }
        o->pts[base + i].x = (float)acc;
    }
    /* y coordinates */
    acc = 0;
    for (i = 0; i < npts; i++) {
        unsigned fl = o->pts[base + i].on;
        if (fl & 4) {
            if (q >= end) return TTF_ERR_GLYPH;
            acc += (fl & 32) ? (int)rd8(f, q) : -(int)rd8(f, q);
            q++;
        } else if (!(fl & 32)) {
            if (q + 2 > end) return TTF_ERR_GLYPH;
            acc += rds16(f, q);
            q += 2;
        }
        o->pts[base + i].y = (float)acc;
        o->pts[base + i].on = (unsigned char)(fl & 1);
    }
    for (i = 0; i < ncont; i++) {
        int e = (int)rd16(f, p + 10 + (unsigned)i * 2);
        if ((r = ol_add_end(o, base + e)) != 0) return r;
    }
    o->npts = base + npts;
    return 0;
}

static float f2dot14(int v) { return (float)v / 16384.0f; }

static int load_composite(const ttf_font *f, unsigned p, unsigned end, outline *o, int depth)
{
    unsigned q = p + 10, flags;
    do {
        int child, base, r, i;
        float a = 1, b = 0, c = 0, d = 1, dx, dy;
        int arg1, arg2;

        if (++o->components > MAX_COMPONENTS) return TTF_ERR_LIMIT;
        if (q + 4 > end) return TTF_ERR_GLYPH;
        flags = rd16(f, q);
        child = (int)rd16(f, q + 2);
        q += 4;
        if (flags & 1) {            /* ARG_1_AND_2_ARE_WORDS */
            if (q + 4 > end) return TTF_ERR_GLYPH;
            if (flags & 2) { arg1 = rds16(f, q); arg2 = rds16(f, q + 2); }
            else { arg1 = (int)rd16(f, q); arg2 = (int)rd16(f, q + 2); }
            q += 4;
        } else {
            if (q + 2 > end) return TTF_ERR_GLYPH;
            if (flags & 2) { arg1 = (signed char)rd8(f, q); arg2 = (signed char)rd8(f, q + 1); }
            else { arg1 = (int)rd8(f, q); arg2 = (int)rd8(f, q + 1); }
            q += 2;
        }
        if (flags & 8) {            /* WE_HAVE_A_SCALE */
            if (q + 2 > end) return TTF_ERR_GLYPH;
            a = d = f2dot14(rds16(f, q));
            q += 2;
        } else if (flags & 0x40) {  /* WE_HAVE_AN_X_AND_Y_SCALE */
            if (q + 4 > end) return TTF_ERR_GLYPH;
            a = f2dot14(rds16(f, q));
            d = f2dot14(rds16(f, q + 2));
            q += 4;
        } else if (flags & 0x80) {  /* WE_HAVE_A_TWO_BY_TWO */
            if (q + 8 > end) return TTF_ERR_GLYPH;
            a = f2dot14(rds16(f, q));
            b = f2dot14(rds16(f, q + 2));
            c = f2dot14(rds16(f, q + 4));
            d = f2dot14(rds16(f, q + 6));
            q += 8;
        }

        base = o->npts;
        if ((r = load_glyph(f, child, o, depth + 1)) != 0) return r;
        for (i = base; i < o->npts; i++) {
            float x = o->pts[i].x, y = o->pts[i].y;
            o->pts[i].x = a * x + c * y;
            o->pts[i].y = b * x + d * y;
        }
        if (flags & 2) {            /* ARGS_ARE_XY_VALUES */
            dx = (float)arg1;
            dy = (float)arg2;
            if ((flags & 0x800) && !(flags & 0x1000)) {   /* SCALED_COMPONENT_OFFSET */
                float tx = a * dx + c * dy, ty = b * dx + d * dy;
                dx = tx;
                dy = ty;
            }
        } else {                    /* point matching */
            if (arg1 < 0 || arg1 >= base || arg2 < 0 || arg2 >= o->npts - base)
                return TTF_ERR_GLYPH;
            dx = o->pts[arg1].x - o->pts[base + arg2].x;
            dy = o->pts[arg1].y - o->pts[base + arg2].y;
        }
        for (i = base; i < o->npts; i++) {
            o->pts[i].x += dx;
            o->pts[i].y += dy;
        }
    } while (flags & 0x20);         /* MORE_COMPONENTS */
    return 0;
}

static int load_glyph(const ttf_font *f, int g, outline *o, int depth)
{
    unsigned off, len;
    int ncont, r;
    if (depth > MAX_DEPTH) return TTF_ERR_LIMIT;
    if ((r = glyph_range(f, g, &off, &len)) != 0) return r;
    if (len == 0) return 0;
    if (len < 10) return TTF_ERR_GLYPH;
    ncont = rds16(f, off);
    if (ncont >= 0) return load_simple(f, off, off + len, ncont, o);
    return load_composite(f, off, off + len, o, depth);
}

/* ------------------------------------------------------------------ */
/* Flattening into line segments (pixel space, y down)                 */

typedef struct { float x0, y0, x1, y1; } seg;

typedef struct {
    seg *s;
    int n, cap;
    int err;
} seglist;

static void add_seg(seglist *l, float x0, float y0, float x1, float y1)
{
    if (l->err) return;
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 256;
        seg *p;
        if (nc > MAX_SEGMENTS) { l->err = TTF_ERR_LIMIT; return; }
        p = (seg *)realloc(l->s, (size_t)nc * sizeof(seg));
        if (!p) { l->err = TTF_ERR_NOMEM; return; }
        l->s = p;
        l->cap = nc;
    }
    l->s[l->n].x0 = x0;
    l->s[l->n].y0 = y0;
    l->s[l->n].x1 = x1;
    l->s[l->n].y1 = y1;
    l->n++;
}

static void add_quad(seglist *l, float x0, float y0, float cx, float cy, float x1, float y1)
{
    float ddx = x0 - 2.0f * cx + x1, ddy = y0 - 2.0f * cy + y1;
    float dev = ddx * ddx + ddy * ddy;
    int n, i;
    float px, py;
    if (dev < 16.0f * FLATTEN_TOL * FLATTEN_TOL) {
        add_seg(l, x0, y0, x1, y1);
        return;
    }
    /* the max distance between a quad and its chord is |p0-2c+p1|/4; with
     * n segments it is |p0-2c+p1|/(4 n^2). Pick n for an error <= TOL px. */
    n = 1 + (int)__builtin_floorf(__builtin_sqrtf(__builtin_sqrtf(dev) * (0.25f / FLATTEN_TOL)));
    if (n > 64) n = 64;
    px = x0;
    py = y0;
    for (i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float x = mt * mt * x0 + 2.0f * mt * t * cx + t * t * x1;
        float y = mt * mt * y0 + 2.0f * mt * t * cy + t * t * y1;
        if (i == n) { x = x1; y = y1; }
        add_seg(l, px, py, x, y);
        px = x;
        py = y;
    }
}

static void flatten(const outline *o, const float *tm, seglist *l)
{
    int c, start = 0;
    for (c = 0; c < o->nends; c++) {
        int end = o->ends[c], n = end - start + 1, i, first, have_ctrl = 0;
        float sx, sy, curx, cury, ctx = 0, cty = 0;
        if (end >= o->npts) break;
        if (n < 2) { start = end + 1; continue; }
#define PX(k) (o->pts[k].x * tm[0] + tm[2])
#define PY(k) (-(o->pts[k].y * tm[1] + tm[3]))
        if (o->pts[start].on) {
            sx = PX(start); sy = PY(start); first = 1;
        } else if (o->pts[end].on) {
            sx = PX(end); sy = PY(end); first = 0; n--;   /* end is the start */
        } else {
            sx = 0.5f * (PX(start) + PX(end));
            sy = 0.5f * (PY(start) + PY(end));
            first = 0;
        }
        curx = sx;
        cury = sy;
        for (i = first; i < n; i++) {
            int k = start + i;
            float x = PX(k), y = PY(k);
            if (o->pts[k].on) {
                if (have_ctrl) add_quad(l, curx, cury, ctx, cty, x, y);
                else add_seg(l, curx, cury, x, y);
                curx = x; cury = y;
                have_ctrl = 0;
            } else {
                if (have_ctrl) {
                    float mx = 0.5f * (ctx + x), my = 0.5f * (cty + y);
                    add_quad(l, curx, cury, ctx, cty, mx, my);
                    curx = mx; cury = my;
                }
                ctx = x; cty = y;
                have_ctrl = 1;
            }
        }
        if (have_ctrl) add_quad(l, curx, cury, ctx, cty, sx, sy);
        else add_seg(l, curx, cury, sx, sy);
#undef PX
#undef PY
        start = end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Signed-area accumulation rasterizer                                 */

/* acc has h rows of `stride` (= w + 2) floats. Coordinates are already
 * translated so the outline lies inside [0,w] x [0,h]. */
static void acc_line(float *acc, int w, int h, int stride,
                     float x0, float y0, float x1, float y1)
{
    float dir, dxdy, x, fw = (float)w;
    int y, ys, ye;

    if (y0 == y1) return;
    if (y0 < y1) {
        dir = 1.0f;
    } else {
        float t;
        dir = -1.0f;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    dxdy = (x1 - x0) / (y1 - y0);
    x = x0;
    if (y0 < 0.0f) { x -= y0 * dxdy; ys = 0; }
    else ys = (int)y0;
    ye = (int)__builtin_ceilf(y1);
    if (ye > h) ye = h;

    for (y = ys; y < ye; y++) {
        float *row = acc + (long)y * stride;
        float ytop = (float)y > y0 ? (float)y : y0;
        float ybot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = ybot - ytop, xnext = x + dxdy * dy, d = dy * dir;
        float xa, xb, xaf;
        int xai, xbi;

        if (x < xnext) { xa = x; xb = xnext; } else { xa = xnext; xb = x; }
        if (xa < 0.0f) xa = 0.0f;
        if (xb < 0.0f) xb = 0.0f;
        if (xa > fw) xa = fw;
        if (xb > fw) xb = fw;
        xaf = __builtin_floorf(xa);
        xai = (int)xaf;
        xbi = (int)__builtin_ceilf(xb);

        if (xbi <= xai + 1) {
            /* the edge stays inside one pixel column on this row */
            float xmf = 0.5f * (xa + xb) - xaf;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float sl = 1.0f / (xb - xa);
            float x0f = xa - xaf;
            float a0 = 0.5f * sl * (1.0f - x0f) * (1.0f - x0f);
            float x1f = xb - (float)xbi + 1.0f;
            float am = 0.5f * sl * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = sl * (1.5f - x0f), a2;
                int xi;
                row[xai + 1] += d * (a1 - a0);
                for (xi = xai + 2; xi < xbi - 1; xi++) row[xi] += d * sl;
                a2 = a1 + (float)(xbi - xai - 3) * sl;
                row[xbi - 1] += d * (1.0f - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

int ttf_render(const ttf_font *f, int glyph, float px, ttf_glyph *out)
{
    float s = ttf_scale(f, px);
    if (!(px > 0.0f) || px > 2048.0f) {
        if (out) memset(out, 0, sizeof *out);
        return TTF_ERR_ARG;
    }
    return ttf_render_ex(f, glyph, s, s, 0.0f, 0.0f, iround((float)ttf_glyph_advance_units(f, glyph) * s), out);
}

int ttf_render_ex(const ttf_font *f, int glyph, float sx, float sy, float dx, float dy,
                  int advance, ttf_glyph *out)
{
    outline o;
    seglist l;
    float tm[4], minx, miny, maxx, maxy, ox, oy;
    int r, i, w, h, stride, x0, y0;
    float *acc;
    unsigned char *cov;

    if (!out) return TTF_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!f || !f->data) return TTF_ERR_ARG;
    if (glyph < 0 || glyph >= f->num_glyphs) return TTF_ERR_ARG;
    /* NaN fails every comparison, so these also reject NaN */
    if (!(sx > 0.0f && sx <= 256.0f && sy > 0.0f && sy <= 256.0f &&
          dx > -1e4f && dx < 1e4f && dy > -1e4f && dy < 1e4f)) return TTF_ERR_ARG;
    tm[0] = sx; tm[1] = sy; tm[2] = dx; tm[3] = dy;

    memset(&o, 0, sizeof o);
    memset(&l, 0, sizeof l);
    r = load_glyph(f, glyph, &o, 0);
    if (r == 0) {
        flatten(&o, tm, &l);
        r = l.err;
    }
    free(o.pts);
    free(o.ends);
    if (r != 0) {
        free(l.s);
        return r;
    }
    out->advance = advance;
    if (l.n == 0) {
        free(l.s);
        return 0;
    }

    minx = maxx = l.s[0].x0;
    miny = maxy = l.s[0].y0;
    for (i = 0; i < l.n; i++) {
        float xs[2] = { l.s[i].x0, l.s[i].x1 }, ys[2] = { l.s[i].y0, l.s[i].y1 };
        int k;
        for (k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }
    /* outlines are at most 16 bits * 2 (composite offsets) * scale, but
     * guard against absurd values before converting to int */
    if (!(minx > -1e6f && maxx < 1e6f && miny > -1e6f && maxy < 1e6f)) {
        free(l.s);
        return TTF_ERR_LIMIT;
    }
    x0 = (int)__builtin_floorf(minx);
    y0 = (int)__builtin_floorf(miny);
    w = (int)__builtin_ceilf(maxx) - x0;
    h = (int)__builtin_ceilf(maxy) - y0;
    if (w <= 0 || h <= 0) {        /* degenerate (zero-area) outline */
        free(l.s);
        return 0;
    }
    if (w > MAX_BITMAP_DIM || h > MAX_BITMAP_DIM) {
        free(l.s);
        return TTF_ERR_LIMIT;
    }

    stride = w + 2;
    acc = (float *)calloc((size_t)stride * (size_t)h, sizeof(float));
    cov = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!acc || !cov) {
        free(acc);
        free(cov);
        free(l.s);
        return TTF_ERR_NOMEM;
    }
    ox = (float)x0;
    oy = (float)y0;
    for (i = 0; i < l.n; i++)
        acc_line(acc, w, h, stride, l.s[i].x0 - ox, l.s[i].y0 - oy,
                 l.s[i].x1 - ox, l.s[i].y1 - oy);
    free(l.s);

    for (r = 0; r < h; r++) {
        const float *row = acc + (long)r * stride;
        unsigned char *dst = cov + (long)r * w;
        float sum = 0.0f;
        int x;
        for (x = 0; x < w; x++) {
            float v;
            sum += row[x];
            v = __builtin_fabsf(sum);
            if (v > 1.0f) v = 1.0f;
            dst[x] = (unsigned char)(int)(v * 255.0f + 0.5f);
        }
    }
    free(acc);

    /* Trim all-zero border rows and columns (float noise can add a row of
     * zero coverage when an extreme lands just past a pixel boundary). */
    {
        int t = 0, btm = h - 1, lft = w, rgt = -1, x, y;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (cov[(long)y * w + x]) {
                    if (x < lft) lft = x;
                    if (x > rgt) rgt = x;
                }
        if (rgt < 0) {               /* nothing visible */
            free(cov);
            return 0;
        }
        while (t < h) {
            for (x = 0; x < w && !cov[(long)t * w + x]; x++) {}
            if (x < w) break;
            t++;
        }
        while (btm > t) {
            for (x = 0; x < w && !cov[(long)btm * w + x]; x++) {}
            if (x < w) break;
            btm--;
        }
        if (t > 0 || btm < h - 1 || lft > 0 || rgt < w - 1) {
            int nw = rgt - lft + 1, nh = btm - t + 1;
            /* destination never runs ahead of the source: copy forward */
            for (y = 0; y < nh; y++)
                for (x = 0; x < nw; x++)
                    cov[(long)y * nw + x] = cov[(long)(y + t) * w + x + lft];
            x0 += lft;
            y0 += t;
            w = nw;
            h = nh;
        }
    }

    out->w = w;
    out->h = h;
    out->left = x0;
    out->top = -y0;
    out->cov = cov;
    return 0;
}

void ttf_glyph_free(ttf_glyph *g)
{
    if (!g) return;
    free(g->cov);
    g->cov = 0;
    g->w = g->h = 0;
}

/* ------------------------------------------------------------------ */
/* Glyph cache: chained hash table + doubly linked LRU list             */

typedef struct {
    int glyph;          /* key: glyph index | CELL_KEY if cell-snapped, -1 = free */
    int next;           /* hash chain */
    int prev_lru, next_lru;
    ttf_glyph g;
} cache_ent;

#define CELL_KEY 0x10000

struct ttf_cache {
    const ttf_font *f;
    float px;
    int flags;
    ttf_metrics m;
    int max, used;
    int nbuckets;       /* power of two */
    int *buckets;
    cache_ent *ents;
    int lru_head, lru_tail;   /* head = most recently used */
    int ascii[128];           /* code point -> glyph index for ASCII */
};

static unsigned hash_glyph(int g) { return (unsigned)g * 2654435761u; }

ttf_cache *ttf_cache_new(const ttf_font *f, float px, int max_glyphs)
{
    return ttf_cache_new_ex(f, px, max_glyphs, TTF_CACHE_CELL_SNAP);
}

ttf_cache *ttf_cache_new_ex(const ttf_font *f, float px, int max_glyphs, int flags)
{
    ttf_cache *c;
    int i, nb = 16;
    if (!f || !f->data || !(px > 0.0f) || px > 2048.0f) return 0;
    if (max_glyphs < 8) max_glyphs = 8;
    if (max_glyphs > 65536) max_glyphs = 65536;
    while (nb < max_glyphs * 2) nb *= 2;
    c = (ttf_cache *)calloc(1, sizeof *c);
    if (!c) return 0;
    c->buckets = (int *)malloc((size_t)nb * sizeof(int));
    c->ents = (cache_ent *)calloc((size_t)max_glyphs, sizeof(cache_ent));
    if (!c->buckets || !c->ents) {
        free(c->buckets);
        free(c->ents);
        free(c);
        return 0;
    }
    c->f = f;
    c->px = px;
    c->flags = flags;
    ttf_metrics_px(f, px, &c->m);
    c->max = max_glyphs;
    c->nbuckets = nb;
    for (i = 0; i < nb; i++) c->buckets[i] = -1;
    for (i = 0; i < max_glyphs; i++) c->ents[i].glyph = -1;
    c->lru_head = c->lru_tail = -1;
    for (i = 0; i < 128; i++) c->ascii[i] = ttf_glyph_index(f, (unsigned)i);
    return c;
}

static void lru_unlink(ttf_cache *c, int i)
{
    cache_ent *e = &c->ents[i];
    if (e->prev_lru >= 0) c->ents[e->prev_lru].next_lru = e->next_lru;
    else c->lru_head = e->next_lru;
    if (e->next_lru >= 0) c->ents[e->next_lru].prev_lru = e->prev_lru;
    else c->lru_tail = e->prev_lru;
}

static void lru_push_front(ttf_cache *c, int i)
{
    cache_ent *e = &c->ents[i];
    e->prev_lru = -1;
    e->next_lru = c->lru_head;
    if (c->lru_head >= 0) c->ents[c->lru_head].prev_lru = i;
    c->lru_head = i;
    if (c->lru_tail < 0) c->lru_tail = i;
}

int ttf_is_cell_glyph(unsigned cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) ||    /* box drawing, block elements */
           (cp >= 0xE0B0 && cp <= 0xE0D7) ||    /* powerline and powerline extra */
           (cp >= 0x1FB00 && cp <= 0x1FBFF);    /* symbols for legacy computing */
}

/* Render glyph stretched so its advance box maps exactly onto the pixel
 * cell: x [0, advance] -> [0, round(advance)], y [descender, ascender] ->
 * [-descent, ascent]. */
static int render_cell(const ttf_cache *c, int glyph, ttf_glyph *out)
{
    const ttf_font *f = c->f;
    float s = ttf_scale(f, c->px), sx = s, sy = s, dy = 0.0f;
    int adv = ttf_glyph_advance_units(f, glyph);
    int cw = iround((float)adv * s);
    if (adv > 0 && cw > 0) sx = (float)cw / (float)adv;
    if (f->ascender > f->descender && c->m.ascent + c->m.descent > 0) {
        sy = (float)(c->m.ascent + c->m.descent) / (float)(f->ascender - f->descender);
        dy = (float)c->m.ascent - (float)f->ascender * sy;
    }
    return ttf_render_ex(f, glyph, sx, sy, 0.0f, dy, cw, out);
}

static const ttf_glyph *cache_lookup(ttf_cache *c, int glyph, int cell)
{
    unsigned b;
    int i, *pp, key;
    cache_ent *e;
    if (glyph < 0 || glyph >= c->f->num_glyphs) glyph = 0;
    key = glyph | (cell ? CELL_KEY : 0);
    b = hash_glyph(key) & (unsigned)(c->nbuckets - 1);
    for (i = c->buckets[b]; i >= 0; i = c->ents[i].next) {
        if (c->ents[i].glyph == key) {
            if (c->lru_head != i) {
                lru_unlink(c, i);
                lru_push_front(c, i);
            }
            return &c->ents[i].g;
        }
    }
    /* miss: take a free slot or evict the least recently used one */
    if (c->used < c->max) {
        i = c->used++;
    } else {
        unsigned ob;
        i = c->lru_tail;
        lru_unlink(c, i);
        ob = hash_glyph(c->ents[i].glyph) & (unsigned)(c->nbuckets - 1);
        for (pp = &c->buckets[ob]; *pp >= 0; pp = &c->ents[*pp].next) {
            if (*pp == i) { *pp = c->ents[i].next; break; }
        }
        ttf_glyph_free(&c->ents[i].g);
    }
    e = &c->ents[i];
    e->glyph = key;
    if ((cell ? render_cell(c, glyph, &e->g) : ttf_render(c->f, glyph, c->px, &e->g)) != 0) {
        memset(&e->g, 0, sizeof e->g);
        e->g.advance = iround((float)ttf_glyph_advance_units(c->f, glyph) * ttf_scale(c->f, c->px));
    }
    e->next = c->buckets[b];
    c->buckets[b] = i;
    lru_push_front(c, i);
    return &e->g;
}

const ttf_glyph *ttf_cache_get_glyph(ttf_cache *c, int glyph)
{
    if (!c) return 0;
    return cache_lookup(c, glyph, 0);
}

const ttf_glyph *ttf_cache_get(ttf_cache *c, unsigned codepoint)
{
    if (!c) return 0;
    if (codepoint < 128) return cache_lookup(c, c->ascii[codepoint], 0);
    return cache_lookup(c, ttf_glyph_index(c->f, codepoint),
                        (c->flags & TTF_CACHE_CELL_SNAP) && ttf_is_cell_glyph(codepoint));
}

void ttf_cache_free(ttf_cache *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < c->used; i++) free(c->ents[i].g.cov);
    free(c->ents);
    free(c->buckets);
    free(c);
}

const ttf_font *ttf_cache_font(const ttf_cache *c) { return c ? c->f : 0; }
float ttf_cache_px(const ttf_cache *c) { return c ? c->px : 0.0f; }
