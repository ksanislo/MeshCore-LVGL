/**
 * LVGL 8.3.x config for CrowPanel Advance 3.5 MeshCore companion firmware.
 * Minimal, focused on ESP32-S3 + ILI9488 480x320 + GT911 touch.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH                  16
#define LV_COLOR_16_SWAP                0
#define LV_COLOR_SCREEN_TRANSP          0
#define LV_COLOR_MIX_ROUND_OFS          (LV_COLOR_DEPTH == 32 ? 0 : 128)
#define LV_COLOR_CHROMA_KEY             lv_color_hex(0x00ff00)

// Keep LVGL's own O(1) TLSF allocator, but back its pool with one big PSRAM
// block grabbed at init (LV_MEM_POOL_ALLOC). This keeps internal SRAM free
// for DMA framebuffers WITHOUT routing LVGL's per-render allocation churn
// through the ESP-IDF heap -- doing the latter (LV_MEM_CUSTOM=1 + ps_malloc)
// fragmented the PSRAM heap and caused multi-hundred-ms render stalls.
#define LV_MEM_CUSTOM                   0
#define LV_MEM_SIZE                     (256U * 1024U)
#define LV_MEM_ADR                      0  // 0 => pool from LV_MEM_POOL_ALLOC
#define LV_MEM_POOL_INCLUDE             "lv_mem_psram.h"
#define LV_MEM_POOL_ALLOC               ps_malloc
#define LV_MEM_BUF_MAX_NUM              16
#define LV_MEMCPY_MEMSET_STD            1

#define LV_DISP_DEF_REFR_PERIOD         16
#define LV_INDEV_DEF_READ_PERIOD        16
#define LV_TICK_CUSTOM                  0
#define LV_DPI_DEF                      130

#define LV_DRAW_COMPLEX                 1
#define LV_SHADOW_CACHE_SIZE            0
#define LV_CIRCLE_CACHE_SIZE            4
#define LV_LAYER_SIMPLE_BUF_SIZE        (24 * 1024)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (3 * 1024)
// Cache decoded images so scrolling emoji aren't re-read from SD every frame.
// Our emoji decoder preloads each glyph to RAM; the cache keeps ~this many live.
#define LV_IMG_CACHE_DEF_SIZE           48
#define LV_GRADIENT_MAX_STOPS           2
#define LV_GRAD_CACHE_DEF_SIZE          0
#define LV_DITHER_GRADIENT              0
#define LV_DISP_ROT_MAX_BUF             (10*1024)
#define LV_USE_GPU_STM32_DMA2D          0
#define LV_USE_GPU_SWM341_DMA2D         0
#define LV_USE_GPU_NXP_PXP              0
#define LV_USE_GPU_NXP_VG_LITE          0
#define LV_USE_GPU_SDL                  0

#define LV_USE_LOG                      0
#define LV_USE_ASSERT_NULL              1
#define LV_USE_ASSERT_MALLOC            1
#define LV_USE_ASSERT_STYLE             0
#define LV_USE_ASSERT_MEM_INTEGRITY     0
#define LV_USE_ASSERT_OBJ               0
#define LV_ASSERT_HANDLER_INCLUDE       <stdint.h>
#define LV_ASSERT_HANDLER               while(1);

#define LV_USE_PERF_MONITOR             0
#define LV_USE_MEM_MONITOR              0
#define LV_USE_REFR_DEBUG               0
#define LV_SPRINTF_CUSTOM               0
#define LV_SPRINTF_USE_FLOAT            0
#define LV_USE_USER_DATA                1
#define LV_ENABLE_GC                    0

#define LV_BIG_ENDIAN_SYSTEM            0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE     1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value)  struct _silence_gcc_warning
#define LV_USE_LARGE_COORD              0

/* Fonts */
#define LV_FONT_MONTSERRAT_8            0
#define LV_FONT_MONTSERRAT_10           1
#define LV_FONT_MONTSERRAT_12           1
#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_MONTSERRAT_16           1
#define LV_FONT_MONTSERRAT_18           1
#define LV_FONT_MONTSERRAT_20           1
#define LV_FONT_MONTSERRAT_22           0
#define LV_FONT_MONTSERRAT_24           1
#define LV_FONT_MONTSERRAT_26           0
#define LV_FONT_MONTSERRAT_28           1
#define LV_FONT_MONTSERRAT_30           0
#define LV_FONT_MONTSERRAT_32           0
#define LV_FONT_MONTSERRAT_34           0
#define LV_FONT_MONTSERRAT_36           0
#define LV_FONT_MONTSERRAT_38           0
#define LV_FONT_MONTSERRAT_40           0
#define LV_FONT_MONTSERRAT_42           0
#define LV_FONT_MONTSERRAT_44           0
#define LV_FONT_MONTSERRAT_46           0
#define LV_FONT_MONTSERRAT_48           0
#define LV_FONT_MONTSERRAT_12_SUBPX     0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK           0
#define LV_FONT_UNSCII_8                1
#define LV_FONT_UNSCII_16               0
#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT                 &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE           0
#define LV_USE_FONT_COMPRESSED          0
#define LV_USE_FONT_SUBPX               0
#define LV_FONT_SUBPX_BGR               0
#define LV_USE_FONT_PLACEHOLDER         1

/* Text */
#define LV_TXT_ENC                      LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS              " ,.;:"
/* '-' and '_' intentionally removed: a #channel-name / @user_name recolor span has
 * no spaces, so breaking mid-token at '-'/'_' would split the span and drop the
 * color on the wrapped line (and the font has no non-breaking-hyphen glyph to swap
 * in). Trade-off: a very long hyphenated run in plain text now wraps as a whole
 * (long-word break is off, LV_TXT_LINE_BREAK_LONG_LEN=0) rather than at a hyphen. */
#define LV_TXT_LINE_BREAK_LONG_LEN      0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD                "\x01"   /* SOH: a control char that never appears in chat text,
                                                   so a literal '#' (e.g. a #hashtag) can be recolored
                                                   like any other char. We generate all recolor markup
                                                   ourselves (addMessageText) and strip \x01 from input
                                                   in sanitizeForFont so a message can't inject colors. */
#define LV_USE_BIDI                     0
#define LV_USE_ARABIC_PERSIAN_CHARS     0

/* Widgets — enable the standard set */
#define LV_USE_ARC                      1
#define LV_USE_BAR                      1
#define LV_USE_BTN                      1
#define LV_USE_BTNMATRIX                1
#define LV_USE_CANVAS                   1
#define LV_USE_CHECKBOX                 1
#define LV_USE_DROPDOWN                 1
#define LV_USE_IMG                      1
#define LV_USE_LABEL                    1
#define LV_LABEL_TEXT_SELECTION         1
#define LV_LABEL_LONG_TXT_HINT          1
#define LV_USE_LINE                     1
#define LV_USE_ROLLER                   1
#define LV_ROLLER_INF_PAGES             7
#define LV_USE_SLIDER                   1
#define LV_USE_SWITCH                   1
#define LV_USE_TEXTAREA                 1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME   1500
#define LV_USE_TABLE                    1

/* Extra widgets */
#define LV_USE_ANIMIMG                  0
#define LV_USE_CALENDAR                 0
#define LV_USE_CHART                    0
#define LV_USE_COLORWHEEL               0
#define LV_USE_IMGBTN                   1
#define LV_USE_KEYBOARD                 1
#define LV_USE_LED                      1
#define LV_USE_LIST                     1
#define LV_USE_MENU                     1
#define LV_USE_METER                    0
#define LV_USE_MSGBOX                   1
#define LV_USE_SPAN                     1
#define LV_SPAN_SNIPPET_STACK_SIZE      64
#define LV_USE_SPINBOX                  0
#define LV_USE_SPINNER                  1
#define LV_USE_TABVIEW                  1
#define LV_USE_TILEVIEW                 1
#define LV_USE_WIN                      0

/* Themes */
#define LV_USE_THEME_DEFAULT            1
#define LV_THEME_DEFAULT_DARK           1
#define LV_THEME_DEFAULT_GROW           1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC              1
#define LV_USE_THEME_MONO               0

/* Layouts */
#define LV_USE_FLEX                     1
#define LV_USE_GRID                     1

/* OS / filesystem / image decoders — all off */
#define LV_USE_FS_STDIO                 0
#define LV_USE_FS_POSIX                 0
#define LV_USE_FS_WIN32                 0
#define LV_USE_FS_FATFS                 0
#define LV_USE_PNG                      0
#define LV_USE_BMP                      0
#define LV_USE_SJPG                     0
#define LV_USE_GIF                      0
#define LV_USE_QRCODE                   1
#define LV_USE_FREETYPE                 0
#define LV_USE_TINY_TTF                 0
#define LV_USE_RLOTTIE                  0
#define LV_USE_FFMPEG                   0
#define LV_USE_SNAPSHOT                 0
#define LV_USE_MONKEY                   0
#define LV_USE_GRIDNAV                  0
#define LV_USE_FRAGMENT                 0
#define LV_USE_IMGFONT                  1
#define LV_USE_MSG                      0
#define LV_USE_IME_PINYIN               0
#define LV_USE_FILE_EXPLORER            0

/* Examples / demos off */
#define LV_BUILD_EXAMPLES               0

#endif /* LV_CONF_H */
