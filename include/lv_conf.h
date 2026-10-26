/*
  lv_conf.h for CROW HUB (LVGL 9.1).
  INSTALL: back up Arduino/libraries/lv_conf.h (Elecrow's), then copy THIS file over it
  (it must sit next to the "lvgl" folder, not inside the sketch).
  Same as Elecrow's, plus: more font sizes, QR-code widget, and malloc-backed LVGL heap
  (so big screens don't run out of LVGL's small built-in 64 KB pool).
*/
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM   /* lv_mem_psram.c: LVGL objects live in PSRAM, internal RAM stays free for Wi-Fi */

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_USE_QRCODE 1

#endif
