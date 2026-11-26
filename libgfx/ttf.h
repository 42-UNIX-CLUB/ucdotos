// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ttf.h - TrueType (glyf) font parser and anti-aliased rasterizer.
 *
 * Freestanding C11. Needs only memcpy/memset/malloc/calloc/realloc/free.
 * The font is a byte buffer owned by the caller; it must stay alive for as
 * long as the ttf_font (and any ttf_cache built from it) is in use.
 *
 * Every table access is bounds-checked: a malformed font makes ttf_open or
 * ttf_render return a negative error code, it never reads outside the
 * buffer.
 */
#ifndef UCOS_TTF_H
#define UCOS_TTF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (all negative). */
#define TTF_OK          0
#define TTF_ERR_FORMAT (-1)   /* not a TrueType font, bad header */
#define TTF_ERR_TABLE  (-2)   /* required table missing or out of bounds */
#define TTF_ERR_GLYPH  (-3)   /* glyph data malformed */
#define TTF_ERR_NOMEM  (-4)   /* malloc failed */
#define TTF_ERR_ARG    (-5)   /* bad argument (glyph index, size) */
#define TTF_ERR_LIMIT  (-6)   /* glyph too complex or bitmap too large */

typedef struct ttf_font {
    const unsigned char *data;
    unsigned len;

    /* absolute offsets and lengths of the tables we use */
    unsigned glyf, glyf_len;
    unsigned loca, loca_len;
    unsigned hmtx, hmtx_len;
    unsigned cmap_sub, cmap_sub_len;  /* selected cmap subtable */
    int cmap_format;                   /* 4 or 12, 0 = no usable cmap */

    int units_per_em;
    int num_glyphs;
    int loca_long;                     /* head.indexToLocFormat */
    int num_hmetrics;

    /* vertical metrics in font units (descender is negative, as stored) */
    int ascender, descender, line_gap;
    int x_min, y_min, x_max, y_max;    /* head bounding box */
    int advance_m;                     /* advance width of 'M' in font units */
} ttf_font;

/* Parse the font. 0 on success, negative TTF_ERR_* on error.
 * A TrueType collection (ttcf) opens its first font. */
int ttf_open(ttf_font *f, const unsigned char *data, unsigned len);

/* Map a Unicode code point to a glyph index. 0 = missing glyph (.notdef). */
int ttf_glyph_index(const ttf_font *f, unsigned codepoint);

/* Advance width of a glyph in font units (0 for an invalid index). */
int ttf_glyph_advance_units(const ttf_font *f, int glyph);

/* Scale factor from font units to pixels for a size in pixels per em. */
float ttf_scale(const ttf_font *f, float px);

/* Font-wide metrics in whole pixels at size px:
 *   ascent      pixels above the baseline (ceil, like FreeType)
 *   descent     pixels below the baseline, as a POSITIVE number (ceil)
 *   line_gap    hhea lineGap (rounded)
 *   line_height ascent + descent + line_gap
 *   advance     advance of 'M' rounded to whole pixels (monospace cell width)
 */
typedef struct { int ascent, descent, line_gap, line_height, advance; } ttf_metrics;
void ttf_metrics_px(const ttf_font *f, float px, ttf_metrics *m);

/* A rendered glyph.
 *   cov      w*h bytes of 8-bit coverage, row-major, top row first
 *            (NULL when w or h is 0, e.g. for a space)
 *   left     x offset from the pen position to the bitmap's left column
 *   top      distance from the baseline UP to the bitmap's top row
 *            (FreeType bitmap_top convention), so the top-left pixel is at
 *            (pen_x + left, baseline_y - top) in y-down screen space
 *   advance  horizontal advance in whole pixels (rounded)
 */
typedef struct { int w, h, left, top, advance; unsigned char *cov; } ttf_glyph;

/* Rasterize a glyph at px pixels per em. Allocates out->cov with malloc
 * (free it with ttf_glyph_free or free()). 0 on success, negative on error;
 * on error out is zeroed and nothing is allocated. */
int ttf_render(const ttf_font *f, int glyph, float px, ttf_glyph *out);
/* General form: a font-unit point (X, Y) (Y up) maps to pixel
 * (X*sx + dx, Y*sy + dy) relative to the pen on the baseline (Y up).
 * `advance` is copied into out->advance. ttf_render(px) is
 * ttf_render_ex(s, s, 0, 0, round(adv*s)) with s = ttf_scale(f, px). */
int ttf_render_ex(const ttf_font *f, int glyph, float sx, float sy, float dx, float dy,
                  int advance, ttf_glyph *out);
void ttf_glyph_free(ttf_glyph *g);

/* Glyph cache: fixed-size hash table keyed by glyph index (the size is
 * fixed per cache), least-recently-used eviction.
 * ttf_cache_get never returns NULL for a valid cache: a glyph that fails to
 * render comes back as an empty bitmap with its advance. The returned
 * pointer stays valid until the next ttf_cache_get call on the same cache
 * (which may evict it) or ttf_cache_free. */
typedef struct ttf_cache ttf_cache;

/* TTF_CACHE_CELL_SNAP: glyphs that are meant to tile a character grid (box
 * drawing U+2500-257F, block elements U+2580-259F, powerline U+E0B0-E0D7,
 * legacy computing U+1FB00-1FBFF) are stretched so that their advance box
 * covers exactly the integer cell: x [0, advance] -> [0, round(advance)] and
 * y [hhea descender, ascender] -> [-descent, ascent] of ttf_metrics_px.
 * Without it, an advance of e.g. 9.6 px drawn in a 10 px cell leaves
 * visible seams between adjacent blocks. The distortion is at most half a
 * pixel per axis; ordinary text is never stretched.
 * ttf_cache_new enables it; use ttf_cache_new_ex(..., 0) to disable. */
#define TTF_CACHE_CELL_SNAP 1
ttf_cache *ttf_cache_new(const ttf_font *f, float px, int max_glyphs);
ttf_cache *ttf_cache_new_ex(const ttf_font *f, float px, int max_glyphs, int flags);
/* 1 if the code point gets cell snapping (see TTF_CACHE_CELL_SNAP). */
int ttf_is_cell_glyph(unsigned codepoint);
const ttf_glyph *ttf_cache_get(ttf_cache *c, unsigned codepoint);
const ttf_glyph *ttf_cache_get_glyph(ttf_cache *c, int glyph);
void ttf_cache_free(ttf_cache *c);
const ttf_font *ttf_cache_font(const ttf_cache *c);
float ttf_cache_px(const ttf_cache *c);

#ifdef __cplusplus
}
#endif
#endif
