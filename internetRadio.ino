/*
  CROW HUB - kitchen hub for the Elecrow CrowPanel Advance 7.0" (ESP32-S3, V1.3 / V1.4 / V1.5)

  Internet radio * podcasts * weather * appointments * Home Assistant tiles * kitchen timers * voice assistant
  * photo frame / night clock * phone remote * LoRa survival mesh.

  Read README.md first. Settings live in config.h (and secrets.h); Wi-Fi, weather location, favourites and
  podcasts are set on the hub itself.

  Layout: library-style modules (stations.h, audio_engine.h, radio_browser.h, podcasts.h, weather.h, ...) come first;
  the screens (ui_*.h) are parts of this sketch, included further down where the state they use is defined.
  Tasks: UI (loop + LVGL timers, core 1), netTask (all HTTP, core 1), audio (core 0), voice (core 0).
*/

#include <Arduino.h>
#include <FS.h>            // file systems before LovyanGFX, or its drawJpgFile(fs, ...) overloads aren't built
#include <LittleFS.h>
#include <SD.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>   // only for decoding images (photos, logos) into sprites; the panel is driven by esp_lcd
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
#include "art.h"
#include "stations.h"
#include "audio_engine.h"
#include "radio_browser.h"
#include "radio_ui.h"
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
static uint8_t *drawBuf;
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
static lv_obj_t *hdrTimer, *hdrTalk, *toastBox, *toastLbl, *modalOv, *modalTitle, *modalBody;
static uint32_t toastUntil = 0;
// home page
static lv_obj_t *homeClock, *homeDate, *homeWxIcon, *homeWxTemp, *homeNextT, *homeNextW, *homeStation, *homeStat, *homePlay;
static lv_obj_t *homeWxPlace;
// more page
static lv_obj_t *moreLocLbl, *moreWifiLbl, *moreSys, *morePhotos, *moreUrl, *moreQr, *sldBright;

// ───────────── display (esp_lcd RGB panel) + touch (GT911 over Wire) ─────────────
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
  toastLbl = mkLabel(toastBox, "", F_M, C_TEXT);
  oneLine(toastLbl, 520, false);
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

#include "ui_wifi.h"

// ───────────── navigation: Home's bottom nav bar (ui_home.h); every other page has a back button ─────────────
// Pages with a header (pageBody()) share a timer chip and the talk button, top right.
static bool pageHasHeader(int p) { return p != PG_HOME && p != PG_PODCASTS && p != PG_WEATHER; }

static void showPage(int p) {
  if (p < 0 || p >= N_PAGES) return;
  curPage = p;
  for (int i = 0; i < N_PAGES; i++) {
    if (i == p) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (pageHasHeader(p)) lv_obj_remove_flag(hdrTalk, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(hdrTalk, LV_OBJ_FLAG_HIDDEN);
  if (!pageHasHeader(p)) lv_obj_add_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  if (p == PG_HOUSE) reqHa = true;
  if (p == PG_WEATHER) weatherRefreshUi();
  if (p == PG_AGENDA) agendaRefreshUi();
  if (p == PG_HOUSE) houseRefreshUi();
}
static void onTimerChip(lv_event_t *) { showPage(PG_TIMERS); }
static void onMicBtn(lv_event_t *) {
  if (netState != NET_UP) { uiToast("Connect to Wi-Fi first", C_WARN); return; }
  voiceTrigger();
}

static void buildChrome(lv_obj_t *scr) {
  for (int i = 0; i < N_PAGES; i++) {
    pages[i] = clearBox(scr, 0, 0, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
  }
}
static void buildHeaderTools(lv_obj_t *scr) {                // after the pages, so they sit on top
  hdrTimer = textBtn(scr, 552, 14, 132, 48, "", I_20, T->surfaceHi, onTimerChip);
  lv_obj_add_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  hdrTalk = textBtn(scr, SCREEN_W - T_PAD_X - 80, 14, 80, 48, LUCIDE_MIC, I_24, T->talk, onMicBtn);
  pressStyle(hdrTalk, 20);
}

#include "ui_home.h"
#include "ui_rename.h"
#include "ui_podcasts.h"
#include "ui_find.h"
#include "ui_location.h"
#include "ui_more.h"

// ───────────── network worker: every HTTP request happens here, one at a time ─────────────
static void netTask(void *) {
  uint32_t lastWx = 0, lastAg = 0, lastHa = 0, lastNew = 0, lastNp = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && timeIsValid()) {
      uint32_t now = millis();
      if (reqWeather || !lastWx || now - lastWx > 20UL * 60000UL) { reqWeather = false; lastWx = now; if (!weatherFetch()) lastWx = now - 19UL * 60000UL; }
      if (reqAgenda || !lastAg || now - lastAg > 15UL * 60000UL) { reqAgenda = false; lastAg = now; if (!agendaFetch() && agendaConfigured) lastAg = now - 14UL * 60000UL; }
      if (HA_ENABLED && (reqHa || now - lastHa > (curPage == PG_HOUSE ? 4000UL : 30000UL))) { reqHa = false; lastHa = now; haPoll(); }
      // things the UI asked for (each answers only the latest request: see radio_browser.h)
      if (geoWant != geoDone) geoFetch();
      if (rbWant != rbDone) radioStationsFetch();
      if (reqPlaces) { reqPlaces = false; radioPlacesFetch(); }
      if (epWant != epDone) podEpisodesFetch();
      if (podWant != podDone) podSearchFetch();
      if (logoStage == 1) radioLogoLookup();
      artNetTick();                                         // logos / artwork, one per pass
      // podcasts "New": at boot, every 3 h, and when the follow list changes
      if (reqNewEps || !lastNew || now - lastNew > 3UL * 3600000UL) { reqNewEps = false; lastNew = now; newEpsFetch(); }
      // now-playing info for streams that don't send it (BBC)
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
  wifiTick(now);
  if (ntpSynced) { ntpSynced = false; systemToRtc(); }

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
  podsTick();
  artUiTick();
  char chip[24];
  bool anyTimer = timersTick(chip, sizeof(chip));
  if (anyTimer && pageHasHeader(curPage)) {
    char b[40]; snprintf(b, sizeof(b), LUCIDE_TIMER " %s", chip);
    if (strcmp(lv_label_get_text(btnLabel(hdrTimer)), b)) lv_label_set_text(btnLabel(hdrTimer), b);
    lv_obj_remove_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(hdrTimer, LV_OBJ_FLAG_HIDDEN);
  homeTimerChip(anyTimer ? chip : nullptr);
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
  themeLoad();                                               // Tonal dark / light (NVS "ui"/"theme"): before any screen exists
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
  epPosLoad();                                               // where part-played podcast episodes got to
  calLoad();
  ambMode = prefs.getUChar("ssmode", 0) % 3;
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
  artInit();
  favsLoad();                                                // creates listMx: before podsLoad
  podsLoad();
#if ENABLE_SD
  sdMount();
#endif
  rtcToSystem();

  lv_init();
  lv_tick_set_cb(lvTick);
  // One small draw buffer in INTERNAL RAM: the RGB panel streams its frame buffer out of PSRAM non-stop and
  // rendering into PSRAM as well starved it; dispFlush copies synchronously, so a second buffer would buy nothing.
  const uint32_t bufBytes = SCREEN_W * 10 * 2;
  drawBuf = (uint8_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!drawBuf) drawBuf = (uint8_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(disp, dispFlush);
  lv_display_set_buffers(disp, drawBuf, nullptr, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(T->primary), lv_color_hex(T->secondary),
                                                   themeMode == THEME_MODE_DARK, F_M));
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
  buildHeaderTools(scrMainRef);
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
  // why a connection attempt failed (wrong password vs network not found...), for the serial log
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("wifi: disconnected, reason %u (%s)\n", r, WiFi.disconnectReasonName((wifi_err_reason_t)r));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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

// serial console for testing from a PC (115200): play <url>, ep <url>, seek <+/-s>, pos, mic, remote, theme dark|light
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
      playStationData(s);
      Serial.printf("playing %s\n", s.url);
    } else if (!strncmp(line, "seek ", 5)) {
      audioCmd(AC_SEEK, atoi(line + 5));
    } else if (!strcmp(line, "mic")) {                               // same as tapping the mic button
      voiceReq = true;
    } else if (!strcmp(line, "remote")) {                            // the phone remote's address (with its token)
      Serial.printf("remote: http://%s/?k=%s\n", WiFi.localIP().toString().c_str(), remoteToken.c_str());
    } else if (!strcmp(line, "theme dark") || !strcmp(line, "theme light")) {   // saves and restarts
      themeSet(line[6] == 'l' ? THEME_MODE_LIGHT : THEME_MODE_DARK);
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
