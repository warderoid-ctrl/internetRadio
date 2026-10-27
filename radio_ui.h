#pragma once
// Radio page: carousel of favourites + recently played, the now-playing card (logo, name, title, transport, volume,
// sleep, rename, favourite). UI task only.

static lv_obj_t *lblStation, *lblTitle, *lblStatus, *lblPlay, *sldVol, *lblSleep, *lblFav, *carousel;
static lv_obj_t *lblPrev, *lblNext, *sldSeek, *lblTime;      // episodes: -30 / +30 s and a time bar
static lv_obj_t *npCard, *npArt;                              // now-playing card and its logo
static uint32_t sleepAt = 0;                                  // millis() when the sleep timer fires (0 = off)
static void carouselRebuild();
static void carouselHighlight();
static void transportUpdate();
static void onFindStations(lv_event_t *);                     // station search screen (ui_find.h)
static void onRenameBtn(lv_event_t *);                        // rename dialog (ui_rename.h)

static void npArtUpdate() {                                   // swap the now-playing logo (not part of any card event)
  if (!npCard) return;
  if (npArt) lv_obj_delete(npArt);
  npArt = artImage(npCard, 24, 16, curSt.logo);
}
static void favBtnUpdate() {
  if (lblFav) lv_label_set_text(lblFav, curStation >= 0 ? LV_SYMBOL_OK "  Favourite" : LV_SYMBOL_PLUS "  Favourite");
}

// play any station: a favourite, a recent, a search result or a podcast episode
static void playStationData(const Station &s) {
  if (!s.url[0]) return;
  setCurrent(s);
  resumeStation = -1;
  lv_label_set_text(lblStation, curSt.name);
  lv_label_set_text(lblTitle, curSt.tag);
  favBtnUpdate();
  transportUpdate();
  npArtUpdate();
  recentPush(curSt);
  lv_async_call([](void *) { carouselRebuild(); }, nullptr); // after the current event: we may be inside a card's click
  setNowPlayingText("");
  playState = 1; wantPlaying = true;
  reqNowPlaying = true;
  audioCmd(AC_STATION, curSt.kind, curSt.url);
}
static void playStation(int i) {                             // favourite i, wrapping round
  if (nFavs == 0) return;
  if (i < 0) i = nFavs - 1;
  if (i >= nFavs) i = 0;
  Station s = favs[i];
  playStationData(s);
}
static void playCurrent() { if (curSt.url[0]) playStationData(curSt); else playStation(0); }
static bool favToggle(const Station &s) {                     // add / remove a favourite; true if it is one afterwards
  if (!favToggleData(s)) return false;
  favBtnUpdate();
  carouselRebuild();
  return favFind(s.url) >= 0;
}
static void stationRename(const char *name) {
  if (!name[0]) return;
  renameData(name);
  carouselRebuild();
  lv_label_set_text(lblStation, curSt.name);
}
static void radioStop() { playState = 0; wantPlaying = false; resumeStation = -1; audioCmd(AC_STOP); }
static void setVolume(int v) {
  volume = constrain(v, 0, 21);
  audioCmd(AC_VOLUME, volume);
  if (sldVol) lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  prefs.putUChar("vol", (uint8_t)volume);
}

static void onFavBtn(lv_event_t *) { favToggle(curSt); }
static void onNext(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, 30); else playStation(curStation + 1); }
static void onPrev(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, -30); else playStation(curStation - 1); }
static void onSeek(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, (int)lv_slider_get_value(sldSeek) - (int)posSec); }
static void fmtTime(char *b, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
// live radio: prev / next station; podcast episode: -30 s / +30 s and a draggable time bar
static void transportUpdate() {
  if (!lblPrev) return;
  bool ep = (curSt.kind == 1);
  lv_label_set_text(lblPrev, ep ? "-30" : LV_SYMBOL_PREV);
  lv_label_set_text(lblNext, ep ? "+30" : LV_SYMBOL_NEXT);
  lv_obj_set_style_text_font(lblPrev, ep ? F_M : F_L, 0);
  lv_obj_set_style_text_font(lblNext, ep ? F_M : F_L, 0);
  if (ep) { lv_obj_remove_flag(sldSeek, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(lblTime, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_add_flag(sldSeek, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(lblTime, LV_OBJ_FLAG_HIDDEN); }
}
static void onPlayPause(lv_event_t *) {
  if (playState == 0 || playState == 3 || playState == 4) playCurrent(); else radioStop();
}
static void onVolume(lv_event_t *e) {
  volume = lv_slider_get_value(sldVol);
  audioCmd(AC_VOLUME, volume);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs.putUChar("vol", (uint8_t)volume);
}
static void onSleep(lv_event_t *) {
  static const int mins[] = {0, 15, 30, 60};
  static int idx = 0;
  idx = (idx + 1) % 4;
  if (mins[idx]) { sleepAt = millis() + (uint32_t)mins[idx] * 60000UL; lv_label_set_text_fmt(lblSleep, "%dm", mins[idx]); }
  else { sleepAt = 0; lv_label_set_text(lblSleep, LV_SYMBOL_BELL); }
}

// ───────────── carousel ─────────────
// cards carry what they play in user_data: fav index, or CARD_RECENT + recent index (others: CARD_NONE)
#define CARD_NONE   (-1)
#define CARD_RECENT 1000
static lv_obj_t *carouselCard(int w, uint32_t bg) {
  lv_obj_t *b = lv_button_create(carousel);
  lv_obj_set_size(b, w, 140);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 24, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 16, 0);
  return b;
}
static const Station *cardStation(lv_obj_t *b) {
  int k = (int)(intptr_t)lv_obj_get_user_data(b);
  if (k >= CARD_RECENT && k - CARD_RECENT < nRecent) return &recents[k - CARD_RECENT];
  if (k >= 0 && k < nFavs) return &favs[k];
  return nullptr;
}
// mark the playing station and bring it into view (child 1 of a card is its tag line)
static void carouselHighlight() {
  if (!carousel) return;
  lv_obj_t *sel = nullptr;
  for (int i = 0; i < (int)lv_obj_get_child_count(carousel); i++) {
    lv_obj_t *b = lv_obj_get_child(carousel, i);
    const Station *st = cardStation(b);
    if (!st) continue;
    bool on = !strcmp(st->url, curSt.url);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_CARD3 : C_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(b, on ? 4 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), lv_color_hex(on ? C_ACCENT : C_MUTED), 0);
    if (on && !sel) sel = b;
  }
  if (sel) lv_obj_scroll_to_view(sel, LV_ANIM_ON);
}
static void onCarouselCard(lv_event_t *e) {
  const Station *st = cardStation((lv_obj_t *)lv_event_get_current_target(e));
  if (!st) return;
  Station c = *st;                                            // playing reorders the recent list under st
  playStationData(c);
}
static lv_obj_t *stationCard(const Station &st, int key) {
  lv_obj_t *b = carouselCard(210, C_CARD);
  lv_obj_set_user_data(b, (void *)(intptr_t)key);
  lv_obj_t *n = mkLabel(b, st.name, F_S, C_TEXT);
  lv_obj_set_width(n, 106);
  lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_height(n, 64, 0);
  lv_obj_align(n, LV_ALIGN_TOP_LEFT, 72, 0);
  lv_obj_t *t = mkLabel(b, st.tag, F_XS, C_MUTED);
  oneLine(t, 178, false);
  lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  artImage(b, 0, 0, st.logo);
  lv_obj_add_event_cb(b, onCarouselCard, LV_EVENT_CLICKED, nullptr);
  return b;
}
// favourites as big swipeable cards, then recently played (not already favourites), then "Find stations".
// Never call directly from a carousel card's own event (use lv_async_call).
static void carouselRebuild() {
  if (!carousel) return;
  lv_obj_clean(carousel);
  for (int i = 0; i < nFavs; i++) stationCard(favs[i], i);
  bool header = false;
  for (int i = 0; i < nRecent; i++) {
    if (favFind(recents[i].url) >= 0) continue;
    if (!header) {                                            // small "Recent" marker between the two groups
      header = true;
      lv_obj_t *h = lv_obj_create(carousel);
      lv_obj_remove_style_all(h);
      lv_obj_set_size(h, 76, 140);
      lv_obj_set_user_data(h, (void *)(intptr_t)CARD_NONE);
      lv_obj_t *l = mkLabel(h, LV_SYMBOL_LOOP "\nRecent", F_XS, C_MUTED);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(l);
    }
    stationCard(recents[i], CARD_RECENT + i);
  }
  lv_obj_t *f = carouselCard(170, C_ACCENT);
  lv_obj_set_user_data(f, (void *)(intptr_t)CARD_NONE);
  lv_obj_set_style_bg_grad_color(f, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
  lv_obj_t *fl = mkLabel(f, LV_SYMBOL_PLUS "\nFind\nstations", F_M, C_TEXT);
  lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(fl);
  lv_obj_add_event_cb(f, onFindStations, LV_EVENT_CLICKED, nullptr);
  lv_obj_update_layout(carousel);
  carouselHighlight();
}

static lv_obj_t *mkSlider(lv_obj_t *parent, int x, int y, int w, int lo, int hi, int val, uint32_t ind, int knobPad) {
  lv_obj_t *s = lv_slider_create(parent);
  lv_slider_set_range(s, lo, hi);
  lv_slider_set_value(s, val, LV_ANIM_OFF);
  lv_obj_set_size(s, w, 14);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_CARD3), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(ind), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, knobPad, LV_PART_KNOB);         // fat knob: easy with floury fingers
  return s;
}
static void radioBuild(lv_obj_t *pg) {
  carousel = lv_obj_create(pg);
  lv_obj_remove_style_all(carousel);
  lv_obj_set_pos(carousel, 0, 4);
  lv_obj_set_size(carousel, PAGE_W, 150);
  lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(carousel, 12, 0);
  lv_obj_set_style_pad_hor(carousel, 12, 0);
  lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *c = card(pg, 12, 166, 688, 246);
  npCard = c;
  npArt = artImage(c, 24, 16, curSt.logo);
  lv_obj_t *cap = mkLabel(c, "NOW PLAYING", F_XS, C_ACCENT);
  lv_obj_set_style_text_letter_space(cap, 3, 0);
  lv_obj_set_pos(cap, 100, 14);
  lblStation = mkLabel(c, curSt.name, F_L, C_TEXT);
  oneLine(lblStation, 224, true);
  lv_obj_set_pos(lblStation, 100, 34);
  lblTitle = mkLabel(c, curSt.tag, F_M, C_MUTED);
  oneLine(lblTitle, 564, true);
  lv_obj_set_pos(lblTitle, 100, 74);
  lblStatus = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_pos(lblStatus, 24, 106);

  lblSleep = btnLabel(textBtn(c, 334, 18, 60, 52, LV_SYMBOL_BELL, F_S, C_CARD3, onSleep));   // sleep timer: tap to cycle
  textBtn(c, 404, 18, 60, 52, LV_SYMBOL_EDIT, F_M, C_CARD3, onRenameBtn);
  lblFav = btnLabel(textBtn(c, 474, 18, 190, 52, "", F_S, C_CARD3, onFavBtn));
  favBtnUpdate();

  lblPrev = btnLabel(textBtn(c, 24, 150, 76, 76, LV_SYMBOL_PREV, F_L, C_CARD3, onPrev));
  lv_obj_t *play = textBtn(c, 116, 138, 100, 100, LV_SYMBOL_PLAY, F_XL, C_ACCENT, onPlayPause);
  lblPlay = btnLabel(play);
  lv_obj_set_style_bg_grad_color(play, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(play, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(play, 40, 0);
  lv_obj_set_style_shadow_color(play, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(play, LV_OPA_40, 0);
  lblNext = btnLabel(textBtn(c, 232, 150, 76, 76, LV_SYMBOL_NEXT, F_L, C_CARD3, onNext));

  lblTime = mkLabel(c, "", F_XS, C_MUTED);
  lv_obj_set_pos(lblTime, 340, 136);
  sldSeek = mkSlider(c, 340, 166, 324, 0, 1, 0, C_ACCENT2, 10);
  lv_obj_add_event_cb(sldSeek, onSeek, LV_EVENT_RELEASED, nullptr);
  transportUpdate();

  lv_obj_t *vlo = mkLabel(c, LV_SYMBOL_VOLUME_MID, F_M, C_MUTED);
  lv_obj_set_pos(vlo, 340, 202);
  lv_obj_t *vhi = mkLabel(c, LV_SYMBOL_VOLUME_MAX, F_M, C_MUTED);
  lv_obj_set_pos(vhi, 638, 202);
  sldVol = mkSlider(c, 382, 208, 236, 0, 21, volume, C_ACCENT, 12);
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_RELEASED, nullptr);

  carouselRebuild();
}

// called from the 5 Hz tick. radioStatusText lets the home page mirror the status.
static const char *radioStatusText = "";
static void radioTick(bool netUp) {
  if (metaDirty && xSemaphoreTake(metaMx, 0) == pdTRUE) {
    char t[160];
    strlcpy(t, metaTitle, sizeof(t));
    metaDirty = false;
    xSemaphoreGive(metaMx);
    lv_label_set_text(lblTitle, t[0] ? t : curSt.tag);
  }
  const char *st; uint32_t sc = C_MUTED;
  if (!netUp)                 st = "Waiting for Wi-Fi...";
  else if (audioMode == 1)    { st = LV_SYMBOL_AUDIO " Speaking"; sc = C_ACCENT; }
  else switch (playState) {
    case 1:  st = "Connecting to stream..."; sc = C_WARN; break;
    case 2:  st = LV_SYMBOL_AUDIO " LIVE";   sc = C_GOOD; break;
    case 3:  st = "Stream problem - retrying..."; sc = C_BAD; break;
    case 4:  st = "Episode finished"; break;
    default: st = "Stopped"; break;
  }
  radioStatusText = st;
  static const char *lastSt = nullptr;
  if (st != lastSt) {
    lastSt = st;
    lv_label_set_text(lblStatus, st);
    lv_obj_set_style_text_color(lblStatus, lv_color_hex(sc), 0);
  }
  if (curSt.kind == 1 && sldSeek && !lv_obj_has_state(sldSeek, LV_STATE_PRESSED)) {
    static uint32_t lastPos = 1, lastDur = 1;
    uint32_t p = posSec, d = durSec;
    if (p != lastPos || d != lastDur) {
      lastPos = p; lastDur = d;
      lv_slider_set_range(sldSeek, 0, d ? (int32_t)d : 1);
      lv_slider_set_value(sldSeek, (int32_t)std::min(p, d ? d : 1), LV_ANIM_OFF);
      char a[12], b[12]; fmtTime(a, sizeof(a), p); fmtTime(b, sizeof(b), d);
      lv_label_set_text_fmt(lblTime, d ? "%s / %s" : "%s", a, b);
    }
  }
  // station logos: start a lookup when idle, apply a finished one
  if (logoStage == 2) {
    if (logoRes[0]) { logoApply(logoReqUrl, logoRes); carouselRebuild(); npArtUpdate(); }
    logoStage = 0;
  } else if (logoStage == 0 && netUp) logoPick();
  static bool lastPlayIcon = true;
  bool playIcon = (playState == 1 || playState == 2);
  if (playIcon != lastPlayIcon) { lastPlayIcon = playIcon; lv_label_set_text(lblPlay, playIcon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); }

  if (sleepAt && (int32_t)(millis() - sleepAt) >= 0) {
    sleepAt = 0;
    radioStop();
    lv_label_set_text(lblSleep, LV_SYMBOL_BELL);
    postUi(UM_TOAST, "Sleep timer - radio off. Night night!", C_ACCENT);
  }
}
