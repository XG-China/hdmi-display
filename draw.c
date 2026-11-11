/*
 * draw.c - software compositor: rectangles, lines, bitmap text, bars, waveform.
 * All drawing writes directly into a DRM dumb buffer (XRGB8888).
 */
#include "draw.h"
#include "font8x16.h"
#include "font_cjk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void cv_init(canvas *c, uint8_t *base, int pitch_bytes, int w, int h)
{
    c->px = base;
    c->pitch = pitch_bytes;
    c->w = w;
    c->h = h;
}

static inline uint32_t *px_at(canvas *c, int x, int y)
{
    return (uint32_t *)(c->px + (size_t)y * (size_t)c->pitch) + x;
}

static inline void px_set(canvas *c, int x, int y, uint32_t rgb)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h)
        *px_at(c, x, y) = rgb;
}

static inline void px_blend(canvas *c, int x, int y, uint32_t rgb, unsigned a)
{
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h)
        return;
    if (a == 0)
        return;
    if (a >= 255) {
        *px_at(c, x, y) = rgb;
        return;
    }
    uint32_t d = *px_at(c, x, y);
    unsigned ia = 255 - a;
    unsigned r = (((rgb >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia + 127) / 255;
    unsigned g = (((rgb >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia + 127) / 255;
    unsigned b = ((rgb & 0xff) * a + (d & 0xff) * ia + 127) / 255;
    *px_at(c, x, y) = (r << 16) | (g << 8) | b;
}

void cv_fill(canvas *c, uint32_t rgb)
{
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = px_at(c, 0, y);
        for (int x = 0; x < c->w; x++)
            row[x] = rgb;
    }
}

void cv_rect(canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = px_at(c, 0, yy);
        for (int xx = x0; xx < x1; xx++)
            row[xx] = rgb;
    }
}

void cv_rect_a(canvas *c, int x, int y, int w, int h, uint32_t rgb, unsigned alpha)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            px_blend(c, xx, yy, rgb, alpha);
}

void cv_hline(canvas *c, int x, int y, int w, int thickness, uint32_t rgb)
{
    cv_rect(c, x, y, w, thickness < 1 ? 1 : thickness, rgb);
}

void cv_vline(canvas *c, int x, int y, int h, int thickness, uint32_t rgb)
{
    cv_rect(c, x, y, thickness < 1 ? 1 : thickness, h, rgb);
}

void cv_outline(canvas *c, int x, int y, int w, int h, int t, uint32_t rgb)
{
    if (t < 1) t = 1;
    cv_rect(c, x, y, w, t, rgb);
    cv_rect(c, x, y + h - t, w, t, rgb);
    cv_rect(c, x, y, t, h, rgb);
    cv_rect(c, x + w - t, y, t, h, rgb);
}

int cv_utf8_next(const char **sp, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)*sp;
    if (!p || !*p) {
        if (cp) *cp = 0;
        return 0;
    }
    if (p[0] < 0x80) {
        if (cp) *cp = p[0];
        *sp += 1;
        return 1;
    }
    if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *sp += 2;
        return 2;
    }
    if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x0f) << 12) |
                      ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *sp += 3;
        return 3;
    }
    if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        if (cp) *cp = ((uint32_t)(p[0] & 0x07) << 18) |
                      ((uint32_t)(p[1] & 0x3f) << 12) |
                      ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        *sp += 4;
        return 4;
    }
    if (cp) *cp = 0xfffd;
    *sp += 1;
    return 1;
}

static const unsigned char *cjk_glyph(uint32_t cp)
{
    int lo = 0, hi = font_cjk_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t v = font_cjk_cp[mid];
        if (v == cp)
            return font_cjk_glyphs + (size_t)mid * 32;
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

int cv_utf8_cells(const char *s)
{
    int cells = 0;
    const char *p = s;
    while (p && *p) {
        uint32_t cp = 0;
        const char *q = p;
        int len = cv_utf8_next(&q, &cp);
        if (len <= 0)
            break;
        cells += (cp < 0x80 || !cjk_glyph(cp)) ? 1 : 2;
        p = q;
    }
    return cells;
}

static void draw_ascii(canvas *c, int x, int y, unsigned ch, uint32_t rgb,
                       int scale)
{
    if (ch < FONT_FIRST || ch > FONT_LAST)
        ch = '?';
    const unsigned char *g = font8x16[ch - FONT_FIRST];
    for (int gy = 0; gy < FONT_GLYPH_H; gy++) {
        unsigned bits = g[gy];
        if (!bits)
            continue;
        for (int gx = 0; gx < FONT_GLYPH_W; gx++) {
            if (!(bits & (0x80u >> gx)))
                continue;
            int bx = x + gx * scale;
            int by = y + gy * scale;
            for (int yy = 0; yy < scale; yy++)
                for (int xx = 0; xx < scale; xx++)
                    px_set(c, bx + xx, by + yy, rgb);
        }
    }
}

static void draw_cjk(canvas *c, int x, int y, const unsigned char *g,
                     uint32_t rgb, int scale)
{
    for (int gy = 0; gy < FONT_CJK_H; gy++) {
        unsigned bits = ((unsigned)g[gy * 2] << 8) | g[gy * 2 + 1];
        if (!bits)
            continue;
        for (int gx = 0; gx < FONT_CJK_W; gx++) {
            if (!(bits & (0x8000u >> gx)))
                continue;
            int bx = x + gx * scale;
            int by = y + gy * scale;
            for (int yy = 0; yy < scale; yy++)
                for (int xx = 0; xx < scale; xx++)
                    px_set(c, bx + xx, by + yy, rgb);
        }
    }
}

int cv_text_w(const char *s, int scale)
{
    if (scale < 1) scale = 1;
    int w = 0;
    const char *p = s;
    while (p && *p) {
        uint32_t cp = 0;
        const char *q = p;
        int len = cv_utf8_next(&q, &cp);
        if (len <= 0)
            break;
        if (cp < 0x80 || !cjk_glyph(cp))
            w += FONT_GLYPH_W * scale;
        else
            w += FONT_CJK_W * scale;
        p = q;
    }
    return w;
}

int cv_text_h(int scale)
{
    if (scale < 1) scale = 1;
    return FONT_GLYPH_H * scale;
}

void cv_text(canvas *c, int x, int y, const char *s, uint32_t rgb, int scale)
{
    if (scale < 1) scale = 1;
    const char *p = s;
    while (p && *p) {
        uint32_t cp = 0;
        const char *q = p;
        int len = cv_utf8_next(&q, &cp);
        if (len <= 0)
            break;
        if (cp < 0x80) {
            draw_ascii(c, x, y, (unsigned)cp, rgb, scale);
            x += FONT_GLYPH_W * scale;
        } else {
            const unsigned char *g = cjk_glyph(cp);
            if (g) {
                draw_cjk(c, x, y, g, rgb, scale);
                x += FONT_CJK_W * scale;
            } else {
                draw_ascii(c, x, y, '?', rgb, scale);
                x += FONT_GLYPH_W * scale;
            }
        }
        p = q;
    }
}

void cv_text_center(canvas *c, int cx, int y, const char *s, uint32_t rgb, int scale)
{
    cv_text(c, cx - cv_text_w(s, scale) / 2, y, s, rgb, scale);
}

void cv_bar(canvas *c, int x, int y, int w, int h, double value, uint32_t fg, uint32_t bg)
{
    if (w <= 0 || h <= 0)
        return;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    cv_rect(c, x, y, w, h, bg);
    int fill = (int)((w - 2) * value);
    if (fill > 0)
        cv_rect(c, x + 1, y + 1, fill, h - 2, fg);
    cv_outline(c, x, y, w, h, 1, RGB888(80, 90, 105));
}

void cv_wave(canvas *c, int x, int y, int w, int h, double t, double level, uint32_t rgb)
{
    if (w <= 2 || h <= 2)
        return;
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;
    int cy = y + h / 2;
    int half = h / 2 - 1;
    int step = h > 120 ? 3 : 2;
    for (int i = 0; i < w; i += step) {
        double px = (double)i;
        double env = 0.30 + 0.70 * (0.5 + 0.5 * sin(t * 0.9 + px * 0.012));
        double s = 0.62 * sin(px * 0.115 + t * 7.0) +
                   0.38 * sin(px * 0.310 - t * 3.3);
        int amp = (int)(half * level * env * fabs(s));
        if (amp < 1)
            amp = 1;
        cv_vline(c, x + i, cy - amp, amp * 2, step, rgb);
    }
    cv_hline(c, x, cy, w, 1, RGB888(40, 48, 60));
}
