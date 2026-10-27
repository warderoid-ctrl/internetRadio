#pragma once
// Shared look, helpers and cross-thread plumbing. (Everything is one translation unit: .ino includes the modules.)

// ───────────── fonts: Inter / Inter Tight + Lucide icons (fonts/, made by tools/gen_tonal_fonts.py) ─────────────
// Text fonts fall back to Montserrat, so LV_SYMBOL_* still draw where LVGL uses them itself (keyboard).
#include "tonal_fonts.h"
#define F_CLOCK  (&tonal_clock_128)   // Inter Tight SemiBold 128: home clock (digits, ':' and '-' only)
#define F_NIGHT  (&tonal_night_236)   // Inter Tight Light 236: night clock (digits, ':' and '-' only)
#define F_HERO   (&tonal_hero_112)    // Inter Tight SemiBold 112 / 72: temperatures (digits, '°', '-' only)
#define F_HERO_S (&tonal_hero_72)
#define F_T34    (&tonal_t34)         // Inter Tight SemiBold 34 / Bold 26 / Bold 22: titles
#define F_T26    (&tonal_t26)
#define F_T22    (&tonal_t22)
#define F_L      (&tonal_l20)         // Inter SemiBold 20: card titles
#define F_M      (&tonal_m16)         // Inter SemiBold 16: buttons, list rows
#define F_S      (&tonal_s14)         // Inter Medium 14 / 12: supporting text
#define F_XS     (&tonal_xs12)
#define I_20     (&tonal_i20)         // Lucide icons at 20 / 24 / 28 px; text in the same label falls back to Inter
#define I_24     (&tonal_i24)
#define I_28     (&tonal_i28)
#define I_56     (&tonal_i56)         // weather icons only

// ───────────── palette: Tonal theme roles (tonal_theme.h), dark or light, chosen at boot ─────────────
// The old names map onto roles while the screens are migrated; new code should use T->role directly.
#include "tonal_theme.h"
#define C_BG      (T->bg)
#define C_CARD    (T->surface)          // cards
#define C_CARD2   (T->surface)          // list rows, text fields (mostly sit on bg)
#define C_CARD3   (T->selected)         // secondary buttons, highlights, slider tracks: visible on bg and surface
#define C_TEXT    (T->text)
#define C_MUTED   (T->muted)
#define C_ACCENT  (T->primary)
#define C_ACCENT2 (T->primary)          // was the gradient end: Tonal has no gradients, so fills are flat
#define C_GOOD    (T->good)
#define C_WARN    (T->warn)
#define C_BAD     (T->bad)
#define C_BLUE    (T->secondary)
// text / icon colour to put on a fill: a role's "on" colour, otherwise the normal text colour
static uint32_t onFill(uint32_t bg) {
  if (bg == T->primary) return T->onPrimary;
  if (bg == T->secondary) return T->onSecondary;
  if (bg == T->tertiary) return T->onTertiary;
  if (bg == T->talk) return T->onTalk;
  return T->text;
}

#define SCREEN_W 800
#define SCREEN_H 480
// Pages are full screen. Home has the bottom nav bar; Podcasts and Weather lay out their own back button; the
// others get a header (back button + title, pageBody()) and put their content in the PAGE_* box under it.
#define PAGE_X   T_PAD_X
#define PAGE_Y   76
#define PAGE_W   (SCREEN_W - 2 * T_PAD_X)
#define PAGE_H   (SCREEN_H - PAGE_Y - T_PAD_BOTTOM)

// ───────────── board control (helper MCU on I2C 0x30) ─────────────
#define STC8_ADDR     0x30
#define STC_TOUCH_ON  250
#define STC_BL_MAX    0       // 0 = brightest ... 244 dimmest, 245 off
#define STC_SPK_ON    248
#define STC_SPK_OFF   249
#define STC_MIC_PATH  2
#define STC_AUDIO_DEF 3
#define STC_BUZZ_ON   246
#define STC_BUZZ_OFF  247

static SemaphoreHandle_t i2cMx;    // Wire is shared by touch + helper MCU + (other tasks): take this around it

static void stc(uint8_t cmd) {
  if (i2cMx) xSemaphoreTake(i2cMx, portMAX_DELAY);
  Wire.beginTransmission(STC8_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
  if (i2cMx) xSemaphoreGive(i2cMx);
}

// copy the text the Tonal fonts can draw: ASCII, Latin-1 and common punctuation (curly quotes, dashes, bullet,
// ellipsis, euro, minus). Anything else (emoji, other scripts, broken UTF-8) is dropped; a cut never splits a character.
static bool fontHas(uint32_t cp) {
  return (cp >= 0xA0 && cp <= 0xFF) || cp == 0x2013 || cp == 0x2014 || cp == 0x2018 || cp == 0x2019 ||
         cp == 0x201C || cp == 0x201D || cp == 0x2022 || cp == 0x2026 || cp == 0x20AC || cp == 0x2212;
}
static void copyText(char *dst, size_t n, const char *src) {
  size_t o = 0;
  for (size_t i = 0; src[i];) {
    uint8_t c = (uint8_t)src[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    bool keep = (len == 1) && c >= 32 && c < 127;
    if (len > 1) {
      uint32_t cp = c & (0x7F >> len);
      for (int k = 1; k < len; k++) {
        uint8_t d = (uint8_t)src[i + k];
        if ((d & 0xC0) != 0x80) { len = k; cp = 0; break; }   // truncated sequence (also stops at the end)
        cp = (cp << 6) | (d & 0x3F);
      }
      keep = fontHas(cp);
    }
    if (keep) {
      if (o + len > n - 1) break;
      memcpy(dst + o, src + i, len);
      o += len;
    }
    i += len;
  }
  dst[o] = 0;
}

// ───────────── tiny helpers for building widgets ─────────────
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *clearBox(lv_obj_t *parent, int x, int y, int w, int h) {   // transparent container
  lv_obj_t *o = panel(parent, x, y, w, h, C_BG, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  return o;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t col) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  return l;
}

// fill with a container's "on" colour at ~10 %: chips and round buttons sitting inside a coloured card
static void tint(lv_obj_t *o, uint32_t onCol) {
  lv_obj_set_style_bg_color(o, lv_color_hex(onCol), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_10, 0);
}

// pressed: a little rounder (pills stay round) and slightly see-through, eased over 120 ms
static void pressStyle(lv_obj_t *b, int radius) {
  static const lv_style_prop_t props[] = {LV_STYLE_RADIUS, LV_STYLE_BG_OPA, LV_STYLE_PROP_INV};
  static lv_style_transition_dsc_t tr;
  static bool init = false;
  if (!init) { lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, nullptr); init = true; }
  lv_obj_set_style_radius(b, radius, 0);
  lv_obj_set_style_radius(b, radius == LV_RADIUS_CIRCLE ? radius : radius + 10, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_style_transition(b, &tr, 0);
  lv_obj_set_style_transition(b, &tr, LV_STATE_PRESSED);
}

// a button with one centred label. The main action (primary fill) is squarer, everything else is a pill.
// Icons: pass a LUCIDE_* string with an icon font (I_20 / I_24 / I_28); text after the icon falls back to Inter.
static lv_obj_t *textBtn(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                         const lv_font_t *f, uint32_t bg, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  pressStyle(b, bg == T->primary ? T_R_ACTION : LV_RADIUS_CIRCLE);
  lv_obj_t *l = mkLabel(b, txt, f, onFill(bg));
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}
static lv_obj_t *btnLabel(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }
// recolour a textBtn (selected tab etc.): fill + matching label colour
static void btnFill(lv_obj_t *btn, uint32_t bg) {
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
  lv_obj_set_style_text_color(btnLabel(btn), lv_color_hex(onFill(bg)), 0);
}
// round icon button (back, prev / next...): surfaceHi, or tinted with onCol when it sits inside a coloured card
static lv_obj_t *roundBtn(lv_obj_t *parent, int x, int y, int d, const char *icon, lv_event_cb_t cb,
                          uint32_t onCol = 0, void *ud = nullptr) {
  lv_obj_t *b = textBtn(parent, x, y, d, d, icon, d >= 56 ? I_28 : I_24, T->surfaceHi, cb, ud);
  if (onCol) { tint(b, onCol); lv_obj_set_style_text_color(btnLabel(b), lv_color_hex(onCol), 0); }
  return b;
}

// chip: 44 px pill sized to its text. Selected = secondary fill, otherwise a 1 px outline.
static void chipSel(lv_obj_t *c, bool sel) {
  lv_obj_set_style_bg_color(c, lv_color_hex(T->secondary), 0);
  lv_obj_set_style_bg_opa(c, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, sel ? 0 : 1, 0);
  lv_obj_set_style_text_color(btnLabel(c), lv_color_hex(sel ? T->onSecondary : T->text), 0);
}
static lv_obj_t *chip(lv_obj_t *parent, const char *txt, bool sel, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *c = lv_button_create(parent);
  lv_obj_set_size(c, LV_SIZE_CONTENT, 44);
  lv_obj_set_style_pad_hor(c, 16, 0);
  lv_obj_set_style_pad_ver(c, 0, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(T->outline), 0);
  pressStyle(c, LV_RADIUS_CIRCLE);
  lv_obj_t *l = mkLabel(c, txt, F_S, T->text);
  lv_obj_set_style_text_font(l, txt[0] == '\xEE' ? I_20 : F_S, 0);    // a leading Lucide icon needs an icon font
  lv_obj_center(l);
  chipSel(c, sel);
  if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
  return c;
}
// a row of chips (flex, wraps nothing: keep them few)
static lv_obj_t *chipRow(lv_obj_t *parent, int x, int y, int w) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_size(r, w, 44);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(r, 6, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}
// keep a label on one line: LONG_DOT / scrolling only work once the height is fixed (else LVGL wraps the text)
static void oneLine(lv_obj_t *l, int w, bool scroll) {
  lv_obj_set_width(l, w);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
  lv_label_set_long_mode(l, scroll ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) { return panel(parent, x, y, w, h, C_CARD, T_R_TILE); }
// a big filled container (clock, weather...): role colour, the 36 px Tonal radius
static lv_obj_t *container(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col) { return panel(parent, x, y, w, h, col, T_R_CARD); }

// flex column that scrolls (lists)
static lv_obj_t *scrollCol(lv_obj_t *parent, int x, int y, int w, int h, int gap) {
  lv_obj_t *l = lv_obj_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_size(l, w, h);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(l, 0, 0);
  lv_obj_set_style_pad_all(l, 0, 0);
  lv_obj_set_style_pad_row(l, gap, 0);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
  return l;
}

// ───────────── shared widgets for the search / list screens ─────────────
// one-line text field; onTap usually brings the keyboard up
static lv_obj_t *textField(lv_obj_t *parent, int x, int y, int w, int h, const char *placeholder, lv_event_cb_t onTap) {
  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, x, y);
  lv_obj_set_size(ta, w, h);
  lv_textarea_set_one_line(ta, true);
  if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_style_text_font(ta, F_M, 0);
  lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_radius(ta, T_R_ROW, 0);
  lv_obj_set_style_pad_hor(ta, 18, 0);
  if (onTap) lv_obj_add_event_cb(ta, onTap, LV_EVENT_CLICKED, nullptr);
  return ta;
}
// on-screen keyboard docked at the bottom of parent, typing into ta; cb gets READY (Enter) / CANCEL
static lv_obj_t *keyboardFor(lv_obj_t *parent, int w, int h, lv_obj_t *ta, lv_event_cb_t cb) {
  lv_obj_t *kb = lv_keyboard_create(parent);
  lv_obj_set_size(kb, w, h);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(kb, F_M, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(T->surface), 0);
  lv_obj_set_style_radius(kb, T_R_TILE, 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 8, 0);
  lv_obj_set_style_pad_gap(kb, 6, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(T->surfaceHi), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(T->selected), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(kb, lv_color_hex(T->selected), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(kb, lv_color_hex(T->text), LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 14, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, cb, LV_EVENT_ALL, nullptr);
  return kb;
}
// the keyboard and the results list share the lower screen: show one, hide the other
static void kbToggle(lv_obj_t *kb, lv_obj_t *list, bool showKb) {
  if (showKb) { lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN); }
}
// a tappable row in a scrollCol list
static lv_obj_t *listRow(lv_obj_t *list, int w, int h) {
  lv_obj_t *row = lv_button_create(list);
  lv_obj_set_size(row, w, h);
  lv_obj_set_style_pad_all(row, 4, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(T->selected), LV_STATE_PRESSED);
  lv_obj_set_style_radius(row, T_R_ROW, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  return row;
}
// title (scrolls when too long) over a detail line (ends in "..."), starting at x
static void rowText(lv_obj_t *row, int x, int w, const char *title, const char *detail) {
  lv_obj_t *n = mkLabel(row, title, F_M, C_TEXT);
  oneLine(n, w, true);
  lv_obj_set_pos(n, x, 6);
  lv_obj_t *d = mkLabel(row, detail, F_XS, C_MUTED);
  oneLine(d, w, false);
  lv_obj_set_pos(d, x, 36);
}
// one full-screen page (Wi-Fi, Find stations, weather location...) with a Back button
static lv_obj_t *subScreen(lv_event_cb_t onBack, const char *title) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  if (onBack) roundBtn(scr, T_PAD_X, 14, 48, LUCIDE_CHEVRON_LEFT, onBack);
  if (title) { lv_obj_t *t = mkLabel(scr, title, F_T26, C_TEXT); lv_obj_set_pos(t, onBack ? 88 : T_PAD_X, 22); }
  return scr;
}

static lv_obj_t *bar(lv_obj_t *p, int x, int y, int w, int h, uint32_t col) {
  return panel(p, x, y, w, h, col, h < w ? h / 2 : w / 2);
}

// ───────────── weather icons: Lucide glyphs, coloured like text ─────────────
enum WxKind : uint8_t { WX_CLEAR, WX_PARTLY, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM };

static const char *wxGlyph(uint8_t kind, bool night) {
  switch (kind) {
    case WX_CLEAR:   return night ? LUCIDE_MOON : LUCIDE_SUN;
    case WX_PARTLY:  return night ? LUCIDE_CLOUD_MOON : LUCIDE_CLOUD_SUN;
    case WX_FOG:     return LUCIDE_CLOUD_FOG;
    case WX_DRIZZLE: return LUCIDE_CLOUD_DRIZZLE;
    case WX_RAIN:    return LUCIDE_CLOUD_RAIN;
    case WX_SNOW:    return LUCIDE_CLOUD_SNOW;
    case WX_STORM:   return LUCIDE_CLOUD_LIGHTNING;
  }
  return LUCIDE_CLOUD;
}
static void wxDraw(lv_obj_t *icon, uint8_t kind, bool night) { lv_label_set_text(icon, wxGlyph(kind, night)); }
// a weather icon label of about size px (56 / 28 / 24 / 20), top-left at x, y
static lv_obj_t *wxIcon(lv_obj_t *parent, int x, int y, int size, uint8_t kind, bool night, uint32_t col = 0) {
  const lv_font_t *f = size >= 48 ? I_56 : size >= 28 ? I_28 : size >= 24 ? I_24 : I_20;
  lv_obj_t *l = mkLabel(parent, wxGlyph(kind, night), f, col ? col : T->text);
  lv_obj_set_pos(l, x, y);
  return l;
}

// ───────────── small time helpers ─────────────
static bool nowTm(struct tm *t) { return getLocalTime(t, 0); }
static const char *DOW3[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *DOWF[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *MON3[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *MONF[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                             "October", "November", "December"};

static bool isNight() {
  struct tm t;
  if (!nowTm(&t)) return false;
  int h = t.tm_hour;
  return (NIGHT_START_HOUR > NIGHT_END_HOUR) ? (h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR)
                                              : (h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR);
}

// ───────────── cross-thread: toasts and speech requests posted by worker tasks, shown by the UI thread ─────────────
struct UiMsg { uint8_t type; uint32_t color; char text[200]; };
enum { UM_TOAST = 1, UM_VOICE_STATE, UM_VOICE_RESULT };
static QueueHandle_t uiQ;
static void postUi(uint8_t type, const char *text, uint32_t color = C_TEXT) {
  if (!uiQ) return;
  UiMsg m; m.type = type; m.color = color;
  copyText(m.text, sizeof(m.text), text);
  xQueueSend(uiQ, &m, 0);
}

// shared data guard + "version" counters (worker bumps, UI notices and redraws)
static SemaphoreHandle_t dataMx;
static volatile uint32_t wxVer = 0, agVer = 0, haVer = 0;
#define DATA_LOCK()   xSemaphoreTake(dataMx, portMAX_DELAY)
#define DATA_UNLOCK() xSemaphoreGive(dataMx)

// ───────────── navigation ─────────────
enum Page : uint8_t { PG_HOME, PG_RADIO, PG_PODCASTS, PG_WEATHER, PG_AGENDA, PG_HOUSE, PG_TIMERS, PG_MORE, N_PAGES };
static lv_obj_t *pages[N_PAGES];
static int curPage = PG_HOME;
static void showPage(int p);
static void beep(int count);                 // timers.h: short buzzer beeps (non-blocking)

// header for a page that has one (back to Home + title); returns the content box under it (PAGE_W x PAGE_H)
static void onPageBack(lv_event_t *) { showPage(PG_HOME); }
static lv_obj_t *pageBody(lv_obj_t *pg, const char *title) {
  roundBtn(pg, T_PAD_X, 14, 48, LUCIDE_CHEVRON_LEFT, onPageBack);
  lv_obj_t *t = mkLabel(pg, title, F_T26, C_TEXT);
  lv_obj_set_pos(t, 88, 22);
  lv_obj_t *body = lv_obj_create(pg);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, PAGE_X, PAGE_Y);
  lv_obj_set_size(body, PAGE_W, PAGE_H);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  return body;
}
