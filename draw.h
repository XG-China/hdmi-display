/*
 * draw.h - tiny software drawing library for 32-bit XRGB8888 surfaces.
 *
 * The framebuffer format used by the DRM code is DRM_FORMAT_XRGB8888, which on
 * little-endian machines is stored as bytes B,G,R,X.  A canvas pixel is written
 * as 0x00RRGGBB, i.e. exactly that byte order.
 */
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

typedef struct {
    uint8_t *px;    /* top-left pixel */
    int      pitch; /* bytes per scan line */
    int      w;
    int      h;
} canvas;

#define RGB888(r, g, b) ((uint32_t)((((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))

#define COL_WHITE   RGB888(255, 255, 255)
#define COL_BLACK   RGB888(0, 0, 0)
#define COL_GREY    RGB888(150, 158, 168)
#define COL_DIM     RGB888(90, 98, 110)
#define COL_GREEN   RGB888(70, 220, 120)
#define COL_RED     RGB888(240, 70, 70)
#define COL_AMBER   RGB888(255, 190, 60)
#define COL_CYAN    RGB888(60, 200, 240)

void cv_init(canvas *c, uint8_t *base, int pitch_bytes, int w, int h);

void cv_fill(canvas *c, uint32_t rgb);
void cv_rect(canvas *c, int x, int y, int w, int h, uint32_t rgb);
void cv_rect_a(canvas *c, int x, int y, int w, int h, uint32_t rgb, unsigned alpha);
void cv_outline(canvas *c, int x, int y, int w, int h, int thickness, uint32_t rgb);
void cv_hline(canvas *c, int x, int y, int w, int thickness, uint32_t rgb);
void cv_vline(canvas *c, int x, int y, int h, int thickness, uint32_t rgb);

/* 8x16 ASCII + 16x16 GB2312 bitmap font; UTF-8 input.
 * scale >= 1 multiplies the glyph size. */
void cv_text(canvas *c, int x, int y, const char *s, uint32_t rgb, int scale);
void cv_text_center(canvas *c, int cx, int y, const char *s, uint32_t rgb, int scale);
int  cv_text_w(const char *s, int scale);
int  cv_text_h(int scale);

/* UTF-8 helpers: decode one codepoint and count display cells
 * (ASCII = 1 cell, CJK = 2 cells). */
int  cv_utf8_next(const char **s, uint32_t *cp);
int  cv_utf8_cells(const char *s);

/* Horizontal progress / level bar with border. value in 0.0 .. 1.0 */
void cv_bar(canvas *c, int x, int y, int w, int h, double value, uint32_t fg, uint32_t bg);

/* Decorative voice waveform, t = seconds, level = 0.0 .. 1.0 */
void cv_wave(canvas *c, int x, int y, int w, int h, double t, double level, uint32_t rgb);

#endif /* DRAW_H */
