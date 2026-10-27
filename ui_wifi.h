#pragma once
// Wi-Fi: network list + password entry, the connect / retry logic, and what happens once Wi-Fi is up.
// Part of the sketch: included from internetRadio.ino after its state and toast helpers.

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
    lv_obj_t *row = listRow(wifiList, 736, 64);
    char buf[40]; copyText(buf, sizeof(buf), s.c_str());
    lv_obj_t *name = mkLabel(row, buf, F_M, C_TEXT);
    oneLine(name, 540, false);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);
    uint32_t col = rssi > -60 ? C_GOOD : (rssi > -75 ? C_WARN : C_BAD);
    lv_obj_t *sig = mkLabel(row, netOpen[netCount] ? "open  " LUCIDE_WIFI : LUCIDE_LOCK "  " LUCIDE_WIFI, I_20, col);
    lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -8, 0);
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

// connect / retry state machine, from the 5 Hz tick
static void wifiTick(uint32_t now) {
  if (scanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) populateNetworks();
    else if (n == WIFI_SCAN_FAILED) { scanning = false; lv_label_set_text(wifiStatus, "Scan failed - tap Scan"); }
  }
  if (netState == NET_BOOT) {
    if (WiFi.status() == WL_CONNECTED) onWifiUp(AUTOPLAY_ON_BOOT && prefs.getUChar("playing", 1));   // only if it was on
    else if (now - netT0 > 30000) {                         // saved network not there (yet): router rebooting after a
      netState = NET_NONE; netT0 = now;                       // power cut, slow mesh... keep the screens, retry quietly
      static bool told = false;
      if (!told) {
        told = true;
        char m[96]; snprintf(m, sizeof(m), "Can't reach %s yet - retrying. Settings > Wi-Fi to change network.", selSSID.c_str());
        uiToast(m, C_WARN);
      }
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
  if (netState == NET_NONE && !scanning && now - netT0 > 20000 && lv_obj_has_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN)) {
    String ssid = prefs.getString("ssid", "");
    if (ssid.length()) {
      selSSID = ssid; selPass = prefs.getString("pass", "");
      lv_label_set_text_fmt(wifiStatus, "Trying %s again... (or tap your network)", ssid.c_str());
      wifiBegin(selSSID, selPass);
      netState = NET_BOOT;
    }
    netT0 = now;
  }
}

static void buildWifi() {
  scrWifi = subScreen(nullptr, nullptr);
  wifiListView = clearBox(scrWifi, 0, 0, SCREEN_W, SCREEN_H);
  // back button only when there's somewhere to go back to (showWifiScreen); the title starts after it either way
  wifiBackBtn = roundBtn(wifiListView, T_PAD_X, 14, 48, LUCIDE_CHEVRON_LEFT, onWifiBack);
  lv_obj_t *title = mkLabel(wifiListView, "Connect to Wi-Fi", F_T26, C_TEXT);
  lv_obj_set_pos(title, 88, 16);
  wifiStatus = mkLabel(wifiListView, "", F_S, C_MUTED);
  lv_obj_set_pos(wifiStatus, 88, 52);
  textBtn(wifiListView, SCREEN_W - T_PAD_X - 132, 14, 132, 52, LUCIDE_REFRESH_CW "  Scan", I_20, T->primary, onRescan);
  wifiList = scrollCol(wifiListView, 32, 96, 736, 364, 10);

  wifiPwView = clearBox(scrWifi, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(wifiPwView, LV_OBJ_FLAG_HIDDEN);
  roundBtn(wifiPwView, T_PAD_X, 14, 48, LUCIDE_CHEVRON_LEFT, onPwBack);
  wifiSsidLbl = mkLabel(wifiPwView, "", F_T26, C_TEXT);
  oneLine(wifiSsidLbl, 640, false);
  lv_obj_set_pos(wifiSsidLbl, 88, 22);
  wifiTa = textField(wifiPwView, 20, 84, 560, 64, "Wi-Fi password", nullptr);
  lv_textarea_set_password_mode(wifiTa, true);
  textBtn(wifiPwView, 592, 84, 72, 64, LUCIDE_EYE, I_24, T->surfaceHi, onEye);
  textBtn(wifiPwView, 674, 84, 106, 64, "Join", F_M, T->primary, onConnectBtn);
  wifiPwStatus = mkLabel(wifiPwView, "", F_S, C_MUTED);
  lv_obj_set_pos(wifiPwStatus, 24, 158);
  wifiKb = keyboardFor(wifiPwView, SCREEN_W, 270, wifiTa, onKb);
}
