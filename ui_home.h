#pragma once
// Home / Today (Tonal): clock card with the next appointment, weather card, radio mini-player, and the bottom nav bar.
// Part of the sketch: included from internetRadio.ino after the navigation helpers.

static lv_obj_t *homeTimer, *homeWifi, *homeNext, *homeWxLine, *homeTile, *homePrev, *homeNext2;

static void onHomeNextTap(lv_event_t *) { showPage(PG_AGENDA); }
static void onHomeWxTap(lv_event_t *) { showPage(PG_WEATHER); }
static void onHomeRadioTap(lv_event_t *) { showPage(PG_RADIO); }
static void onHomeTimers(lv_event_t *) { showPage(PG_TIMERS); }
static void onNavPhotos(lv_event_t *) { ambientEnter(); }
static void onNav(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

// left column (388 wide: a 24 h "17:42" at 128 px needs it), right column the rest; nav bar along the bottom
#define HOME_COL1   388
#define HOME_COL2_X (T_PAD_X + HOME_COL1 + T_GAP)
#define HOME_COL2_W (SCREEN_W - T_PAD_X - HOME_COL2_X)
#define HOME_NAV_Y  (SCREEN_H - T_PAD_BOTTOM - T_NAV_H)
#define HOME_TOP_H  (HOME_NAV_Y - T_GAP - T_PAD_TOP)
#define HOME_HALF_H ((HOME_TOP_H - T_GAP) / 2)

static lv_obj_t *flexCol(lv_obj_t *parent, int gap) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, gap, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
  return c;
}

static void homeClockCard(lv_obj_t *pg) {
  uint32_t on = T->onPrimary;
  lv_obj_t *cc = container(pg, T_PAD_X, T_PAD_TOP, HOME_COL1, HOME_TOP_H, T->primary);
  homeDate = mkLabel(cc, "", F_M, on);
  lv_obj_set_pos(homeDate, 26, 28);
  // top right: running timer (tap for Timers) and, when offline, a Wi-Fi button
  homeWifi = textBtn(cc, HOME_COL1 - 26 - 44, 14, 44, 44, LUCIDE_WIFI_OFF, I_20, T->primary, onWifiBtn);
  tint(homeWifi, on);
  lv_obj_add_flag(homeWifi, LV_OBJ_FLAG_HIDDEN);
  homeTimer = textBtn(cc, HOME_COL1 - 26 - 128, 14, 128, 44, "", I_20, T->primary, onHomeTimers);
  tint(homeTimer, on);
  lv_obj_add_flag(homeTimer, LV_OBJ_FLAG_HIDDEN);

  homeClock = mkLabel(cc, "--:--", F_CLOCK, on);
  lv_obj_set_style_text_letter_space(homeClock, -6, 0);
  lv_obj_align(homeClock, LV_ALIGN_LEFT_MID, 20, -18);

  // next appointment: a tinted chip along the bottom
  lv_obj_t *nc = panel(cc, 20, HOME_TOP_H - 20 - 72, HOME_COL1 - 40, 72, on, 22);
  tint(nc, on);
  lv_obj_add_flag(nc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(nc, onHomeNextTap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *ic = mkLabel(nc, LUCIDE_CALENDAR, I_24, on);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
  homeNextW = mkLabel(nc, "", F_XS, on);
  lv_obj_set_style_text_opa(homeNextW, LV_OPA_80, 0);
  oneLine(homeNextW, HOME_COL1 - 40 - 68, false);
  lv_obj_set_pos(homeNextW, 52, 14);
  homeNextT = mkLabel(nc, "", F_M, on);
  oneLine(homeNextT, HOME_COL1 - 40 - 68, false);
  lv_obj_set_pos(homeNextT, 52, 34);
}

static void homeWeatherCard(lv_obj_t *pg) {
  uint32_t on = T->onSecondary;
  lv_obj_t *wc = container(pg, HOME_COL2_X, T_PAD_TOP, HOME_COL2_W, HOME_HALF_H, T->secondary);
  lv_obj_add_flag(wc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wc, onHomeWxTap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *col = flexCol(wc, 2);
  lv_obj_align(col, LV_ALIGN_LEFT_MID, 26, 0);
  homeWxPlace = mkLabel(col, "Outside now", F_S, on);
  lv_obj_set_style_text_opa(homeWxPlace, LV_OPA_80, 0);
  homeWxTemp = mkLabel(col, "--", F_HERO_S, on);
  lv_obj_set_style_text_letter_space(homeWxTemp, -4, 0);
  homeWxLine = mkLabel(col, "Fetching weather...", F_S, on);
  oneLine(homeWxLine, HOME_COL2_W - 26 - 130, false);
  lv_obj_t *circ = panel(wc, HOME_COL2_W - 20 - 104, (HOME_HALF_H - 104) / 2, 104, 104, on, LV_RADIUS_CIRCLE);
  tint(circ, on);
  lv_obj_add_flag(circ, LV_OBJ_FLAG_EVENT_BUBBLE);
  homeWxIcon = mkLabel(circ, LUCIDE_CLOUD_SUN, I_56, on);
  lv_obj_center(homeWxIcon);
}

static void homeRadioCard(lv_obj_t *pg) {
  int y = T_PAD_TOP + HOME_HALF_H + T_GAP, h = HOME_TOP_H - HOME_HALF_H - T_GAP;
  lv_obj_t *rc = container(pg, HOME_COL2_X, y, HOME_COL2_W, h, T->surface);
  lv_obj_add_flag(rc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(rc, onHomeRadioTap, LV_EVENT_CLICKED, nullptr);
  int tile = h - 32;
  lv_obj_t *t = panel(rc, 16, 16, tile, tile, T->tertiary, T_R_TILE);
  lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
  mkLabel(t, LUCIDE_RADIO, I_28, T->onTertiary);
  lv_obj_set_pos(lv_obj_get_child(t, 0), 12, 12);
  homeTile = mkLabel(t, curSt.name, F_T22, T->onTertiary);
  lv_obj_set_style_text_letter_space(homeTile, -1, 0);
  lv_obj_set_size(homeTile, tile - 24, 54);
  lv_label_set_long_mode(homeTile, LV_LABEL_LONG_DOT);
  lv_obj_align(homeTile, LV_ALIGN_BOTTOM_LEFT, 12, -10);

  int x = 16 + tile + 16, w = HOME_COL2_W - x - 16;
  homeStat = mkLabel(rc, "", F_S, C_MUTED);
  oneLine(homeStat, w, false);
  lv_obj_set_pos(homeStat, x, 22);
  homeStation = mkLabel(rc, curSt.name, F_L, C_TEXT);
  oneLine(homeStation, w, false);
  lv_obj_set_pos(homeStation, x, 44);
  int by = h - 16 - 48 - 4;
  homePrev = btnLabel(textBtn(rc, x, by, 56, 48, LUCIDE_SKIP_BACK, I_20, T->surfaceHi, onPrev));
  lv_obj_t *play = textBtn(rc, x + 64, by, w - 128, 48, LUCIDE_PLAY, I_24, T->primary, onPlayPause);
  pressStyle(play, 16);
  homePlay = btnLabel(play);
  homeNext2 = btnLabel(textBtn(rc, x + w - 56, by, 56, 48, LUCIDE_SKIP_FORWARD, I_20, T->surfaceHi, onNext));
}

// nav bar: Today (always the active pill here: the other destinations are full pages with a back button), the other
// destinations as icons, and the talk button at the right
static void homeNavBar(lv_obj_t *pg) {
  lv_obj_t *nav = panel(pg, T_PAD_X, HOME_NAV_Y, SCREEN_W - 2 * T_PAD_X, T_NAV_H, T->surface, T_NAV_H / 2);
  lv_obj_set_style_pad_all(nav, 8, 0);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(nav, 4, 0);

  lv_obj_t *today = textBtn(nav, 0, 0, LV_SIZE_CONTENT, 60, LUCIDE_GRID_2X2 "  Today", I_24, T->selected, nullptr);
  lv_obj_set_style_pad_hor(today, 22, 0);
  lv_obj_set_style_text_font(btnLabel(today), I_24, 0);
  struct { const char *icon; int page; } d[] = {
    {LUCIDE_RADIO, PG_RADIO}, {LUCIDE_PODCAST, PG_PODCASTS}, {LUCIDE_CLOUD_SUN, PG_WEATHER}, {LUCIDE_HOUSE, PG_HOUSE},
    {LUCIDE_TIMER, PG_TIMERS}, {LUCIDE_IMAGE, -1}, {LUCIDE_SETTINGS, PG_MORE}};
  for (auto &it : d) {
    lv_obj_t *b = textBtn(nav, 0, 0, 10, 60, it.icon, I_24, T->surface, it.page < 0 ? onNavPhotos : onNav,
                          (void *)(intptr_t)it.page);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_text_color(btnLabel(b), lv_color_hex(T->muted), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(T->selected), LV_STATE_PRESSED);
  }
  lv_obj_t *talk = textBtn(nav, 0, 0, 92, 60, LUCIDE_MIC, I_28, T->talk, onMicBtn);
  pressStyle(talk, 20);
}

static void homeBuild(lv_obj_t *pg) {
  homeClockCard(pg);
  homeWeatherCard(pg);
  homeRadioCard(pg);
  homeNavBar(pg);
}

// timer chip text from the 5 Hz tick (nullptr: no timer running)
static void homeTimerChip(const char *chip) {
  if (!chip) { lv_obj_add_flag(homeTimer, LV_OBJ_FLAG_HIDDEN); return; }
  char b[40]; snprintf(b, sizeof(b), LUCIDE_TIMER " %s", chip);
  if (strcmp(lv_label_get_text(btnLabel(homeTimer)), b)) lv_label_set_text(btnLabel(homeTimer), b);
  lv_obj_remove_flag(homeTimer, LV_OBJ_FLAG_HIDDEN);
}

static void homeTick(bool newSecond) {
  if (newSecond) {
    struct tm t;
    if (nowTm(&t)) {
      char b[8]; snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
      if (strcmp(lv_label_get_text(homeClock), b)) lv_label_set_text(homeClock, b);
      char d[40]; snprintf(d, sizeof(d), "%s %d %s", DOWF[t.tm_wday], t.tm_mday, MONF[t.tm_mon]);
      if (strcmp(lv_label_get_text(homeDate), d)) lv_label_set_text(homeDate, d);
    }
    bool off = (netState != NET_UP);
    if (off != !lv_obj_has_flag(homeWifi, LV_OBJ_FLAG_HIDDEN)) {
      if (off) lv_obj_remove_flag(homeWifi, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(homeWifi, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_x(homeTimer, HOME_COL1 - 26 - 128 - (off ? 52 : 0));
  }
  static uint32_t seenWx = 0, seenAg = 0;
  if (wxVer != seenWx) {
    seenWx = wxVer;
    DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
    if (w.valid) {
      wxDraw(homeWxIcon, wxKindOf(w.code), !w.day);
      lv_label_set_text_fmt(homeWxTemp, "%d\xC2\xB0", w.temp);
      lv_label_set_text_fmt(homeWxLine, "%s \xC2\xB7 H %d\xC2\xB0 L %d\xC2\xB0", wxText(w.code), w.days[0].hi, w.days[0].lo);
    }
  }
  static time_t lastNextStart = -1;
  Appt a; bool hasNext = agendaNext(a);
  time_t key = hasNext ? a.start : 0;
  if (agVer != seenAg || key != lastNextStart) {
    seenAg = agVer; lastNextStart = key;
    if (hasNext) { char w[48]; fmtWhen(a, w, sizeof(w)); lv_label_set_text(homeNextT, a.title); lv_label_set_text(homeNextW, w); }
    else { lv_label_set_text(homeNextT, agendaConfigured ? "Nothing coming up" : "No calendar connected"); lv_label_set_text(homeNextW, agendaConfigured ? "Enjoy the quiet" : "Add one from your phone (Settings)"); }
  }
  if (strcmp(lv_label_get_text(homeStation), curSt.name)) {   // only on change: no redraw
    lv_label_set_text(homeStation, curSt.name);
    lv_label_set_text(homeTile, curSt.name);
  }
  static const char *lastS = nullptr;
  if (radioStatusText != lastS) { lastS = radioStatusText; lv_label_set_text(homeStat, radioStatusText); }
  static bool lastIcon = true;
  bool icon = (playState == 1 || playState == 2);
  if (icon != lastIcon) { lastIcon = icon; lv_label_set_text(homePlay, icon ? LUCIDE_PAUSE : LUCIDE_PLAY); }
  static int lastKind = -1;                                   // episodes: the side buttons skip back 15 / on 30 s
  if (curSt.kind != lastKind) {
    lastKind = curSt.kind;
    lv_label_set_text(homePrev, curSt.kind == 1 ? LUCIDE_ROTATE_CCW : LUCIDE_SKIP_BACK);
    lv_label_set_text(homeNext2, curSt.kind == 1 ? LUCIDE_ROTATE_CW : LUCIDE_SKIP_FORWARD);
  }
}
