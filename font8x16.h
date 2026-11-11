#ifndef FONT8X16_H
#define FONT8X16_H

#define FONT_GLYPH_W 8
#define FONT_GLYPH_H 16
#define FONT_FIRST   32
#define FONT_LAST    126
#define FONT_GLYPH_COUNT (FONT_LAST - FONT_FIRST + 1)

/* One byte per scan line, bit 7 = leftmost pixel. */
extern const unsigned char font8x16[FONT_GLYPH_COUNT][FONT_GLYPH_H];

#endif /* FONT8X16_H */
