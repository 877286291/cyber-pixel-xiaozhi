/*******************************************************************************
 * Size: 8 px
 * Bpp: 4
 * Opts: --font silkscreen/Silkscreen-Regular.ttf --range 0x20-0x7E --size 8 --bpp 4 --format lvgl --lv-font-name cyber_font_sm --no-kerning -o generated/cyber_font_sm.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef CYBER_FONT_SM
#define CYBER_FONT_SM 1
#endif

#if CYBER_FONT_SM

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+0021 "!" */
    0xf0, 0x7, 0xf8,

    /* U+0022 "\"" */
    0xf0, 0xf0, 0x8,

    /* U+0023 "#" */
    0xf, 0xf, 0xf, 0xf, 0xf, 0xf0, 0x78, 0x7f,
    0x83, 0xc3, 0xfc, 0x1e, 0x1e,

    /* U+0024 "$" */
    0x0, 0x78, 0x3, 0xc3, 0xff, 0xf0, 0x3, 0xff,
    0xf0, 0x78, 0x0,

    /* U+0025 "%" */
    0xff, 0x7, 0x80, 0x7b, 0xff, 0x0, 0x3f, 0xf0,
    0x7, 0x0,

    /* U+0026 "&" */
    0x0, 0x78, 0x3, 0xc3, 0xff, 0xf0, 0x7f, 0xc1,
    0xff, 0x83, 0xc3, 0xc0,

    /* U+0027 "'" */
    0xf0,

    /* U+0028 "(" */
    0xf, 0xf8, 0x3, 0x7f, 0x80,

    /* U+0029 ")" */
    0xf0, 0xff, 0x0, 0x6f, 0xf0,

    /* U+002A "*" */
    0x0, 0x78, 0x3, 0xc0, 0x2f, 0xf8, 0x3f, 0xf0,
    0x7f, 0xc0, 0x17, 0x80,

    /* U+002B "+" */
    0x0, 0x78, 0x7, 0xdf, 0xe0, 0xff, 0xc1, 0xfe,
    0x0, 0xe0,

    /* U+002C "," */
    0xf, 0xf8,

    /* U+002D "-" */
    0xff, 0x80,

    /* U+002E "." */
    0xf0,

    /* U+002F "/" */
    0x0, 0x78, 0x6, 0xff, 0xc0, 0x18,

    /* U+0030 "0" */
    0xf, 0xf0, 0x7f, 0xe0, 0xf, 0xdf, 0xf8,

    /* U+0031 "1" */
    0xff, 0x7, 0x80, 0x7e, 0xf0, 0xf0,

    /* U+0032 "2" */
    0xff, 0x83, 0xff, 0x7, 0xff, 0x80, 0x1f, 0xf0,

    /* U+0033 "3" */
    0xff, 0x83, 0xff, 0x7, 0xfc, 0x1f, 0xff, 0x0,

    /* U+0034 "4" */
    0xf0, 0xf0, 0xf, 0x78, 0x7f, 0xc1, 0xe0, 0x18,

    /* U+0035 "5" */
    0xff, 0xc1, 0xff, 0x7, 0xf8, 0x3f, 0xff, 0x0,

    /* U+0036 "6" */
    0xf, 0xf0, 0x7f, 0xc0, 0xe, 0xe0, 0x3, 0x31,
    0xfe, 0xdd, 0x78,

    /* U+0037 "7" */
    0xff, 0xf8, 0x2, 0xff, 0x7, 0xf8, 0x3, 0x80,

    /* U+0038 "8" */
    0xf, 0xf0, 0x7f, 0xff, 0x66, 0x3f, 0xd9, 0x8f,

    /* U+0039 "9" */
    0xf, 0xf0, 0x7f, 0xf7, 0x70, 0x1, 0xfe, 0x0,
    0x77, 0x3c,

    /* U+003A ":" */
    0xff, 0x80,

    /* U+003B ";" */
    0xf, 0xf, 0xf, 0xf8,

    /* U+003C "<" */
    0x0, 0x78, 0x7f, 0xe0, 0xff, 0x0, 0x3f, 0xc0,

    /* U+003D "=" */
    0xff, 0xfe,

    /* U+003E ">" */
    0xf0, 0x7, 0xf8, 0x1, 0xfe, 0xf, 0xfc, 0x0,

    /* U+003F "?" */
    0xff, 0x83, 0xff, 0x7, 0xfc, 0x1f, 0xe0, 0x7,
    0x80, 0x0,

    /* U+0040 "@" */
    0xf, 0xf8, 0x3f, 0xc1, 0xfe, 0x0, 0xbf, 0xc0,
    0xf, 0xf0, 0x7f, 0xe0,

    /* U+0041 "A" */
    0xf, 0xf0, 0x7f, 0xe0, 0xff, 0x0, 0x3f, 0xc0,
    0x1c,

    /* U+0042 "B" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0x80, 0x1f, 0xe0,
    0x7, 0xfc,

    /* U+0043 "C" */
    0xf, 0xf0, 0x7f, 0xe0, 0xb, 0xc0, 0x2f, 0xfe,

    /* U+0044 "D" */
    0xff, 0x80, 0x1f, 0xf0, 0x7, 0xf7, 0xfc,

    /* U+0045 "E" */
    0xff, 0x83, 0xfc, 0x1f, 0xe0, 0xff, 0x7, 0xf8,

    /* U+0046 "F" */
    0xff, 0x83, 0xfc, 0x1f, 0xe0, 0xff, 0x0, 0x40,

    /* U+0047 "G" */
    0xf, 0xff, 0x80, 0x1f, 0xe0, 0x7, 0x87, 0xfe,

    /* U+0048 "H" */
    0xf0, 0x7, 0x80, 0x77, 0xf8, 0x1, 0xfe, 0x0,
    0xe0,

    /* U+0049 "I" */
    0xf0, 0xc,

    /* U+004A "J" */
    0x0, 0xbc, 0x3, 0xf7, 0x80, 0x5f, 0xf8,

    /* U+004B "K" */
    0xf0, 0x7, 0x80, 0x3f, 0xc1, 0xfe, 0x0, 0x7f,
    0x80, 0x2f, 0xf0,

    /* U+004C "L" */
    0xf0, 0xf, 0xfe, 0x7, 0xf8,

    /* U+004D "M" */
    0xf0, 0xb, 0xc3, 0xc3, 0xc0, 0x1f, 0xf0, 0x5,
    0xe0, 0x1f, 0x0,

    /* U+004E "N" */
    0xf0, 0xb, 0xc3, 0xc0, 0x37, 0xf8, 0x3, 0x7f,
    0x80, 0x37, 0x80,

    /* U+004F "O" */
    0xf, 0xf0, 0x7f, 0xe0, 0xf, 0xdf, 0xf8,

    /* U+0050 "P" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0xc1, 0xfe, 0x0,
    0xe0,

    /* U+0051 "Q" */
    0xf, 0xf0, 0x7f, 0xe0, 0xf, 0xdf, 0xf8, 0x3f,
    0xe0,

    /* U+0052 "R" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0xc1, 0xe0, 0x1b,
    0xfc,

    /* U+0053 "S" */
    0xf, 0xff, 0xf0, 0x3, 0xff, 0xe0,

    /* U+0054 "T" */
    0xff, 0xc1, 0xe0, 0x1f, 0xc0,

    /* U+0055 "U" */
    0xf0, 0x7, 0x80, 0x7f, 0xf0, 0x3f, 0xf0,

    /* U+0056 "V" */
    0xf0, 0xb, 0xc0, 0x3b, 0xfc, 0x1f, 0xe0, 0xf,
    0x7f, 0xc0,

    /* U+0057 "W" */
    0xf0, 0xb, 0xc0, 0x1e, 0x1, 0xff, 0xc0, 0xff,
    0xe0,

    /* U+0058 "X" */
    0xf0, 0xb, 0xfe, 0xf, 0xf0, 0x7f, 0xc0, 0xf,
    0xf8, 0x3f, 0xc1, 0xfe,

    /* U+0059 "Y" */
    0xf0, 0xb, 0xfe, 0xf, 0xf0, 0x7f, 0xc0, 0x1f,
    0xf0,

    /* U+005A "Z" */
    0xff, 0xe0, 0x7, 0xfe, 0x0, 0x7f, 0x80,

    /* U+005B "[" */
    0xff, 0x7, 0x80, 0x77, 0x80,

    /* U+005C "\\" */
    0xf0, 0xe, 0xff, 0x0, 0x3f, 0xc0, 0x10,

    /* U+005D "]" */
    0xff, 0x80, 0x3b, 0xc0,

    /* U+005E "^" */
    0xf, 0xf, 0xf8,

    /* U+005F "_" */
    0xff, 0xc0,

    /* U+0060 "`" */
    0xf0, 0xff, 0x0,

    /* U+0061 "a" */
    0xf, 0xf0, 0x7f, 0xe0, 0xff, 0x0, 0x3f, 0xc0,
    0x1c,

    /* U+0062 "b" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0x80, 0x1f, 0xe0,
    0x7, 0xfc,

    /* U+0063 "c" */
    0xf, 0xf0, 0x7f, 0xe0, 0xb, 0xc0, 0x2f, 0xfe,

    /* U+0064 "d" */
    0xff, 0x80, 0x1f, 0xf0, 0x7, 0xf7, 0xfc,

    /* U+0065 "e" */
    0xff, 0x83, 0xfc, 0x1f, 0xe0, 0xff, 0x7, 0xf8,

    /* U+0066 "f" */
    0xff, 0x83, 0xfc, 0x1f, 0xe0, 0xff, 0x0, 0x40,

    /* U+0067 "g" */
    0xf, 0xff, 0x80, 0x1f, 0xe0, 0x7, 0x87, 0xfe,

    /* U+0068 "h" */
    0xf0, 0x7, 0x80, 0x77, 0xf8, 0x1, 0xfe, 0x0,
    0xe0,

    /* U+0069 "i" */
    0xf0, 0xc,

    /* U+006A "j" */
    0x0, 0xbc, 0x3, 0xf7, 0x80, 0x5f, 0xf8,

    /* U+006B "k" */
    0xf0, 0x7, 0x80, 0x3f, 0xc1, 0xfe, 0x0, 0x7f,
    0x80, 0x2f, 0xf0,

    /* U+006C "l" */
    0xf0, 0xf, 0xfe, 0x7, 0xf8,

    /* U+006D "m" */
    0xf0, 0xb, 0xc3, 0xc3, 0xc0, 0x1f, 0xf0, 0x5,
    0xe0, 0x1f, 0x0,

    /* U+006E "n" */
    0xf0, 0xb, 0xc3, 0xc0, 0x37, 0xf8, 0x3, 0x7f,
    0x80, 0x37, 0x80,

    /* U+006F "o" */
    0xf, 0xf0, 0x7f, 0xe0, 0xf, 0xdf, 0xf8,

    /* U+0070 "p" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0xc1, 0xfe, 0x0,
    0xe0,

    /* U+0071 "q" */
    0xf, 0xf0, 0x7f, 0xe0, 0xf, 0xdf, 0xf8, 0x3f,
    0xe0,

    /* U+0072 "r" */
    0xff, 0x80, 0x1f, 0xf0, 0x7f, 0xc1, 0xe0, 0x1b,
    0xfc,

    /* U+0073 "s" */
    0xf, 0xff, 0xf0, 0x3, 0xff, 0xe0,

    /* U+0074 "t" */
    0xff, 0xc1, 0xe0, 0x1f, 0xc0,

    /* U+0075 "u" */
    0xf0, 0x7, 0x80, 0x7f, 0xf0, 0x3f, 0xf0,

    /* U+0076 "v" */
    0xf0, 0xb, 0xc0, 0x3b, 0xfc, 0x1f, 0xe0, 0xf,
    0x7f, 0xc0,

    /* U+0077 "w" */
    0xf0, 0xb, 0xc0, 0x1e, 0x1, 0xff, 0xc0, 0xff,
    0xe0,

    /* U+0078 "x" */
    0xf0, 0xb, 0xfe, 0xf, 0xf0, 0x7f, 0xc0, 0xf,
    0xf8, 0x3f, 0xc1, 0xfe,

    /* U+0079 "y" */
    0xf0, 0xb, 0xfe, 0xf, 0xf0, 0x7f, 0xc0, 0x1f,
    0xf0,

    /* U+007A "z" */
    0xff, 0xe0, 0x7, 0xfe, 0x0, 0x7f, 0x80,

    /* U+007B "{" */
    0xf, 0xf0, 0x3, 0xfe, 0xf, 0xf0, 0x5, 0xe0,

    /* U+007C "|" */
    0xf0, 0xf, 0x0,

    /* U+007D "}" */
    0xff, 0x7, 0x80, 0x5f, 0xe0, 0xff, 0x80, 0x0,

    /* U+007E "~" */
    0xf, 0xf, 0xfe
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 64, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 48, .box_w = 1, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 80, .box_w = 3, .box_h = 2, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 6, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 19, .adv_w = 96, .box_w = 4, .box_h = 7, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 30, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 40, .adv_w = 96, .box_w = 4, .box_h = 7, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 52, .adv_w = 48, .box_w = 1, .box_h = 2, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 53, .adv_w = 64, .box_w = 2, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 58, .adv_w = 64, .box_w = 2, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 63, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 75, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 85, .adv_w = 64, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 87, .adv_w = 80, .box_w = 3, .box_h = 1, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 89, .adv_w = 48, .box_w = 1, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 90, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 96, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 103, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 109, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 117, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 125, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 133, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 141, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 152, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 160, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 168, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 178, .adv_w = 48, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 180, .adv_w = 64, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 184, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 192, .adv_w = 80, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 194, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 202, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 212, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 224, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 233, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 243, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 251, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 258, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 266, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 274, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 282, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 291, .adv_w = 48, .box_w = 1, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 300, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 311, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 316, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 327, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 338, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 345, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 354, .adv_w = 96, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 363, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 372, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 378, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 383, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 390, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 400, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 409, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 421, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 430, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 437, .adv_w = 64, .box_w = 2, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 442, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 449, .adv_w = 64, .box_w = 2, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 453, .adv_w = 80, .box_w = 3, .box_h = 2, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 456, .adv_w = 96, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 458, .adv_w = 64, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 461, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 470, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 480, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 488, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 495, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 503, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 511, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 519, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 528, .adv_w = 48, .box_w = 1, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 530, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 537, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 548, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 553, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 564, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 575, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 582, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 591, .adv_w = 96, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 600, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 609, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 615, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 620, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 627, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 637, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 646, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 658, .adv_w = 112, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 667, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 674, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 682, .adv_w = 48, .box_w = 1, .box_h = 7, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 685, .adv_w = 80, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 693, .adv_w = 96, .box_w = 4, .box_h = 2, .ofs_x = 1, .ofs_y = 3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t cyber_font_sm = {
#else
lv_font_t cyber_font_sm = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 9,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if CYBER_FONT_SM*/

