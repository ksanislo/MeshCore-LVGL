/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --font fonts/fa-solid-900.ttf -r 0xF002 --size 16 --bpp 4 --format lvgl --lv-include lvgl.h --no-compress --force-fast-kern-format -o ../examples/companion_radio_lvgl/ui-lvgl/meshcore_icons_16.cpp
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef MESHCORE_ICONS_16
#define MESHCORE_ICONS_16 1
#endif

#if MESHCORE_ICONS_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F002 "" */
    0x0, 0x3, 0xae, 0xfe, 0xa3, 0x0, 0x0, 0x0,
    0x0, 0x9f, 0xff, 0xff, 0xff, 0x90, 0x0, 0x0,
    0x9, 0xff, 0x82, 0x2, 0x8f, 0xf9, 0x0, 0x0,
    0x3f, 0xf3, 0x0, 0x0, 0x3, 0xff, 0x30, 0x0,
    0xaf, 0x80, 0x0, 0x0, 0x0, 0x8f, 0xa0, 0x0,
    0xef, 0x20, 0x0, 0x0, 0x0, 0x2f, 0xe0, 0x0,
    0xff, 0x0, 0x0, 0x0, 0x0, 0xf, 0xf0, 0x0,
    0xef, 0x20, 0x0, 0x0, 0x0, 0x2f, 0xe0, 0x0,
    0xaf, 0x80, 0x0, 0x0, 0x0, 0x8f, 0xa0, 0x0,
    0x3f, 0xf3, 0x0, 0x0, 0x3, 0xff, 0x40, 0x0,
    0x9, 0xff, 0x82, 0x2, 0x8f, 0xfe, 0x10, 0x0,
    0x0, 0x9f, 0xff, 0xff, 0xff, 0xef, 0xd1, 0x0,
    0x0, 0x3, 0xae, 0xfe, 0xa4, 0x1d, 0xfd, 0x10,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xdf, 0xd1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1d, 0xfc,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xcc
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 256, .box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61442, .range_length = 1, .glyph_id_start = 1,
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
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
extern const lv_font_t meshcore_icons_16 = {   // 'extern' -> external linkage under C++
#else
lv_font_t meshcore_icons_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if MESHCORE_ICONS_16*/

