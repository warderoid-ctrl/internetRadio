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

static lv_obj_t *moreRow(lv_obj_t *col, int h) { return card(col, 0, 0, 688, h); }
static lv_obj_t *moreTitle(lv_obj_t *row, const char *t, int y) {
  lv_obj_t *l = mkLabel(row, t, F_M, C_TEXT);
  lv_obj_set_pos(l, 20, y);
  return l;
}

static void moreBuild(lv_obj_t *pg) {
  lv_obj_t *col = scrollCol(pg, 12, 4, 688, 408, 10);
  lv_obj_t *r = moreRow(col, 84);
  moreTitle(r, LV_SYMBOL_EYE_OPEN "  Brightness", 14);
  sldBright = mkSlider(r, 24, 54, 640, 10, 100, brightPct, C_ACCENT, 12);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldBright, onBright, LV_EVENT_RELEASED, nullptr);

  r = moreRow(col, 190);
  moreTitle(r, LV_SYMBOL_UPLOAD "  Phone remote", 14);
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
  moreWifiLbl = moreTitle(r, LV_SYMBOL_WIFI "  Wi-Fi", 26);
  oneLine(moreWifiLbl, 460, false);
  textBtn(r, 500, 14, 170, 56, "Change", F_M, C_CARD3, onWifiBtn);

  r = moreRow(col, 84);
  moreLocLbl = moreTitle(r, "", 26);
  oneLine(moreLocLbl, 460, false);
  lv_label_set_text_fmt(moreLocLbl, LV_SYMBOL_GPS "  Weather: %s", wxPlace);
  textBtn(r, 500, 14, 170, 56, "Change", F_M, C_CARD3, onLocBtn);

  r = moreRow(col, 84);
  morePhotos = moreTitle(r, LV_SYMBOL_IMAGE "  Photo frame", 26);
  oneLine(morePhotos, 270, false);
  lv_obj_t *mb = textBtn(r, 300, 14, 190, 56, "", F_S, C_CARD3, onAmbMode);   // screensaver: Auto / Photos / Clock
  lv_label_set_text_fmt(btnLabel(mb), "Show: %s", AMB_MODE_NAMES[ambMode]);
  textBtn(r, 500, 14, 170, 56, "Start now", F_M, C_CARD3, onMoreFrame);

  r = moreRow(col, 108);
  moreTitle(r, LV_SYMBOL_WARNING "  Doomsday mode (LoRa mesh)", 12);
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
}
