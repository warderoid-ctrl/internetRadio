#pragma once
// More page: brightness, phone remote QR, Wi-Fi, weather location, photo frame / screensaver, doomsday mode, system.
// Part of the sketch: included from internetRadio.ino after the Wi-Fi and location screens.

static void onBright(lv_event_t *e) {
  setBrightness(lv_slider_get_value(sldBright));
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs.putUChar("bri", (uint8_t)brightPct);
}
static void onMoreFrame(lv_event_t *) { ambientEnter(); }
static void onAmbMode(lv_event_t *e) {
  ambMode = (ambMode + 1) % 3;
  prefs.putUChar("ssmode", ambMode);
  lv_label_set_text_fmt(btnLabel((lv_obj_t *)lv_event_get_target(e)), "Show: %s", AMB_MODE_NAMES[ambMode]);
}
static void onDoomsday(lv_event_t *) {
  setBootMode(1);
  showModal("Doomsday mode armed", "Power the hub OFF. Fit the SX1262 LoRa module, set switches S1 S0 to 01, then power on. The radio, mic and speaker share pins, so this is a separate mode.");
}
static void onRestart(lv_event_t *) { ESP.restart(); }
// theme: saved in NVS, then the hub restarts and builds every screen in the new colours
static void onTheme(lv_event_t *e) {
  ThemeMode m = (ThemeMode)(intptr_t)lv_event_get_user_data(e);
  if (m != themeMode) themeSet(m);
}

#define MORE_W  PAGE_W
#define MORE_BX (MORE_W - 20 - 170)                       // right-hand buttons
static lv_obj_t *moreRow(lv_obj_t *col, int h) { return card(col, 0, 0, MORE_W, h); }
static lv_obj_t *moreTitle(lv_obj_t *row, const char *t, int y) {
  lv_obj_t *l = mkLabel(row, t, I_20, C_TEXT);           // icon + Inter 16 title
  lv_obj_set_pos(l, 20, y);
  return l;
}

static void moreBuild(lv_obj_t *pg) {
  pg = pageBody(pg, "Settings");
  lv_obj_t *col = scrollCol(pg, 0, 0, MORE_W, PAGE_H, 10);
  lv_obj_t *r = moreRow(col, 84);
  moreTitle(r, LUCIDE_SUN_DIM "  Brightness", 14);
  sldBright = mkSlider(r, 24, 56, MORE_W - 48, 10, 100, brightPct);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_RELEASED, nullptr);

  r = moreRow(col, 84);
  moreTitle(r, LUCIDE_PALETTE "  Theme", 30);
  lv_obj_t *chips = chipRow(r, MORE_W - 20 - 200, 20, 200);
  lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  chip(chips, LUCIDE_MOON " Dark", themeMode == THEME_MODE_DARK, onTheme, (void *)(intptr_t)THEME_MODE_DARK);
  chip(chips, LUCIDE_SUN " Light", themeMode == THEME_MODE_LIGHT, onTheme, (void *)(intptr_t)THEME_MODE_LIGHT);

  r = moreRow(col, 190);
  moreTitle(r, LUCIDE_SMARTPHONE "  Phone remote", 14);
  moreQr = lv_qrcode_create(r);
  lv_qrcode_set_size(moreQr, 140);
  lv_qrcode_set_dark_color(moreQr, lv_color_hex(0x000000));
  lv_qrcode_set_light_color(moreQr, lv_color_hex(0xFFFFFF));
  lv_obj_set_pos(moreQr, 20, 40);
  lv_obj_set_style_border_color(moreQr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(moreQr, 6, 0);
  lv_qrcode_update(moreQr, "http://crowhub.local", 20);
  moreUrl = mkLabel(r, "http://crowhub.local", F_M, C_ACCENT);
  lv_obj_set_pos(moreUrl, 190, 50);
  lv_obj_t *d = mkLabel(r, "Scan with your phone: control the radio, start timers,\nsend a message to this screen and upload photos\nfor the photo frame.", F_S, C_MUTED);
  lv_obj_set_pos(d, 190, 84);

  r = moreRow(col, 84);
  moreWifiLbl = moreTitle(r, LUCIDE_WIFI "  Wi-Fi", 30);
  oneLine(moreWifiLbl, MORE_BX - 40, false);
  textBtn(r, MORE_BX, 18, 170, 48, "Change", F_M, T->surfaceHi, onWifiBtn);

  r = moreRow(col, 84);
  moreLocLbl = moreTitle(r, "", 30);
  oneLine(moreLocLbl, MORE_BX - 40, false);
  lv_label_set_text_fmt(moreLocLbl, LUCIDE_MAP_PIN "  Weather: %s", wxPlace);
  textBtn(r, MORE_BX, 18, 170, 48, "Change", F_M, T->surfaceHi, onLocBtn);

  r = moreRow(col, 84);
  morePhotos = moreTitle(r, LUCIDE_IMAGE "  Photo frame", 30);
  oneLine(morePhotos, MORE_BX - 200 - 40, false);
  lv_obj_t *mb = textBtn(r, MORE_BX - 200, 18, 190, 48, "", F_M, T->surfaceHi, onAmbMode);   // screensaver: Auto / Photos / Clock
  lv_label_set_text_fmt(btnLabel(mb), "Show: %s", AMB_MODE_NAMES[ambMode]);
  textBtn(r, MORE_BX, 18, 170, 48, "Start now", F_M, T->surfaceHi, onMoreFrame);

  r = moreRow(col, 108);
  moreTitle(r, LUCIDE_SATELLITE_DISH "  Doomsday mode (LoRa mesh)", 14);
  lv_obj_t *dd = mkLabel(r, "Off-grid text messaging with other hubs. Needs the LoRa module\nand switches S1 S0 = 01 (radio shares pins with speaker).", F_XS, C_MUTED);
  lv_obj_set_pos(dd, 20, 50);
  textBtn(r, MORE_BX, 44, 170, 48, "Arm", F_M, T->surfaceHi, onDoomsday);

  r = moreRow(col, 120);
  moreSys = mkLabel(r, "", F_S, C_MUTED);
  lv_obj_set_pos(moreSys, 20, 18);
  textBtn(r, MORE_BX, 56, 170, 48, LUCIDE_POWER " Restart", I_20, T->surfaceHi, onRestart);
}

static void moreTick() {
  if (curPage != PG_MORE) return;
  char b[200];
  snprintf(b, sizeof(b), "Free memory: %u KB internal, %u KB PSRAM\nUp %lu min    Flash photos: %d", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024), (unsigned long)(millis() / 60000), nPhotos);
  if (strcmp(lv_label_get_text(moreSys), b)) lv_label_set_text(moreSys, b);
  char w[80];
  if (netState == NET_UP) snprintf(w, sizeof(w), LUCIDE_WIFI "  %s  (%d dBm)", WiFi.SSID().c_str(), WiFi.RSSI()); else snprintf(w, sizeof(w), LUCIDE_WIFI_OFF "  not connected");
  if (strcmp(lv_label_get_text(moreWifiLbl), w)) lv_label_set_text(moreWifiLbl, w);   // (copyText would drop the icon)
}
