#ifndef FONT_CJK_H_INCLUDED
#define FONT_CJK_H_INCLUDED

#include <stdint.h>

/* 16x16 GB2312 bitmap glyphs generated from Noto Sans CJK SC.
 * Noto Sans CJK is licensed under the SIL Open Font License 1.1; see
 * packaging/fonts/OFL-Noto.txt.
 * font_cjk_cp[] is sorted; glyph N occupies 32 bytes at N*32. */
#define FONT_CJK_W 16
#define FONT_CJK_H 16

extern const uint16_t font_cjk_cp[];
extern const unsigned char font_cjk_glyphs[];
extern const int font_cjk_count;

#endif /* FONT_CJK_H_INCLUDED */
