#pragma once
// Home page: clock, weather, next appointment, radio mini-player and quick actions.
// Part of the sketch: included from internetRadio.ino after the navigation helpers.

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
  oneLine(homeWxCond, 210, false);
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
  oneLine(homeNextT, 400, false);
  lv_obj_set_pos(homeNextT, 20, 34);
  homeNextW = mkLabel(nc, "", F_S, C_MUTED);
  lv_obj_set_pos(homeNextW, 20, 62);

  lv_obj_t *rc = card(pg, 464, 212, 236, 92);
  lv_obj_add_event_cb(rc, onHomeRadioTap, LV_EVENT_CLICKED, nullptr);
  homeStation = mkLabel(rc, curSt.name, F_S, C_TEXT);
  oneLine(homeStation, 140, false);
  lv_obj_set_pos(homeStation, 16, 18);
  homeStat = mkLabel(rc, "", F_XS, C_MUTED);
  oneLine(homeStat, 140, false);
  lv_obj_set_pos(homeStat, 16, 48);
  homePlay = btnLabel(textBtn(rc, 160, 14, 64, 64, LV_SYMBOL_PLAY, F_M, C_ACCENT, onPlayPause));

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
    else { lv_label_set_text(homeNextT, agendaConfigured ? "Nothing coming up" : "No calendar connected"); lv_label_set_text(homeNextW, agendaConfigured ? "Enjoy the quiet" : "Add one from your phone (More)"); }
  }
  if (strcmp(lv_label_get_text(homeStation), curSt.name)) lv_label_set_text(homeStation, curSt.name);   // only on change: no redraw
  static const char *lastS = nullptr;
  if (radioStatusText != lastS) { lastS = radioStatusText; lv_label_set_text(homeStat, radioStatusText); }
  static bool lastIcon = true;
  bool icon = (playState == 1 || playState == 2);
  if (icon != lastIcon) { lastIcon = icon; lv_label_set_text(homePlay, icon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); }
}
