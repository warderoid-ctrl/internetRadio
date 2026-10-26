#pragma once
// Shared look, helpers and cross-thread plumbing. (Everything is one translation unit: .ino includes the modules.)

extern "C" { extern const lv_font_t font_clock_xl; extern const lv_font_t font_num_lg; }

// ───────────── fonts (lv_conf.h in this folder enables them all; fallbacks keep it compiling) ─────────────
#if LV_FONT_MONTSERRAT_48
  #define F_XL (&lv_font_montserrat_48)
#else
  #define F_XL (&lv_font_montserrat_40)
#endif
#define F_L  (&lv_font_montserrat_28)
#define F_M  (&lv_font_montserrat_20)
#define F_S  (&lv_font_montserrat_16)
#define F_XS (&lv_font_montserrat_14)
#define F_CLK (&font_clock_xl)
#define F_NUM (&font_num_lg)

// ───────────── palette ─────────────
#define C_BG      0x0B0C10
#define C_CARD    0x15171F
#define C_CARD2   0x1E212C
#define C_CARD3   0x2A2E3D
#define C_TEXT    0xF2F3F7
#define C_MUTED   0x8B90A0
#define C_ACCENT  0xFF6B3D
#define C_ACCENT2 0xFF3D6E
#define C_GOOD    0x3DDC97
#define C_WARN    0xFFC145
#define C_BAD     0xFF5C5C
#define C_BLUE    0x5CB8FF

#define SCREEN_W 800
#define SCREEN_H 480
#define RAIL_W   88
#define HDR_H    64
#define PAGE_X   RAIL_W
#define PAGE_Y   HDR_H
#define PAGE_W   (SCREEN_W - RAIL_W)
#define PAGE_H   (SCREEN_H - HDR_H)

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

// copy only plain ASCII (built-in Montserrat has no accents / emoji); keeps degree sign out too
static void copyAscii(char *dst, size_t n, const char *src) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o < n - 1; i++) {
    uint8_t c = (uint8_t)src[i];
    if (c >= 32 && c < 127) dst[o++] = (char)c;
    else if (c == 0xC3 && src[i + 1]) {              // common accented latin -> plain letter
      static const char *m = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      uint8_t d = (uint8_t)src[i + 1];
      if (d >= 0x80 && d <= 0xBF) dst[o++] = m[d - 0x80];
      i++;
    }
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

static lv_obj_t *textBtn(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                         const lv_font_t *f, uint32_t bg, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_t *l = mkLabel(b, txt, f, C_TEXT);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}
static lv_obj_t *btnLabel(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }
// keep a label on one line: LONG_DOT / scrolling only work once the height is fixed (else LVGL wraps the text)
static void oneLine(lv_obj_t *l, int w, bool scroll) {
  lv_obj_set_width(l, w);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
  lv_label_set_long_mode(l, scroll ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) { return panel(parent, x, y, w, h, C_CARD, 24); }

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

// ───────────── weather icons drawn from shapes (no image assets) ─────────────
enum WxKind : uint8_t { WX_CLEAR, WX_PARTLY, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM };

static lv_obj_t *dot(lv_obj_t *p, int x, int y, int d, uint32_t col) {
  lv_obj_t *o = panel(p, x, y, d, d, col, LV_RADIUS_CIRCLE);
  return o;
}
static lv_obj_t *bar(lv_obj_t *p, int x, int y, int w, int h, uint32_t col) {
  return panel(p, x, y, w, h, col, h < w ? h / 2 : w / 2);
}

static void wxDrawCloud(lv_obj_t *c, int S, int ox, int oy, float k, uint32_t col) {
  int s = (int)(S * k);
  dot(c, ox + s * 8 / 100,  oy + s * 42 / 100, s * 34 / 100, col);
  dot(c, ox + s * 26 / 100, oy + s * 22 / 100, s * 44 / 100, col);
  dot(c, ox + s * 52 / 100, oy + s * 34 / 100, s * 36 / 100, col);
  bar(c, ox + s * 18 / 100, oy + s * 52 / 100, s * 62 / 100, s * 24 / 100, col);
}

static void wxDraw(lv_obj_t *c, uint8_t kind, bool night) {
  lv_obj_clean(c);
  int S = lv_obj_get_width(c);
  uint32_t sunCol = night ? 0xE6EAF7 : 0xFFC145;
  uint32_t cloudCol = 0xC9CFDD, darkCloud = 0x8E97AD;
  auto sun = [&](int x, int y, int d) {
    lv_obj_t *s = dot(c, x, y, d, sunCol);
    lv_obj_set_style_shadow_width(s, d / 2, 0);
    lv_obj_set_style_shadow_color(s, lv_color_hex(sunCol), 0);
    lv_obj_set_style_shadow_opa(s, LV_OPA_40, 0);
  };
  switch (kind) {
    case WX_CLEAR: sun(S * 20 / 100, S * 20 / 100, S * 60 / 100); break;
    case WX_PARTLY: sun(S * 8 / 100, S * 6 / 100, S * 46 / 100); wxDrawCloud(c, S, S * 14 / 100, S * 26 / 100, 0.86f, cloudCol); break;
    case WX_CLOUD: wxDrawCloud(c, S, 0, S * 8 / 100, 1.0f, cloudCol); break;
    case WX_FOG:
      for (int i = 0; i < 4; i++) bar(c, S * (i % 2 ? 22 : 10) / 100, S * (22 + i * 17) / 100, S * 68 / 100, S * 8 / 100, cloudCol);
      break;
    case WX_DRIZZLE: case WX_RAIN: {
      wxDrawCloud(c, S, 0, 0, 1.0f, darkCloud);
      int n = kind == WX_RAIN ? 4 : 3;
      for (int i = 0; i < n; i++) bar(c, S * (24 + i * 17) / 100, S * (kind == WX_RAIN ? 70 : 74) / 100, S * 6 / 100 + 1, S * (kind == WX_RAIN ? 24 : 16) / 100, C_BLUE);
      break; }
    case WX_SNOW: {
      wxDrawCloud(c, S, 0, 0, 1.0f, cloudCol);
      for (int i = 0; i < 3; i++) dot(c, S * (24 + i * 22) / 100, S * (72 + (i % 2) * 10) / 100, S * 10 / 100 + 1, 0xFFFFFF);
      break; }
    case WX_STORM: {
      wxDrawCloud(c, S, 0, 0, 1.0f, 0x6E7791);
      lv_obj_t *b = mkLabel(c, LV_SYMBOL_CHARGE, S >= 60 ? F_L : F_S, C_WARN);
      lv_obj_set_pos(b, S * 38 / 100, S * 56 / 100);
      break; }
  }
}

static lv_obj_t *wxIcon(lv_obj_t *parent, int x, int y, int size, uint8_t kind, bool night) {
  lv_obj_t *c = clearBox(parent, x, y, size, size);
  wxDraw(c, kind, night);
  return c;
}

// a little drawn microphone
static void drawMic(lv_obj_t *parent, int cx, int cy, int s, uint32_t col) {
  lv_obj_t *body = bar(parent, cx - s * 18 / 100, cy - s * 40 / 100, s * 36 / 100, s * 56 / 100, col);
  (void)body;
  lv_obj_t *u = panel(parent, cx - s * 32 / 100, cy - s * 22 / 100, s * 64 / 100, s * 46 / 100, C_BG, s * 30 / 100);
  lv_obj_set_style_bg_opa(u, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(u, s / 11 + 1, 0);
  lv_obj_set_style_border_color(u, lv_color_hex(col), 0);
  lv_obj_set_style_border_side(u, LV_BORDER_SIDE_BOTTOM, 0);
  bar(parent, cx - s * 4 / 100 - 1, cy + s * 24 / 100, s * 9 / 100 + 1, s * 14 / 100, col);
  bar(parent, cx - s * 16 / 100, cy + s * 36 / 100, s * 32 / 100, s * 8 / 100 + 1, col);
}

// ───────────── small time helpers ─────────────
static bool nowTm(struct tm *t) { return getLocalTime(t, 0); }
static const char *DOW3[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *DOWF[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *MON3[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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
  copyAscii(m.text, sizeof(m.text), text);
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
static lv_obj_t *railBtn[N_PAGES];
static lv_obj_t *lblPageTitle;
static int curPage = PG_HOME;
static void showPage(int p);
static void beep(int count);                 // timers.h: short buzzer beeps (non-blocking)
