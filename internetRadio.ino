/*
  CROW HUB - kitchen hub for the Elecrow CrowPanel Advance 7.0" (ESP32-S3, V1.3 / V1.4 / V1.5)

  Internet radio * weather * appointments * Home Assistant tiles * kitchen timers * voice assistant
  * photo frame / night clock * phone remote * LoRa survival mesh.

  Read README.md first (setup is ~15 minutes). Settings live in config.h.
*/

#include <Arduino.h>
#include <FS.h>            // file systems before LovyanGFX, or its drawJpgFile(fs, ...) overloads aren't built
#include <LittleFS.h>
#include <SD.h>
#include "pins_config.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>   // only for decoding photos into sprites; the panel itself is driven by esp_lcd below
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <esp_sntp.h>
#include <mbedtls/platform.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <algorithm>
#include <time.h>
#include <lvgl.h>
#include "Audio.h"

#include "config.h"


#include "ui_common.h"
#include "rtc.h"
#include "net_util.h"
#include "radio_app.h"
#include "podcasts.h"
#include "weather.h"
#include "agenda.h"
#include "house.h"
#include "timers.h"
#include "ambient.h"
#include "voice.h"
#include "lora.h"
#include "remote.h"

// ───────────── state ─────────────
static lv_display_t *disp;
static uint8_t *drawBuf1, *drawBuf2;
enum NetState : uint8_t { NET_NONE, NET_BOOT, NET_CONNECTING, NET_UP };
static NetState netState = NET_NONE;
static uint32_t netT0 = 0;
static bool scanning = false;
static String selSSID, selPass;
static String netSSID[16];
static bool netOpen[16];
static int netCount = 0;
static volatile bool ntpSynced = false;
static int bootMode = 0;                                   // 0 = hub, 1 = LoRa survival mesh
static int brightPct = 100;

static lv_obj_t *scrWifi;
static lv_obj_t *wifiListView, *wifiPwView, *wifiList, *wifiStatus, *wifiBackBtn, *wifiSsidLbl, *wifiTa, *wifiKb, *wifiPwStatus;
static lv_obj_t *hdrClock, *hdrTimer, *hdrWifi, *toastBox, *toastLbl, *modalOv, *modalTitle, *modalBody;
static uint32_t toastUntil = 0;
// home page
static lv_obj_t *homeClock, *homeDate, *homeWxIcon, *homeWxTemp, *homeWxCond, *homeWxHiLo, *homeNextT, *homeNextW, *homeStation, *homeStat, *homePlay;
// more page
static lv_obj_t *homeWxPlace, *moreLocLbl;
static lv_obj_t *moreWifiLbl, *moreSys, *morePhotos, *moreUrl, *moreQr, *sldBright;

static void showWifiScreen(bool allowBack);

// ───────────── LVGL <-> LovyanGFX glue ─────────────
static uint32_t lvTick() { return millis(); }
// 7" RGB panel via esp_lcd with bounce buffers: DMA streams the screen from two small internal-RAM buffers that an
// ISR refills from the PSRAM frame buffer, so PSRAM traffic elsewhere can no longer shift or tear the picture.
static esp_lcd_panel_handle_t lcdPanel;
static void panelInit() {
  esp_lcd_rgb_panel_config_t c = {};
  c.clk_src = LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz = 14000000;
  c.timings.h_res = SCREEN_W; c.timings.v_res = SCREEN_H;
  c.timings.hsync_pulse_width = 4; c.timings.hsync_back_porch = 8; c.timings.hsync_front_porch = 8;
  c.timings.vsync_pulse_width = 4; c.timings.vsync_back_porch = 8; c.timings.vsync_front_porch = 8;
  c.timings.flags.hsync_idle_low = 1; c.timings.flags.vsync_idle_low = 1;   // same signalling as Elecrow's LovyanGFX setup
  c.timings.flags.pclk_active_neg = 1;
  c.data_width = 16;
  c.bits_per_pixel = 16;
  c.num_fbs = 1;
  c.bounce_buffer_size_px = SCREEN_W * 8;     // 2 x 12.8 KB internal RAM
  c.dma_burst_size = 64;
  c.hsync_gpio_num = 40; c.vsync_gpio_num = 41; c.de_gpio_num = 42; c.pclk_gpio_num = 39; c.disp_gpio_num = -1;
  const int data[16] = {21, 47, 48, 45, 38,  9, 10, 11, 12, 13, 14,  7, 17, 18, 3, 46};   // B0-4, G0-5, R0-4
  for (int i = 0; i < 16; i++) c.data_gpio_nums[i] = data[i];
  c.flags.fb_in_psram = 1;
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&c, &lcdPanel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(lcdPanel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(lcdPanel));
}
static void dispFlush(lv_display_t *d, const lv_area_t *area, uint8_t *px) {
  esp_lcd_panel_draw_bitmap(lcdPanel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);   // copies into the frame buffer
  lv_display_flush_ready(d);
}
// GT911 touch at 0x5D, read through Wire (status 0x814E: bit7 = data ready, low nibble = points; point 1 at 0x8150)
#define GT911_ADDR 0x5D
static bool gt911Read(uint16_t &x, uint16_t &y, bool &ready) {
  uint8_t st = 0, p[4];
  Wire.beginTransmission(GT911_ADDR); Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)1) != 1) { ready = false; return false; }
  st = Wire.read();
  ready = st & 0x80;
  if (!ready) return false;
  bool down = (st & 0x0F) > 0;
  if (down) {
    Wire.beginTransmission(GT911_ADDR); Wire.write(0x81); Wire.write(0x50); Wire.endTransmission(false);
    if (Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)4) == 4) {
      for (int i = 0; i < 4; i++) p[i] = Wire.read();
      x = p[0] | (p[1] << 8); y = p[2] | (p[3] << 8);
    } else down = false;
  }
  Wire.beginTransmission(GT911_ADDR); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission();
  return down;
}
static void touchRead(lv_indev_t *, lv_indev_data_t *data) {
  static bool lastDown = false;
  static uint16_t lx, ly;
  uint16_t x, y; bool ready;
  xSemaphoreTake(i2cMx, portMAX_DELAY);
  bool t = gt911Read(x, y, ready);
  xSemaphoreGive(i2cMx);
  if (ready) { lastDown = t; if (t) { lx = min<uint16_t>(x, SCREEN_W - 1); ly = min<uint16_t>(y, SCREEN_H - 1); } }
  data->state = lastDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = lx; data->point.y = ly;
}

static void setBrightness(int pct) {
  brightPct = constrain(pct, 10, 100);
  stc((uint8_t)((100 - brightPct) * 200 / 100));          // 0 = brightest ... 244 = dimmest
}

// ───────────── toast + modal ─────────────
static void uiToast(const char *msg, uint32_t col) {
  lv_label_set_text(toastLbl, msg);
  lv_obj_set_style_text_color(toastLbl, lv_color_hex(col), 0);
  lv_obj_set_style_border_color(toastBox, lv_color_hex(col), 0);
  lv_obj_remove_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toastBox);
  toastUntil = millis() + 5000;
}
static void showModal(const char *title, const char *body) {
  lv_label_set_text(modalTitle, title);
  lv_label_set_text(modalBody, body);
  lv_obj_remove_flag(modalOv, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(modalOv);
}
static void onModalOk(lv_event_t *) { lv_obj_add_flag(modalOv, LV_OBJ_FLAG_HIDDEN); }
static void setBootMode(int m) { bootMode = m; prefs.putUChar("mode", (uint8_t)m); }

static void overlaysBuild() {
  toastBox = panel(lv_layer_top(), 120, 8, 560, 56, C_CARD, 28);
  lv_obj_set_style_border_width(toastBox, 3, 0);
  lv_obj_set_style_shadow_width(toastBox, 30, 0);
  lv_obj_set_style_shadow_opa(toastBox, LV_OPA_50, 0);
  toastLbl = mkLabel(toastBox, "", F_M, C_TEXT);
  lv_obj_set_width(toastLbl, 520);
  lv_label_set_long_mode(toastLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(toastLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(toastLbl);
  lv_obj_add_flag(toastBox, LV_OBJ_FLAG_HIDDEN);

  modalOv = panel(lv_layer_top(), 0, 0, SCREEN_W, SCREEN_H, 0x000000, 0);
  lv_obj_set_style_bg_opa(modalOv, LV_OPA_90, 0);
  lv_obj_t *c = panel(modalOv, 100, 110, 600, 260, C_CARD, 32);
  modalTitle = mkLabel(c, "", F_L, C_ACCENT);
  lv_obj_set_pos(modalTitle, 28, 24);
  modalBody = mkLabel(c, "", F_M, C_TEXT);
  lv_obj_set_width(modalBody, 544);
  lv_label_set_long_mode(modalBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(modalBody, 28, 82);
  textBtn(c, 200, 190, 200, 56, "OK", F_M, C_ACCENT, onModalOk);
  lv_obj_add_flag(modalOv, LV_OBJ_FLAG_HIDDEN);
}

// ───────────── Wi-Fi screens ─────────────
// Mesh-friendly connect: scan every channel and join the strongest node with this name (the default fast scan
// takes the first match, often a weak far-away node); auto-reconnect re-picks the best node after a drop.
static void wifiBegin(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
}
static void startScan() {
  WiFi.mode(WIFI_STA);
  if (netState != NET_UP) WiFi.disconnect();   // a pending (re)connect to an out-of-range network makes every scan fail
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  scanning = true;
  lv_obj_clean(wifiList);
  lv_label_set_text(wifiStatus, "Scanning...");
}
static void showListView() { lv_obj_remove_flag(wifiListView, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN); }
static void showWifiScreen(bool allowBack) {
  if (allowBack) lv_obj_remove_flag(wifiBackBtn, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(wifiBackBtn, LV_OBJ_FLAG_HIDDEN);
  showListView();
  lv_screen_load(scrWifi);
  startScan();
}
static void doConnect(const char *pass) {
  selPass = pass;
  WiFi.disconnect(true);
  wifiBegin(selSSID, selPass);
  netState = NET_CONNECTING;
  netT0 = millis();
  lv_label_set_text_fmt(wifiPwStatus, "Connecting to %s...", selSSID.c_str());
  lv_obj_set_style_text_color(wifiPwStatus, lv_color_hex(C_MUTED), 0);
}
static void onNetClick(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  selSSID = netSSID[idx];
  if (netOpen[idx]) { showListView(); lv_label_set_text_fmt(wifiStatus, "Connecting to %s...", selSSID.c_str()); doConnect(""); return; }
  lv_label_set_text(wifiSsidLbl, selSSID.c_str());
  lv_textarea_set_text(wifiTa, "");
  lv_label_set_text(wifiPwStatus, "");
  lv_obj_add_flag(wifiListView, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN);
}
static void onRescan(lv_event_t *) { startScan(); }
static void onWifiBack(lv_event_t *) { lv_screen_load(scrMainRef); }
static void onPwBack(lv_event_t *) { showListView(); }
static void onConnectBtn(lv_event_t *) { doConnect(lv_textarea_get_text(wifiTa)); }
static void onKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) doConnect(lv_textarea_get_text(wifiTa));
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) showListView();
}
static void onEye(lv_event_t *) { lv_textarea_set_password_mode(wifiTa, !lv_textarea_get_password_mode(wifiTa)); }
static void onWifiBtn(lv_event_t *) { showWifiScreen(netState == NET_UP); }

static void populateNetworks() {
  int n = WiFi.scanComplete();
  lv_obj_clean(wifiList);
  netCount = 0;
  for (int i = 0; i < n && netCount < 16; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int k = 0; k < netCount; k++) if (netSSID[k] == s) dup = true;
    if (dup) continue;
    netSSID[netCount] = s;
    netOpen[netCount] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    int rssi = WiFi.RSSI(i);
    lv_obj_t *row = lv_button_create(wifiList);
    lv_obj_set_size(row, 736, 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD3), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    char buf[40]; copyAscii(buf, sizeof(buf), s.c_str());
    lv_obj_t *name = mkLabel(row, buf, F_M, C_TEXT);
    lv_obj_set_width(name, 540);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
    uint32_t col = rssi > -60 ? C_GOOD : (rssi > -75 ? C_WARN : C_BAD);
    lv_obj_t *sig = mkLabel(row, netOpen[netCount] ? LV_SYMBOL_WIFI " open" : LV_SYMBOL_WIFI, F_M, col);
    lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(row, onNetClick, LV_EVENT_CLICKED, (void *)(intptr_t)netCount);
    netCount++;
  }
  WiFi.scanDelete();
  scanning = false;
  lv_label_set_text(wifiStatus, netCount ? "Tap your network" : "No networks found - tap Scan");
}

static void ntpCb(struct timeval *) { ntpSynced = true; }

static void onWifiUp(bool autoplay) {
  netState = NET_UP;
  WiFi.setSleep(false);                                      // smoother streaming
  Serial.printf("wifi up, heap: internal %u free (largest %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  sntp_set_time_sync_notification_cb(ntpCb);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  remoteBegin();
  reqWeather = true; reqAgenda = true; reqHa = true;
  if (moreUrl) {
    char u[80]; snprintf(u, sizeof(u), "http://%s/?k=%s", WiFi.localIP().toString().c_str(), remoteToken.c_str());
    lv_label_set_text(moreUrl, u);
    lv_qrcode_update(moreQr, u, strlen(u));
  }
  lv_screen_load(scrMainRef);
  if (autoplay) playCurrent();
}

static void buildWifi() {
  scrWifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrWifi, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scrWifi, LV_OBJ_FLAG_SCROLLABLE);
  wifiListView = clearBox(scrWifi, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_t *title = mkLabel(wifiListView, LV_SYMBOL_WIFI "  Connect to Wi-Fi", F_L, C_TEXT);
  lv_obj_set_pos(title, 32, 22);
  wifiStatus = mkLabel(wifiListView, "", F_S, C_MUTED);
  lv_obj_set_pos(wifiStatus, 34, 66);
  wifiBackBtn = textBtn(wifiListView, 520, 12, 120, 56, LV_SYMBOL_LEFT " Back", F_M, C_CARD3, onWifiBack);
  textBtn(wifiListView, 650, 12, 126, 56, LV_SYMBOL_REFRESH " Scan", F_M, C_ACCENT, onRescan);
  wifiList = scrollCol(wifiListView, 32, 96, 736, 364, 10);

  wifiPwView = clearBox(scrWifi, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN);
  textBtn(wifiPwView, 20, 14, 120, 56, LV_SYMBOL_LEFT " Back", F_M, C_CARD3, onPwBack);
  wifiSsidLbl = mkLabel(wifiPwView, "", F_L, C_TEXT);
  lv_obj_set_width(wifiSsidLbl, 600);
  lv_label_set_long_mode(wifiSsidLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(wifiSsidLbl, 160, 24);
  wifiTa = lv_textarea_create(wifiPwView);
  lv_obj_set_pos(wifiTa, 20, 84);
  lv_obj_set_size(wifiTa, 560, 64);
  lv_textarea_set_one_line(wifiTa, true);
  lv_textarea_set_password_mode(wifiTa, true);
  lv_textarea_set_placeholder_text(wifiTa, "Wi-Fi password");
  lv_obj_set_style_text_font(wifiTa, F_M, 0);
  lv_obj_set_style_bg_color(wifiTa, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_text_color(wifiTa, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_border_width(wifiTa, 0, 0);
  lv_obj_set_style_radius(wifiTa, 18, 0);
  textBtn(wifiPwView, 592, 84, 72, 64, LV_SYMBOL_EYE_OPEN, F_M, C_CARD3, onEye);
  textBtn(wifiPwView, 674, 84, 106, 64, "Join", F_M, C_ACCENT, onConnectBtn);
  wifiPwStatus = mkLabel(wifiPwView, "", F_S, C_MUTED);
  lv_obj_set_pos(wifiPwStatus, 24, 158);
  wifiKb = lv_keyboard_create(wifiPwView);
  lv_obj_set_size(wifiKb, SCREEN_W, 270);
  lv_obj_align(wifiKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(wifiKb, F_M, 0);
  lv_keyboard_set_textarea(wifiKb, wifiTa);
  lv_obj_add_event_cb(wifiKb, onKb, LV_EVENT_ALL, nullptr);
}

// ───────────── navigation: rail + header ─────────────
static const char *PAGE_NAMES[N_PAGES] = {"Home", "Radio", "Podcasts", "Weather", "Agenda", "House", "Timers", "More"};
static const char *PAGE_ICONS[N_PAGES] = {LV_SYMBOL_HOME, LV_SYMBOL_AUDIO, LV_SYMBOL_LIST, LV_SYMBOL_TINT, LV_SYMBOL_LIST, LV_SYMBOL_CHARGE, LV_SYMBOL_BELL, LV_SYMBOL_SETTINGS};

static void showPage(int p) {
  if (p < 0 || p >= N_PAGES) return;
  curPage = p;
  for (int i = 0; i < N_PAGES; i++) {
    if (i == p) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    bool sel = (i == p);
    lv_obj_set_style_bg_opa(railBtn[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(railBtn[i], 0), lv_color_hex(sel ? C_ACCENT : C_MUTED), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(railBtn[i], 1), lv_color_hex(sel ? C_TEXT : C_MUTED), 0);
  }
  lv_label_set_text(lblPageTitle, PAGE_NAMES[p]);
  if (p == PG_HOUSE) reqHa = true;
  if (p == PG_WEATHER) weatherRefreshUi();
  if (p == PG_AGENDA) agendaRefreshUi();
  if (p == PG_HOUSE) houseRefreshUi();
}
static void onRail(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }
static void onTimerChip(lv_event_t *) { showPage(PG_TIMERS); }
static void onMicBtn(lv_event_t *) {
  if (netState != NET_UP) { uiToast("Connect to Wi-Fi first", C_WARN); return; }
  voiceTrigger();
}

static void buildChrome(lv_obj_t *scr) {
  panel(scr, 0, 0, RAIL_W, SCREEN_H, C_CARD, 0);
  for (int i = 0; i < N_PAGES; i++) {
    lv_obj_t *b = lv_button_create(scr);
    railBtn[i] = b;
    lv_obj_set_pos(b, 6, 4 + i * 59);
    lv_obj_set_size(b, RAIL_W - 12, 56);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD3), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_t *ic = mkLabel(b, PAGE_ICONS[i], F_M, C_MUTED);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *cap = mkLabel(b, PAGE_NAMES[i], F_XS, C_MUTED);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(b, onRail, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  lblPageTitle = mkLabel(scr, "Home", F_L, C_TEXT);
  lv_obj_set_pos(lblPageTitle, PAGE_X + 18, 14);
  hdrClock = mkLabel(scr, "--:--", F_M, C_MUTED);
  lv_obj_set_pos(hdrClock, 468, 20);
  hdrTimer = textBtn(scr, 540, 10, 112, 44, "", F_S, C_CARD3, onTimerChip);
  lv_obj_add_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  hdrWifi = textBtn(scr, 660, 10, 60, 44, LV_SYMBOL_WIFI, F_M, C_CARD2, onWifiBtn);
  lv_obj_t *mic = lv_button_create(scr);
  lv_obj_set_pos(mic, 732, 4);
  lv_obj_set_size(mic, 58, 58);
  lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(mic, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_grad_color(mic, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(mic, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(mic, 24, 0);
  lv_obj_set_style_shadow_color(mic, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(mic, LV_OPA_40, 0);
  lv_obj_set_style_border_width(mic, 0, 0);
  lv_obj_add_event_cb(mic, onMicBtn, LV_EVENT_CLICKED, nullptr);
  drawMic(mic, 29, 27, 40, C_TEXT);
  for (int i = 0; i < N_PAGES; i++) {
    pages[i] = clearBox(scr, PAGE_X, PAGE_Y, PAGE_W, PAGE_H);
    lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
  }
}

// ───────────── home page ─────────────
static void onHomeAsk(lv_event_t *) { onMicBtn(nullptr); }
static void onHomeFrame(lv_event_t *) { ambientEnter(); }
static void onHomeTimers(lv_event_t *) { showPage(PG_TIMERS); }
static void onHomeNight(lv_event_t *) { ambientEnter(); }
static void onHomeNextTap(lv_event_t *) { showPage(PG_AGENDA); }
static void onHomeWxTap(lv_event_t *) { showPage(PG_WEATHER); }
static void onHomeRadioTap(lv_event_t *) { showPage(PG_RADIO); }

static void homeBuild(lv_obj_t *pg) {
  lv_obj_t *cc = card(pg, 12, 4, 440, 200);
  homeClock = mkLabel(cc, "--:--", F_CLK, C_TEXT);
  lv_obj_align(homeClock, LV_ALIGN_TOP_MID, 0, 0);
  homeDate = mkLabel(cc, "", F_M, C_MUTED);
  lv_obj_align(homeDate, LV_ALIGN_BOTTOM_MID, 0, -14);

  lv_obj_t *wc = card(pg, 464, 4, 236, 200);
  lv_obj_add_event_cb(wc, onHomeWxTap, LV_EVENT_CLICKED, nullptr);
  homeWxIcon = wxIcon(wc, 16, 16, 72, WX_PARTLY, false);
  homeWxTemp = mkLabel(wc, "--", F_XL, C_TEXT);
  lv_obj_set_pos(homeWxTemp, 100, 24);
  homeWxCond = mkLabel(wc, "", F_S, C_TEXT);
  lv_obj_set_width(homeWxCond, 210);
  lv_label_set_long_mode(homeWxCond, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(homeWxCond, 16, 108);
  homeWxHiLo = mkLabel(wc, "", F_S, C_MUTED);
  lv_obj_set_pos(homeWxHiLo, 16, 140);
  homeWxPlace = mkLabel(wc, wxPlace, F_XS, C_MUTED);
  lv_obj_set_pos(homeWxPlace, 16, 170);

  lv_obj_t *nc = card(pg, 12, 212, 440, 92);
  lv_obj_add_event_cb(nc, onHomeNextTap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *ncap = mkLabel(nc, "NEXT UP", F_XS, C_ACCENT);
  lv_obj_set_style_text_letter_space(ncap, 2, 0);
  lv_obj_set_pos(ncap, 20, 12);
  homeNextT = mkLabel(nc, "Nothing scheduled", F_M, C_TEXT);
  lv_obj_set_width(homeNextT, 400);
  lv_label_set_long_mode(homeNextT, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(homeNextT, 20, 34);
  homeNextW = mkLabel(nc, "", F_S, C_MUTED);
  lv_obj_set_pos(homeNextW, 20, 62);

  lv_obj_t *rc = card(pg, 464, 212, 236, 92);
  lv_obj_add_event_cb(rc, onHomeRadioTap, LV_EVENT_CLICKED, nullptr);
  homeStation = mkLabel(rc, curSt.name, F_S, C_TEXT);
  lv_obj_set_width(homeStation, 140);
  lv_label_set_long_mode(homeStation, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(homeStation, 16, 18);
  homeStat = mkLabel(rc, "", F_XS, C_MUTED);
  lv_obj_set_width(homeStat, 140);
  lv_label_set_long_mode(homeStat, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(homeStat, 16, 48);
  lv_obj_t *pb = textBtn(rc, 160, 14, 64, 64, LV_SYMBOL_PLAY, F_M, C_ACCENT, onPlayPause);
  homePlay = btnLabel(pb);

  struct { const char *t; lv_event_cb_t cb; uint32_t col; } q[4] = {
    {LV_SYMBOL_AUDIO "  Ask Crow", onHomeAsk, C_ACCENT}, {LV_SYMBOL_IMAGE "  Photo frame", onHomeFrame, C_CARD3},
    {LV_SYMBOL_BELL "  Timers", onHomeTimers, C_CARD3}, {LV_SYMBOL_EYE_CLOSE "  Night clock", onHomeNight, C_CARD3}};
  for (int i = 0; i < 4; i++) textBtn(pg, 12 + i * 174, 316, 166, 84, q[i].t, F_S, q[i].col, q[i].cb);
}

static void homeTick(bool newSecond) {
  if (newSecond) {
    struct tm t;
    if (nowTm(&t)) {
      char b[8]; snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
      lv_label_set_text(homeClock, b); lv_label_set_text(hdrClock, b);
      char d[40]; snprintf(d, sizeof(d), "%s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
      lv_label_set_text(homeDate, d);
    }
  }
  static uint32_t seenWx = 0, seenAg = 0;
  if (wxVer != seenWx) {
    seenWx = wxVer;
    DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
    if (w.valid) {
      wxDraw(homeWxIcon, wxKindOf(w.code), !w.day);
      lv_label_set_text_fmt(homeWxTemp, "%d" "\xC2\xB0", w.temp);
      lv_label_set_text(homeWxCond, wxText(w.code));
      lv_label_set_text_fmt(homeWxHiLo, "H %d\xC2\xB0   L %d\xC2\xB0", w.days[0].hi, w.days[0].lo);
    }
  }
  static time_t lastNextStart = -1;
  Appt a; bool hasNext = agendaNext(a);
  time_t key = hasNext ? a.start : 0;
  if (agVer != seenAg || key != lastNextStart) {
    seenAg = agVer; lastNextStart = key;
    if (hasNext) { char w[48]; fmtWhen(a, w, sizeof(w)); lv_label_set_text(homeNextT, a.title); lv_label_set_text(homeNextW, w); }
    else { lv_label_set_text(homeNextT, agendaConfigured ? "Nothing coming up" : "No calendar connected"); lv_label_set_text(homeNextW, agendaConfigured ? "Enjoy the quiet" : "See config.h"); }
  }
  lv_label_set_text(homeStation, curSt.name);
  static const char *lastS = nullptr;
  if (radioStatusText != lastS) { lastS = radioStatusText; lv_label_set_text(homeStat, radioStatusText); }
  static bool lastIcon = true;
  bool icon = (playState == 1 || playState == 2);
  if (icon != lastIcon) { lastIcon = icon; lv_label_set_text(homePlay, icon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); }
}

// ───────────── more page (settings + tools) ─────────────
static void onBright(lv_event_t *e) {
  setBrightness(lv_slider_get_value(sldBright));
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs.putUChar("bri", (uint8_t)brightPct);
}
static void onMoreFrame(lv_event_t *) { ambientEnter(); }
static void onDoomsday(lv_event_t *) {
  setBootMode(1);
  showModal("Doomsday mode armed", "Power the hub OFF. Fit the SX1262 LoRa module, set switches S1 S0 to 01, then power on. The radio, mic and speaker share pins, so this is a separate mode.");
}
static void onRestart(lv_event_t *) { ESP.restart(); }

static lv_obj_t *moreRow(lv_obj_t *col, int h) { return card(col, 0, 0, 688, h); }

// ───────────── rename the playing station ─────────────
static lv_obj_t *renOv, *renTa, *renKb;
static void renClose() { lv_obj_add_flag(renOv, LV_OBJ_FLAG_HIDDEN); }
static void onRenOk(lv_event_t *) { stationRename(lv_textarea_get_text(renTa)); renClose(); }
static void onRenCancel(lv_event_t *) { renClose(); }
static void onRenKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) onRenOk(e);
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) renClose();
}
static void onRenameBtn(lv_event_t *) {
  if (!renOv) {
    renOv = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(renOv);
    lv_obj_set_size(renOv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(renOv, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(renOv, LV_OPA_COVER, 0);
    lv_obj_add_flag(renOv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mkLabel(renOv, LV_SYMBOL_EDIT "  Rename station", F_L, C_TEXT);
    lv_obj_set_pos(t, 24, 24);
    renTa = lv_textarea_create(renOv);
    lv_obj_set_pos(renTa, 20, 84);
    lv_obj_set_size(renTa, 520, 64);
    lv_textarea_set_one_line(renTa, true);
    lv_textarea_set_max_length(renTa, sizeof(curSt.name) - 1);
    lv_obj_set_style_text_font(renTa, F_M, 0);
    lv_obj_set_style_bg_color(renTa, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_text_color(renTa, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_width(renTa, 0, 0);
    lv_obj_set_style_radius(renTa, 18, 0);
    textBtn(renOv, 552, 84, 112, 64, "Cancel", F_M, C_CARD3, onRenCancel);
    textBtn(renOv, 674, 84, 106, 64, "Save", F_M, C_ACCENT, onRenOk);
    renKb = lv_keyboard_create(renOv);
    lv_obj_set_size(renKb, SCREEN_W, 270);
    lv_obj_align(renKb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(renKb, F_M, 0);
    lv_keyboard_set_textarea(renKb, renTa);
    lv_obj_add_event_cb(renKb, onRenKb, LV_EVENT_ALL, nullptr);
  }
  lv_textarea_set_text(renTa, curSt.name);
  lv_obj_remove_flag(renOv, LV_OBJ_FLAG_HIDDEN);
}

// ───────────── Podcasts page: New (latest from followed shows) / Following / Find ─────────────
static lv_obj_t *pcTabBtn[3], *pcList, *pcStatus, *pcSearchBox, *pcTa, *pcKb;
static int pcTab = 0;                                   // 0 New, 1 Following, 2 Find
static bool pcInShow = false;                           // showing one show's episodes (from Following / Find)
static char pcShowName[64];
static void pcLayout(bool kb) {
  for (int t = 0; t < 3; t++) lv_obj_set_style_bg_color(pcTabBtn[t], lv_color_hex(t == pcTab ? C_ACCENT : C_CARD3), 0);
  bool find = (pcTab == 2 && !pcInShow);
  if (find) lv_obj_remove_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN);
  int y = find ? 150 : 90;
  lv_obj_set_pos(pcStatus, 16, y - 28);
  lv_obj_set_pos(pcList, 12, y);
  lv_obj_set_height(pcList, PAGE_H - 4 - y);
  if (find && kb) { lv_obj_remove_flag(pcKb, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(pcList, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(pcKb, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(pcList, LV_OBJ_FLAG_HIDDEN); }
}
static lv_obj_t *pcRow() {
  lv_obj_t *row = lv_button_create(pcList);
  lv_obj_set_size(row, 688, 66);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD3), LV_STATE_PRESSED);
  lv_obj_set_style_radius(row, 18, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  return row;
}
static void pcTwoLines(lv_obj_t *row, const char *a, const char *b, int w, bool dot = false) {
  int x = dot ? 22 : 4;
  if (dot) {                                            // unplayed marker
    lv_obj_t *d = panel(row, 2, 12, 10, 10, C_ACCENT, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_t *n = mkLabel(row, a, F_M, C_TEXT);
  lv_obj_set_width(n, w - x);
  lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(n, x, 0);
  lv_obj_t *d = mkLabel(row, b, F_XS, C_MUTED);
  lv_obj_set_width(d, w - x);
  lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(d, x, 30);
}
static void pcPlay(const Episode &ep, const char *show) {
  Station s;
  setStation(s, ep.title, show, ep.url);
  s.kind = 1;
  podMarkPlayed(ep.url);
  playStationData(s, -1);
  showPage(PG_RADIO);
}
static void pcNewList() {                               // "New": latest across followed shows
  lv_obj_clean(pcList);
  DATA_LOCK();
  for (int i = 0; i < nNew; i++) {
    lv_obj_t *row = pcRow();
    char d[80]; snprintf(d, sizeof(d), "%s  -  %s", newEps[i].show, newEps[i].e.when);
    pcTwoLines(row, newEps[i].e.title, d, 680, !podIsPlayed(newEps[i].e.url));
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
      int k = (int)(intptr_t)lv_event_get_user_data(e);
      if (k >= nNew) return;
      NewEp x = newEps[k];
      pcPlay(x.e, x.show);
    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  int n = nNew;
  DATA_UNLOCK();
  if (newLoading && !n) lv_label_set_text(pcStatus, "Checking your shows for new episodes...");
  else if (!n) lv_label_set_text(pcStatus, nPods ? "No episodes yet - tap " LV_SYMBOL_REFRESH " to check" : "Follow some shows in Find");
  else lv_label_set_text(pcStatus, newLoading ? "Latest from the shows you follow (checking...)" : "Latest from the shows you follow");
}
// rows carry an index: 0..99 followed shows, 100+ search results
static const Pod *pcPodOf(int k) { return k >= 100 ? &podHits[k - 100] : &pods[k]; }
static void pcOpenShow(const Pod &p) {
  if (netState != NET_UP) { lv_label_set_text(pcStatus, "Connect to Wi-Fi first"); return; }
  pcInShow = true;
  strlcpy(pcShowName, p.name, sizeof(pcShowName));
  strlcpy(epFeed, p.feed, sizeof(epFeed));
  reqEpisodes = true;
  lv_obj_clean(pcList);
  lv_label_set_text_fmt(pcStatus, "%s - loading episodes...", p.name);
  pcLayout(false);
}
static void onPcShow(lv_event_t *e) { Pod p = *pcPodOf((int)(intptr_t)lv_event_get_user_data(e)); pcOpenShow(p); }
static void onPcFollow(lv_event_t *e) {
  Pod p = *pcPodOf((int)(intptr_t)lv_event_get_user_data(e));
  bool was = podFind(p.feed) >= 0;
  if (!was && nPods >= MAX_PODS) { uiToast("Following too many shows - unfollow one first", C_WARN); return; }
  podToggle(p);
  lv_label_set_text(btnLabel((lv_obj_t *)lv_event_get_target(e)), was ? LV_SYMBOL_PLUS : LV_SYMBOL_OK);
  uiToast(was ? "Unfollowed" : "Following - new episodes show up in New", was ? C_MUTED : C_GOOD);
}
static void pcShowsList(bool results) {                 // followed shows, or search results
  lv_obj_clean(pcList);
  int n = results ? nPodHits : nPods;
  for (int i = 0; i < n; i++) {
    const Pod &p = results ? podHits[i] : pods[i];
    int k = results ? 100 + i : i;
    lv_obj_t *row = pcRow();
    pcTwoLines(row, p.name, p.author, 600);
    lv_obj_add_event_cb(row, onPcShow, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    textBtn(row, 610, 0, 64, 46, podFind(p.feed) >= 0 ? LV_SYMBOL_OK : LV_SYMBOL_PLUS, F_M, C_CARD3, onPcFollow, (void *)(intptr_t)k);
  }
}
static void pcShowTab(int t) {
  pcTab = t;
  pcInShow = false;
  if (t == 0) pcNewList();
  else if (t == 1) { pcShowsList(false); lv_label_set_text(pcStatus, nPods ? "Shows you follow - tap for episodes, " LV_SYMBOL_OK " to unfollow" : "Follow shows from Find"); }
  else { lv_obj_clean(pcList); lv_label_set_text(pcStatus, "Search the podcast directory (or a SoundCloud show)"); }
  pcLayout(t == 2);
}
static void onPcTab(lv_event_t *e) { pcShowTab((int)(intptr_t)lv_event_get_user_data(e)); }
static void onPcRefresh(lv_event_t *) {
  if (netState != NET_UP) { lv_label_set_text(pcStatus, "Connect to Wi-Fi first"); return; }
  reqNewEps = true;
  newLoading = true;
  pcShowTab(0);
}
static void onPcEpisode(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  if (k < nEps) { Episode ep = eps[k]; pcPlay(ep, pcShowName); }
}
static void pcTick(lv_timer_t *) {
  static uint8_t seenEp = 0, seenHit = 0, seenNew = 0;
  if (epVer != seenEp) {
    seenEp = epVer;
    if (!pcInShow) return;
    lv_obj_clean(pcList);
    if (nEps < 0) { lv_label_set_text(pcStatus, "Couldn't load this show - try again"); return; }
    if (nEps == 0) { lv_label_set_text(pcStatus, "No playable episodes in this feed"); return; }
    lv_label_set_text_fmt(pcStatus, "%s - latest %d episodes", pcShowName, (int)nEps);
    for (int i = 0; i < nEps; i++) {
      lv_obj_t *row = pcRow();
      pcTwoLines(row, eps[i].title, eps[i].when, 680, !podIsPlayed(eps[i].url));
      lv_obj_add_event_cb(row, onPcEpisode, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
  }
  if (podHitVer != seenHit) {
    seenHit = podHitVer;
    if (pcInShow || pcTab != 2) return;
    if (nPodHits < 0) { lv_obj_clean(pcList); lv_label_set_text(pcStatus, "Search failed - check Wi-Fi and try again"); return; }
    pcShowsList(true);
    lv_label_set_text_fmt(pcStatus, nPodHits ? "%d shows - tap for episodes, " LV_SYMBOL_PLUS " to follow" : "No shows found", (int)nPodHits);
  }
  if (newVer != seenNew) {
    seenNew = newVer;
    if (pcTab == 0 && !pcInShow) pcNewList();
  }
}
static void pcSearch() {
  const char *q = lv_textarea_get_text(pcTa);
  if (!q[0]) return;
  if (netState != NET_UP) { lv_label_set_text(pcStatus, "Connect to Wi-Fi first"); return; }
  strlcpy(podQuery, q, sizeof(podQuery));
  reqPodSearch = true;
  lv_obj_clean(pcList);
  lv_label_set_text(pcStatus, "Searching podcasts...");
  pcLayout(false);
}
static void onPcSearchBtn(lv_event_t *) { pcSearch(); }
static void onPcTa(lv_event_t *) { pcLayout(true); }
static void onPcKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) pcSearch();
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) pcLayout(false);
}
static void podcastsBuild(lv_obj_t *pg) {
  static const char *tabs[3] = {"New", "Following", LV_SYMBOL_EDIT " Find"};
  for (int t = 0; t < 3; t++) pcTabBtn[t] = textBtn(pg, 12 + t * 172, 4, 164, 50, tabs[t], F_S, C_CARD3, onPcTab, (void *)(intptr_t)t);
  textBtn(pg, 628, 4, 72, 50, LV_SYMBOL_REFRESH, F_M, C_CARD3, onPcRefresh);
  pcSearchBox = clearBox(pg, 0, 60, PAGE_W, 64);
  pcTa = lv_textarea_create(pcSearchBox);
  lv_obj_set_pos(pcTa, 12, 2);
  lv_obj_set_size(pcTa, 580, 58);
  lv_textarea_set_one_line(pcTa, true);
  lv_textarea_set_placeholder_text(pcTa, "Show, host or topic");
  lv_obj_set_style_text_font(pcTa, F_M, 0);
  lv_obj_set_style_bg_color(pcTa, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_text_color(pcTa, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_border_width(pcTa, 0, 0);
  lv_obj_set_style_radius(pcTa, 18, 0);
  lv_obj_add_event_cb(pcTa, onPcTa, LV_EVENT_CLICKED, nullptr);
  textBtn(pcSearchBox, 600, 2, 100, 58, "Search", F_M, C_ACCENT, onPcSearchBtn);
  pcStatus = mkLabel(pg, "", F_S, C_MUTED);
  pcList = scrollCol(pg, 12, 90, 688, 320, 8);
  pcKb = lv_keyboard_create(pg);
  lv_obj_set_size(pcKb, PAGE_W, 240);
  lv_obj_align(pcKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(pcKb, F_M, 0);
  lv_keyboard_set_textarea(pcKb, pcTa);
  lv_obj_add_event_cb(pcKb, onPcKb, LV_EVENT_ALL, nullptr);
  lv_timer_create(pcTick, 200, nullptr);
  pcShowTab(0);
}

// ───────────── find stations: Search / Genres / Places tabs + Feeling lucky ─────────────
// Results (radio-browser.info) share one list: tap to play, + to keep as a favourite.
static lv_obj_t *scrFind, *findTa, *findKb, *findList, *findStatus, *findSearchBox, *findGenres, *findPlaces;
static lv_obj_t *findTabBtn[3];
static int findTab = 0;
static bool luckyPending = false;
static int luckyGenre = 0;

static void findLayout(bool kb) {
  bool srch = (findTab == 0);
  for (int t = 0; t < 3; t++) lv_obj_set_style_bg_color(findTabBtn[t], lv_color_hex(t == findTab ? C_ACCENT : C_CARD3), 0);
  if (srch) lv_obj_remove_flag(findSearchBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findSearchBox, LV_OBJ_FLAG_HIDDEN);
  if (findTab == 1) lv_obj_remove_flag(findGenres, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findGenres, LV_OBJ_FLAG_HIDDEN);
  if (findTab == 2) lv_obj_remove_flag(findPlaces, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findPlaces, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(findStatus, 24, srch ? 158 : 244);
  lv_obj_set_pos(findList, 32, srch ? 190 : 270);
  lv_obj_set_height(findList, srch ? 280 : 200);
  bool showKb = srch && kb;
  if (showKb) { lv_obj_remove_flag(findKb, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(findList, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(findKb, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(findList, LV_OBJ_FLAG_HIDDEN); }
}
static bool findNetOk() {
  if (netState == NET_UP) return true;
  lv_label_set_text(findStatus, "Connect to Wi-Fi first");
  return false;
}
static void findRequest(uint8_t req, const char *q, const char *what) {
  strlcpy(rbQuery, q, sizeof(rbQuery));
  rbReq = req;
  lv_obj_clean(findList);
  lv_label_set_text_fmt(findStatus, "Loading %s...", what);
}
static void findSearch() {
  const char *q = lv_textarea_get_text(findTa);
  if (!q[0] || !findNetOk()) return;
  findRequest(RB_SEARCH, q, "stations");
  findLayout(false);
}
static void findFavLabel(lv_obj_t *btn, const Station &s) {
  lv_label_set_text(btnLabel(btn), favFind(s.url) >= 0 ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
}
static void onFindFav(lv_event_t *e) {
  const Station &s = rbHits[(intptr_t)lv_event_get_user_data(e)].s;
  bool was = favFind(s.url) >= 0;
  if (!was && nFavs >= MAX_FAVS) { uiToast("Favourites are full - remove one first", C_WARN); return; }
  favToggle(s);
  findFavLabel((lv_obj_t *)lv_event_get_target(e), s);
  uiToast(was ? "Removed from favourites" : "Added to favourites", was ? C_MUTED : C_GOOD);
}
static void findPlay(const Station &s) {
  playStationData(s, favFind(s.url));
  lv_screen_load(scrMainRef);
  showPage(PG_RADIO);
}
static void onFindPlay(lv_event_t *e) { findPlay(rbHits[(intptr_t)lv_event_get_user_data(e)].s); }

// highlight one chip / card in a row of them (restyle only: runs inside their click event)
static void findMark(lv_obj_t *parent, int sel) {
  for (int i = 0; i < (int)lv_obj_get_child_count(parent); i++) {
    lv_obj_t *b = lv_obj_get_child(parent, i);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(b, i == sel ? 3 : 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == sel ? C_CARD3 : C_CARD2), 0);
  }
}
static void onGenre(lv_event_t *e) {
  int g = (int)(intptr_t)lv_event_get_user_data(e);
  findMark(findGenres, g);
  if (!findNetOk()) return;
  char w[48]; snprintf(w, sizeof(w), "%s stations", GENRES[g].name);
  findRequest(RB_BY_GENRE, GENRES[g].tag, w);
}
static void onPlace(lv_event_t *e) {
  int p = (int)(intptr_t)lv_event_get_user_data(e);
  findMark(findPlaces, p);
  if (!findNetOk()) return;
  char w[56]; snprintf(w, sizeof(w), "stations in %s", rbPlaces[p].name);
  findRequest(RB_BY_PLACE, rbPlaces[p].key, w);
}
static lv_obj_t *findChip(lv_obj_t *parent, int w, int h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 18, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  return b;
}
static void findPlacesBuild() {
  lv_obj_clean(findPlaces);
  for (int i = 0; i < nPlaces; i++) {
    lv_obj_t *b = findChip(findPlaces, 196, 140);
    lv_obj_set_style_pad_all(b, 14, 0);
    lv_obj_t *n = mkLabel(b, rbPlaces[i].name, F_M, C_TEXT);
    lv_obj_set_width(n, 168);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(n, 80, 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *c = mkLabel(b, "", F_XS, i == 0 ? C_ACCENT : C_MUTED);
    lv_label_set_text_fmt(c, i == 0 ? "Home  -  %d stations" : "%d stations", rbPlaces[i].count);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b, onPlace, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}
static void onFindTab(lv_event_t *e) {
  findTab = (int)(intptr_t)lv_event_get_user_data(e);
  lv_obj_clean(findList);
  if (findTab == 0) lv_label_set_text(findStatus, "Search 50,000 stations worldwide");
  else if (findTab == 1) lv_label_set_text(findStatus, "Pick a genre (swipe for more)");
  else {
    if (nPlaces < 0 && findNetOk()) { nPlaces = -2; reqPlaces = true; }
    lv_label_set_text(findStatus, nPlaces > 0 ? "Pick a country (swipe for more)" : "Loading countries...");
  }
  findLayout(findTab == 0);
}
static void onLucky(lv_event_t *) {
  if (!findNetOk()) return;
  luckyGenre = esp_random() % N_GENRES;
  luckyPending = true;
  char w[48]; snprintf(w, sizeof(w), "something %s", GENRES[luckyGenre].name);
  findRequest(RB_BY_GENRE, GENRES[luckyGenre].tag, w);
  findLayout(false);
}

static void findTick(lv_timer_t *) {
  static uint8_t seenCat = 0;
  if (rbCatVer != seenCat) {                             // country list arrived
    seenCat = rbCatVer;
    if (nPlaces > 0) findPlacesBuild();
    if (findTab == 2) lv_label_set_text(findStatus, nPlaces > 0 ? "Pick a country (swipe for more)" : "Couldn't load countries - tap Places again");
  }
  static uint8_t seen = 0;
  if (rbVer == seen) return;
  seen = rbVer;
  lv_obj_clean(findList);
  if (luckyPending) {                                    // Feeling lucky: play a random one of the genre's stations
    luckyPending = false;
    if (rbN > 0) {
      const Station &s = rbHits[esp_random() % rbN].s;
      char m[96]; snprintf(m, sizeof(m), "Feeling lucky: %s (%s)", s.name, GENRES[luckyGenre].name);
      findPlay(s);
      uiToast(m, C_ACCENT);
      return;
    }
  }
  if (rbN < 0) { lv_label_set_text(findStatus, "Couldn't load stations - check Wi-Fi and try again"); return; }
  if (rbN == 0) { lv_label_set_text(findStatus, "Nothing found - try another name, town or genre"); return; }
  lv_label_set_text_fmt(findStatus, "%d stations - tap to play, " LV_SYMBOL_PLUS " to keep", (int)rbN);
  for (int i = 0; i < rbN; i++) {
    lv_obj_t *row = lv_button_create(findList);
    lv_obj_set_size(row, 736, 66);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD3), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *name = mkLabel(row, rbHits[i].s.name, F_M, C_TEXT);
    lv_obj_set_width(name, 600);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, 4, 0);
    lv_obj_t *det = mkLabel(row, rbHits[i].detail, F_XS, C_MUTED);
    lv_obj_set_width(det, 600);
    lv_label_set_long_mode(det, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(det, 4, 30);
    lv_obj_add_event_cb(row, onFindPlay, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *fb = textBtn(row, 650, 0, 64, 46, "", F_M, C_CARD3, onFindFav, (void *)(intptr_t)i);
    findFavLabel(fb, rbHits[i].s);
  }
}
static void onFindBack(lv_event_t *) { lv_screen_load(scrMainRef); }
static void onFindSearchBtn(lv_event_t *) { findSearch(); }
static void onFindTa(lv_event_t *) { findLayout(true); }
static void onFindKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) findSearch();
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) findLayout(false);
}
static lv_obj_t *findStrip(int flow) {                   // horizontally swiping area under the tabs
  lv_obj_t *o = lv_obj_create(scrFind);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, 0, 84);
  lv_obj_set_size(o, SCREEN_W, 152);
  lv_obj_set_flex_flow(o, (lv_flex_flow_t)flow);
  lv_obj_set_style_pad_hor(o, 20, 0);
  lv_obj_set_style_pad_row(o, 8, 0);
  lv_obj_set_style_pad_column(o, 10, 0);
  lv_obj_set_scroll_dir(o, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
  return o;
}
static void findBuild() {
  scrFind = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrFind, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scrFind, LV_OBJ_FLAG_SCROLLABLE);
  textBtn(scrFind, 20, 14, 112, 56, LV_SYMBOL_LEFT " Back", F_M, C_CARD3, onFindBack);
  static const char *tabs[3] = {LV_SYMBOL_EDIT " Search", "Genres", LV_SYMBOL_GPS " Places"};
  for (int t = 0; t < 3; t++) findTabBtn[t] = textBtn(scrFind, 146 + t * 142, 14, 134, 56, tabs[t], F_S, C_CARD3, onFindTab, (void *)(intptr_t)t);
  lv_obj_t *lucky = textBtn(scrFind, 582, 14, 198, 56, LV_SYMBOL_SHUFFLE "  Feeling lucky", F_S, C_ACCENT, onLucky);
  lv_obj_set_style_bg_grad_color(lucky, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(lucky, LV_GRAD_DIR_VER, 0);

  findSearchBox = clearBox(scrFind, 0, 80, SCREEN_W, 74);
  findTa = lv_textarea_create(findSearchBox);
  lv_obj_set_pos(findTa, 20, 4);
  lv_obj_set_size(findTa, 644, 64);
  lv_textarea_set_one_line(findTa, true);
  lv_textarea_set_placeholder_text(findTa, "Name, town or genre: KEXP, BBC, Paris, jazz...");
  lv_obj_set_style_text_font(findTa, F_M, 0);
  lv_obj_set_style_bg_color(findTa, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_text_color(findTa, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_border_width(findTa, 0, 0);
  lv_obj_set_style_radius(findTa, 18, 0);
  lv_obj_add_event_cb(findTa, onFindTa, LV_EVENT_CLICKED, nullptr);
  textBtn(findSearchBox, 674, 4, 106, 64, "Search", F_M, C_ACCENT, onFindSearchBtn);

  findGenres = findStrip(LV_FLEX_FLOW_COLUMN_WRAP);       // 3 rows of genre chips, swipe sideways for more
  for (int g = 0; g < N_GENRES; g++) {
    lv_obj_t *b = findChip(findGenres, 168, 44);
    lv_obj_t *l = mkLabel(b, GENRES[g].name, F_S, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onGenre, LV_EVENT_CLICKED, (void *)(intptr_t)g);
  }
  findPlaces = findStrip(LV_FLEX_FLOW_ROW);                // carousel of countries
  lv_obj_set_flex_align(findPlaces, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_snap_x(findPlaces, LV_SCROLL_SNAP_CENTER);

  findStatus = mkLabel(scrFind, "", F_S, C_MUTED);
  findList = scrollCol(scrFind, 32, 190, 736, 280, 8);
  findKb = lv_keyboard_create(scrFind);
  lv_obj_set_size(findKb, SCREEN_W, 270);
  lv_obj_align(findKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(findKb, F_M, 0);
  lv_keyboard_set_textarea(findKb, findTa);
  lv_obj_add_event_cb(findKb, onFindKb, LV_EVENT_ALL, nullptr);
  lv_timer_create(findTick, 200, nullptr);
}
static void onFindStations(lv_event_t *) {
  if (!scrFind) findBuild();
  findTab = 0;
  lv_textarea_set_text(findTa, "");
  lv_obj_clean(findList);
  lv_label_set_text(findStatus, "Search 50,000 stations worldwide");
  findLayout(true);
  lv_screen_load(scrFind);
}

// ───────────── weather location screen: type a town, pick from the matches ─────────────
static lv_obj_t *scrLoc, *locTa, *locKb, *locList, *locStatus;
static void locShowKb(bool kb) {
  if (kb) { lv_obj_remove_flag(locKb, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(locList, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(locKb, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(locList, LV_OBJ_FLAG_HIDDEN); }
}
static void locSearch() {
  const char *q = lv_textarea_get_text(locTa);
  if (!q[0]) return;
  if (netState != NET_UP) { lv_label_set_text(locStatus, "Connect to Wi-Fi first"); return; }
  strlcpy(geoQuery, q, sizeof(geoQuery));
  reqGeo = true;
  lv_obj_clean(locList);
  lv_label_set_text(locStatus, "Searching...");
  locShowKb(false);
}
static void onLocPick(lv_event_t *e) {
  const GeoHit &g = geoHits[(intptr_t)lv_event_get_user_data(e)];
  weatherSaveLocation(g.lat, g.lon, g.name, g.cc);
  lv_label_set_text(homeWxPlace, wxPlace);
  lv_label_set_text(wxLblPlace, wxPlace);
  lv_label_set_text_fmt(moreLocLbl, LV_SYMBOL_GPS "  Weather: %s", wxPlace);
  lv_screen_load(scrMainRef);
  char m[64]; snprintf(m, sizeof(m), "Weather: %s", wxPlace);
  uiToast(m, C_GOOD);
}
static void locTick(lv_timer_t *) {                      // show search results when netTask has them
  static uint8_t seen = 0;
  if (geoVer == seen) return;
  seen = geoVer;
  lv_obj_clean(locList);
  if (geoN < 0) { lv_label_set_text(locStatus, "Search failed - check Wi-Fi and try again"); return; }
  if (geoN == 0) { lv_label_set_text(locStatus, "No places found - try another spelling"); return; }
  lv_label_set_text(locStatus, "Tap your town");
  for (int i = 0; i < geoN; i++) {
    lv_obj_t *row = lv_button_create(locList);
    lv_obj_set_size(row, 736, 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD3), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *name = mkLabel(row, geoHits[i].name, F_M, C_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *det = mkLabel(row, geoHits[i].detail, F_S, C_MUTED);
    lv_obj_set_width(det, 400);
    lv_label_set_long_mode(det, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(det, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(det, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(row, onLocPick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}
static void onLocBack(lv_event_t *) { lv_screen_load(scrMainRef); }
static void onLocSearchBtn(lv_event_t *) { locSearch(); }
static void onLocTa(lv_event_t *) { locShowKb(true); }
static void onLocKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) locSearch();
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) locShowKb(false);
}
static void locBuild() {
  scrLoc = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrLoc, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scrLoc, LV_OBJ_FLAG_SCROLLABLE);
  textBtn(scrLoc, 20, 14, 120, 56, LV_SYMBOL_LEFT " Back", F_M, C_CARD3, onLocBack);
  lv_obj_t *title = mkLabel(scrLoc, LV_SYMBOL_GPS "  Weather location", F_L, C_TEXT);
  lv_obj_set_pos(title, 160, 24);
  locTa = lv_textarea_create(scrLoc);
  lv_obj_set_pos(locTa, 20, 84);
  lv_obj_set_size(locTa, 644, 64);
  lv_textarea_set_one_line(locTa, true);
  lv_textarea_set_placeholder_text(locTa, "Your town, e.g. Leeds");
  lv_obj_set_style_text_font(locTa, F_M, 0);
  lv_obj_set_style_bg_color(locTa, lv_color_hex(C_CARD2), 0);
  lv_obj_set_style_text_color(locTa, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_border_width(locTa, 0, 0);
  lv_obj_set_style_radius(locTa, 18, 0);
  lv_obj_add_event_cb(locTa, onLocTa, LV_EVENT_CLICKED, nullptr);
  textBtn(scrLoc, 674, 84, 106, 64, "Search", F_M, C_ACCENT, onLocSearchBtn);
  locStatus = mkLabel(scrLoc, "", F_S, C_MUTED);
  lv_obj_set_pos(locStatus, 24, 158);
  locList = scrollCol(scrLoc, 32, 190, 736, 280, 8);
  locKb = lv_keyboard_create(scrLoc);
  lv_obj_set_size(locKb, SCREEN_W, 270);
  lv_obj_align(locKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(locKb, F_M, 0);
  lv_keyboard_set_textarea(locKb, locTa);
  lv_obj_add_event_cb(locKb, onLocKb, LV_EVENT_ALL, nullptr);
  lv_timer_create(locTick, 200, nullptr);
}
static void onLocBtn(lv_event_t *) {
  if (!scrLoc) locBuild();
  lv_textarea_set_text(locTa, "");
  lv_obj_clean(locList);
  lv_label_set_text_fmt(locStatus, "Now: %s", wxPlace);
  locShowKb(true);
  lv_screen_load(scrLoc);
}

static void moreBuild(lv_obj_t *pg) {
  lv_obj_t *col = scrollCol(pg, 12, 4, 688, 408, 10);
  lv_obj_t *r = moreRow(col, 84);
  mkLabel(r, LV_SYMBOL_EYE_OPEN "  Brightness", F_M, C_TEXT);
  lv_obj_set_pos(lv_obj_get_child(r, 0), 20, 14);
  sldBright = lv_slider_create(r);
  lv_slider_set_range(sldBright, 10, 100);
  lv_slider_set_value(sldBright, brightPct, LV_ANIM_OFF);
  lv_obj_set_size(sldBright, 640, 14);
  lv_obj_set_pos(sldBright, 24, 54);
  lv_obj_set_style_bg_color(sldBright, lv_color_hex(C_CARD3), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sldBright, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sldBright, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sldBright, 12, LV_PART_KNOB);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_RELEASED, nullptr);

  r = moreRow(col, 190);
  mkLabel(r, LV_SYMBOL_UPLOAD "  Phone remote", F_M, C_TEXT);
  lv_obj_set_pos(lv_obj_get_child(r, 0), 20, 14);
  moreQr = lv_qrcode_create(r);
  lv_qrcode_set_size(moreQr, 140);
  lv_qrcode_set_dark_color(moreQr, lv_color_hex(0x000000));
  lv_qrcode_set_light_color(moreQr, lv_color_hex(0xFFFFFF));
  lv_obj_set_pos(moreQr, 20, 40);
  lv_obj_set_style_border_color(moreQr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(moreQr, 6, 0);
  lv_qrcode_update(moreQr, "http://crowhub.local", 20);
  moreUrl = mkLabel(r, "http://crowhub.local", F_S, C_ACCENT);
  lv_obj_set_pos(moreUrl, 190, 50);
  lv_obj_t *d = mkLabel(r, "Scan with your phone: control the radio, start timers,\nsend a message to this screen and upload photos\nfor the photo frame.", F_S, C_MUTED);
  lv_obj_set_pos(d, 190, 84);

  r = moreRow(col, 84);
  moreWifiLbl = mkLabel(r, LV_SYMBOL_WIFI "  Wi-Fi", F_M, C_TEXT);
  lv_obj_set_pos(moreWifiLbl, 20, 26);
  textBtn(r, 500, 14, 170, 56, "Change", F_M, C_CARD3, onWifiBtn);

  r = moreRow(col, 84);
  moreLocLbl = mkLabel(r, "", F_M, C_TEXT);
  lv_obj_set_width(moreLocLbl, 460);
  lv_label_set_long_mode(moreLocLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(moreLocLbl, 20, 26);
  lv_label_set_text_fmt(moreLocLbl, LV_SYMBOL_GPS "  Weather: %s", wxPlace);
  textBtn(r, 500, 14, 170, 56, "Change", F_M, C_CARD3, onLocBtn);

  r = moreRow(col, 84);
  morePhotos = mkLabel(r, LV_SYMBOL_IMAGE "  Photo frame", F_M, C_TEXT);
  lv_obj_set_pos(morePhotos, 20, 26);
  textBtn(r, 500, 14, 170, 56, "Start now", F_M, C_CARD3, onMoreFrame);

  r = moreRow(col, 108);
  mkLabel(r, LV_SYMBOL_WARNING "  Doomsday mode (LoRa mesh)", F_M, C_TEXT);
  lv_obj_set_pos(lv_obj_get_child(r, 0), 20, 12);
  lv_obj_t *dd = mkLabel(r, "Off-grid text messaging with other hubs. Needs the LoRa module\nand switches S1 S0 = 01 (radio shares pins with speaker).", F_XS, C_MUTED);
  lv_obj_set_pos(dd, 20, 48);
  textBtn(r, 500, 44, 170, 52, "Arm", F_S, C_CARD3, onDoomsday);

  r = moreRow(col, 120);
  moreSys = mkLabel(r, "", F_S, C_MUTED);
  lv_obj_set_pos(moreSys, 20, 14);
  textBtn(r, 500, 56, 170, 52, "Restart", F_S, C_CARD3, onRestart);
}

static void moreTick() {
  if (curPage != PG_MORE) return;
  char b[200];
  snprintf(b, sizeof(b), "Free memory: %u KB internal, %u KB PSRAM\nUp %lu min    Flash photos: %d", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024), (unsigned long)(millis() / 60000), nPhotos);
  lv_label_set_text(moreSys, b);
  char w[80];
  if (netState == NET_UP) snprintf(w, sizeof(w), LV_SYMBOL_WIFI "  %s  (%d dBm)", WiFi.SSID().c_str(), WiFi.RSSI()); else snprintf(w, sizeof(w), LV_SYMBOL_WIFI "  not connected");
  char a[84]; copyAscii(a, sizeof(a), w);
  lv_label_set_text(moreWifiLbl, a);
  char p[60]; snprintf(p, sizeof(p), LV_SYMBOL_IMAGE "  Photo frame");
  lv_label_set_text(morePhotos, p);
}

// ───────────── network worker ─────────────
static void netTask(void *) {
  uint32_t lastWx = 0, lastAg = 0, lastHa = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && timeIsValid()) {
      uint32_t now = millis();
      if (reqWeather || !lastWx || now - lastWx > 20UL * 60000UL) { reqWeather = false; lastWx = now; if (!weatherFetch()) lastWx = now - 19UL * 60000UL; }
      if (reqAgenda || !lastAg || now - lastAg > 15UL * 60000UL) { reqAgenda = false; lastAg = now; if (!agendaFetch() && agendaConfigured) lastAg = now - 14UL * 60000UL; }
      if (HA_ENABLED && (reqHa || now - lastHa > (curPage == PG_HOUSE ? 4000UL : 30000UL))) { reqHa = false; lastHa = now; haPoll(); }
      if (reqGeo) { reqGeo = false; geoFetch(); }
      if (rbReq) { uint8_t r = rbReq; rbReq = RB_NONE; radioStationsFetch(r); }
      if (reqPlaces) { reqPlaces = false; radioPlacesFetch(); }
      if (reqEpisodes) { reqEpisodes = false; podEpisodesFetch(); }
      static uint32_t lastNew = 0;                         // podcasts "New": at boot, every 3 h, and on follow changes
      if (reqNewEps || !lastNew || now - lastNew > 3UL * 3600000UL) { reqNewEps = false; lastNew = now; newEpsFetch(); }
      if (reqPodSearch) { reqPodSearch = false; podSearchFetch(); }
      static uint32_t lastNp = 0;                          // now-playing info for streams that don't send it (BBC)
      if (playState == 2 && audioMode == 0 && (reqNowPlaying || now - lastNp > 30000)) { reqNowPlaying = false; lastNp = now; nowPlayingPoll(); }
      HaCall c;
      while (haQ && xQueueReceive(haQ, &c, 0) == pdTRUE) haCallNow(c);
    }
    vTaskDelay(150);
  }
}

// ───────────── 5 Hz UI tick ─────────────
static void uiTick(lv_timer_t *) {
  uint32_t now = millis();
  // Wi-Fi state machine
  if (scanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) populateNetworks();
    else if (n == WIFI_SCAN_FAILED) { scanning = false; lv_label_set_text(wifiStatus, "Scan failed - tap Scan"); }
  }
  if (netState == NET_BOOT) {
    if (WiFi.status() == WL_CONNECTED) onWifiUp(AUTOPLAY_ON_BOOT);
    else if (now - netT0 > 15000) {                         // saved network not found (yet)
      netState = NET_NONE; netT0 = now;
      if (lv_screen_active() != scrWifi) showWifiScreen(false);   // once, not every tick
    }
  } else if (netState == NET_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) { prefs.putString("ssid", selSSID); prefs.putString("pass", selPass); onWifiUp(true); }
    else if (now - netT0 > 20000) {
      netState = NET_NONE; WiFi.disconnect(true);
      lv_label_set_text(wifiPwStatus, "Couldn't connect - check the password");
      lv_obj_set_style_text_color(wifiPwStatus, lv_color_hex(C_BAD), 0);
      lv_label_set_text(wifiStatus, "Couldn't connect - pick a network to try again");
    }
  }
  // keep retrying the saved network in the background (router rebooting, hub moved room), unless the user is
  // scanning or typing a password
  if (netState == NET_NONE && !scanning && now - netT0 > 30000 && lv_obj_has_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN)) {
    String ssid = prefs.getString("ssid", "");
    if (ssid.length()) {
      selSSID = ssid; selPass = prefs.getString("pass", "");
      lv_label_set_text_fmt(wifiStatus, "Trying %s again... (or tap your network)", ssid.c_str());
      wifiBegin(selSSID, selPass);
      netState = NET_BOOT;
    }
    netT0 = now;
  }
  if (ntpSynced) { ntpSynced = false; systemToRtc(); }
  lv_obj_set_style_text_color(lv_obj_get_child(hdrWifi, 0), lv_color_hex(netState == NET_UP ? C_GOOD : C_BAD), 0);

  // messages from worker tasks
  UiMsg m;
  while (uiQ && xQueueReceive(uiQ, &m, 0) == pdTRUE) {
    if (m.type == UM_TOAST) uiToast(m.text, m.color);
    else if (m.type == UM_VOICE_STATE) voiceUiState(m.text, m.color);
    else if (m.type == UM_VOICE_RESULT) voiceUiResult(m.text, m.color);
  }
  voiceActions();
  voiceUiTick();
  if (toastUntil && (int32_t)(now - toastUntil) >= 0) { toastUntil = 0; lv_obj_add_flag(toastBox, LV_OBJ_FLAG_HIDDEN); }

  radioTick(netState == NET_UP);
  char chip[24];
  bool anyTimer = timersTick(chip, sizeof(chip));
  if (anyTimer) { lv_label_set_text(btnLabel(hdrTimer), chip); lv_obj_remove_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN); } else lv_obj_add_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  beepTick();
  agendaTick();

  // redraw data pages when new data landed
  static uint32_t sw = 0, sa = 0, sh = 0;
  if (wxVer != sw) { sw = wxVer; if (curPage == PG_WEATHER) weatherRefreshUi(); }
  if (agVer != sa) { sa = agVer; if (curPage == PG_AGENDA) agendaRefreshUi(); }
  if (haVer != sh) { sh = haVer; if (curPage == PG_HOUSE) houseRefreshUi(); }
  static uint32_t lastSec = 0;
  bool newSec = (now - lastSec) >= 1000;
  if (newSec) lastSec = now;
  homeTick(newSec);
  moreTick();

  // ambient mode: idle photo frame / clock, and the night clock
  ambientTick();
  if (ambientActive) return;
  uint32_t idle = lv_display_get_inactive_time(disp);
  bool mainShown = (lv_screen_active() == scrMainRef);
  bool busy = voiceBusy || (alarmOv && !lv_obj_has_flag(alarmOv, LV_OBJ_FLAG_HIDDEN)) || !lv_obj_has_flag(modalOv, LV_OBJ_FLAG_HIDDEN);
  if (mainShown && !busy) {
#if AMBIENT_AFTER_MIN > 0
    if (idle > (uint32_t)AMBIENT_AFTER_MIN * 60000UL) ambientEnter();
#endif
    if (isNight() && idle > 60000UL && timeIsValid()) ambientEnter();
  }
}

// ───────────── setup / loop ─────────────
// TLS buffers in PSRAM: the prebuilt core keeps them in internal RAM, where ~40 KB per connection doesn't fit
// next to Wi-Fi and the task stacks.
static void *tlsCalloc(size_t n, size_t sz) {
  void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(n, sz);
}

void setup() {
  Serial.begin(115200);
  mbedtls_platform_set_calloc_free(tlsCalloc, free);
  // The audio task (core 0) can keep that core busy for > 5 s on HLS streams (BBC), starving its idle task; the task
  // watchdog would then reboot the hub. Wi-Fi runs at higher priority and nothing else on core 0 needs the idle task.
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = 5000;
  wdt.idle_core_mask = 0;                                    // core 1's idle task wasn't watched either
  wdt.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdt);
  prefs.begin("crowhub", false);
  bootMode   = prefs.getUChar("mode", 0);
  volume     = constrain(prefs.getUChar("vol", 10), 0, 21);
  brightPct  = constrain(prefs.getUChar("bri", 100), 10, 100);
  remoteTokenInit();
  weatherLoadLocation();
  i2cMx = xSemaphoreCreateMutex();
  dataMx = xSemaphoreCreateMutex();
  uiQ = xQueueCreate(10, sizeof(UiMsg));
  haQ = xQueueCreate(6, sizeof(HaCall));

  Wire.begin(15, 16);
  delay(50);
  for (int i = 0; i < 20; i++) {                             // wait for the helper MCU + touch chip (as in Elecrow's lessons)
    Wire.beginTransmission(0x30); bool a = Wire.endTransmission() == 0;
    Wire.beginTransmission(0x5D); bool b = Wire.endTransmission() == 0;
    if (a && b) break;
    stc(STC_TOUCH_ON);
    pinMode(1, OUTPUT); digitalWrite(1, LOW); delay(120); pinMode(1, INPUT); delay(100);
  }
  stc(STC_TOUCH_ON);
  delay(100);
  panelInit();
  setBrightness(brightPct);

  lfsOk = LittleFS.begin(true);
  favsLoad();
  podsLoad();
#if ENABLE_SD
  sdMount();
#endif
  rtcToSystem();

  lv_init();
  lv_tick_set_cb(lvTick);
  // One draw buffer in INTERNAL RAM. The RGB panel streams its frame buffer out of PSRAM non-stop; rendering into
  // PSRAM as well starves it and the picture shakes / tears sideways. dispFlush copies synchronously, so a second
  // buffer would buy nothing.
  const uint32_t bufBytes = SCREEN_W * 16 * 2;
  drawBuf1 = (uint8_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!drawBuf1) drawBuf1 = (uint8_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  drawBuf2 = nullptr;
  disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(disp, dispFlush);
  lv_display_set_buffers(disp, drawBuf1, drawBuf2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchRead);

  if (bootMode == 1) {                                        // ── survival mesh: no Wi-Fi, no audio ──
    loraBuild();
    lv_screen_load(scrLora);
    loraBegin();
    lv_timer_create([](lv_timer_t *) { loraTick(); loraUiTick(); beepTick(); }, 200, nullptr);
    return;
  }

  detectSwap();
  audioBegin();
  voiceBegin();

  scrMainRef = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrMainRef, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scrMainRef, LV_OBJ_FLAG_SCROLLABLE);
  buildChrome(scrMainRef);
  homeBuild(pages[PG_HOME]);
  radioBuild(pages[PG_RADIO]);
  podcastsBuild(pages[PG_PODCASTS]);
  weatherBuild(pages[PG_WEATHER]);
  agendaBuild(pages[PG_AGENDA]);
  houseBuild(pages[PG_HOUSE]);
  timersBuild(pages[PG_TIMERS]);
  moreBuild(pages[PG_MORE]);
  alarmBuild();
  overlaysBuild();
  voiceUiBuild();
  buildWifi();
  ambientBuild();
  showPage(PG_HOME);
  lv_screen_load(scrMainRef);
  lv_timer_create(uiTick, 200, nullptr);

  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 1);

  Serial.printf("heap: internal %u free (largest %u), PSRAM %u free\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) {
    selSSID = ssid; selPass = prefs.getString("pass", "");
    wifiBegin(selSSID, selPass);
    netState = NET_BOOT;
    netT0 = millis();
  } else {
    showWifiScreen(false);
  }
}

// serial console for testing from a PC (115200): "play <stream url>"
static void serialTick() {
  static char line[300];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (n < sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;
    if (!strncmp(line, "play ", 5) || !strncmp(line, "ep ", 3)) {    // "ep <url>": play as a podcast episode
      bool ep = line[0] == 'e';
      const char *u = line + (ep ? 3 : 5);
      Station s; setStation(s, "Test stream", u, u);
      s.kind = ep ? 1 : 0;
      playStationData(s, favFind(s.url));
      Serial.printf("playing %s\n", s.url);
    } else if (!strncmp(line, "seek ", 5)) {                         // "seek <+/- seconds>"
      audioCmd(AC_SEEK, atoi(line + 5));
    } else if (!strcmp(line, "pos")) {
      Serial.printf("pos %u / %u s, state %u\n", (unsigned)posSec, (unsigned)durSec, (unsigned)playState);
    }
  }
}

void loop() {
  lv_timer_handler();
  remoteTick();
  serialTick();
  delay(5);
}
