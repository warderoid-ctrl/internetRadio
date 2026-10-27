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

// ───────────── resume: episode positions and "was it playing" survive a reboot (NVS) ─────────────
// "eppos": where the last EP_RESUME_N part-played episodes got to, newest first; "playing": whether something was
// playing (so a power cut brings the radio back, but a radio you stopped stays quiet).
#define EP_RESUME_N 16
struct EpPos { uint32_t h, pos, dur; };                       // h = artHash(url)
static EpPos epPos[EP_RESUME_N];
static uint32_t resumeSeekTo = 0;                             // jump here once the episode has started (0 = no jump)
static void epPosLoad() { if (prefs.getBytesLength("eppos") == sizeof(epPos)) prefs.getBytes("eppos", epPos, sizeof(epPos)); }
static uint32_t epPosGet(const char *url, uint32_t *dur = nullptr) {
  uint32_t h = artHash(url);
  for (auto &e : epPos) if (e.h == h && e.pos) { if (dur) *dur = e.dur; return e.pos; }
  return 0;
}
// remember an episode's position; one that's (nearly) finished or barely started is forgotten instead
static void epPosSave(const char *url, uint32_t pos, uint32_t dur) {
  uint32_t h = artHash(url);
  bool keep = pos >= 30 && (!dur || pos + 60 < dur);
  int i = 0;
  while (i < EP_RESUME_N && epPos[i].h != h) i++;
  if (i == EP_RESUME_N) { if (!keep) return; i = EP_RESUME_N - 1; }   // new one: replaces the oldest
  memmove(&epPos[1], &epPos[0], sizeof(EpPos) * i);           // take its old slot out, everything above moves down
  if (keep) epPos[0] = {h, pos, dur};
  else { memmove(&epPos[0], &epPos[1], sizeof(EpPos) * (EP_RESUME_N - 1)); epPos[EP_RESUME_N - 1] = {0, 0, 0}; }
  prefs.putBytes("eppos", epPos, sizeof(epPos));
}
static void epPosSaveNow() { if (curSt.kind == 1 && posSec) epPosSave(curSt.url, posSec, durSec); }
static void setWasPlaying(bool on) {
  static int8_t last = -1;
  if (last != on) { last = on; prefs.putUChar("playing", on); }
}

static void npArtUpdate() {                                   // swap the now-playing logo (not part of any card event)
  if (!npCard) return;
  if (npArt) lv_obj_delete(npArt);
  npArt = artImage(npCard, 24, 24, curSt.logo);
}
static void favBtnUpdate() {
  if (lblFav) lv_label_set_text(lblFav, curStation >= 0 ? LUCIDE_HEART "  Favourite" : LUCIDE_PLUS "  Favourite");
}

// play any station: a favourite, a recent, a search result or a podcast episode
static void playStationData(const Station &s) {
  if (!s.url[0]) return;
  if (strcmp(s.url, curSt.url)) epPosSaveNow();               // leaving an episode: keep its place
  else if (curSt.kind == 1 && posSec) epPosSaveNow();          // same episode again: carry on from here
  resumeSeekTo = s.kind == 1 ? epPosGet(s.url) : 0;
  setWasPlaying(true);
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
static void radioStop() {
  epPosSaveNow();
  setWasPlaying(false);
  playState = 0; wantPlaying = false; resumeStation = -1; audioCmd(AC_STOP);
}
static void setVolume(int v) {
  volume = constrain(v, 0, 21);
  audioCmd(AC_VOLUME, volume);
  if (sldVol) lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  prefs.putUChar("vol", (uint8_t)volume);
}

static void onFavBtn(lv_event_t *) { favToggle(curSt); }
static void onNext(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, 30); else playStation(curStation + 1); }
static void onPrev(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, -15); else playStation(curStation - 1); }
static void onSeek(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, (int)lv_slider_get_value(sldSeek) - (int)posSec); }
static void fmtTime(char *b, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
// live radio: prev / next station; podcast episode: back 15 s / on 30 s and a draggable time bar
static void transportUpdate() {
  if (!lblPrev) return;
  bool ep = (curSt.kind == 1);
  lv_label_set_text(lblPrev, ep ? LUCIDE_ROTATE_CCW " 15" : LUCIDE_SKIP_BACK);
  lv_label_set_text(lblNext, ep ? "30 " LUCIDE_ROTATE_CW : LUCIDE_SKIP_FORWARD);
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
  if (mins[idx]) { sleepAt = millis() + (uint32_t)mins[idx] * 60000UL; lv_label_set_text_fmt(lblSleep, LUCIDE_MOON " %dm", mins[idx]); }
  else { sleepAt = 0; lv_label_set_text(lblSleep, LUCIDE_MOON); }
}

// ───────────── carousel ─────────────
// cards carry what they play in user_data: fav index, or CARD_RECENT + recent index (others: CARD_NONE)
#define CARD_NONE   (-1)
#define CARD_RECENT 1000
static lv_obj_t *carouselCard(int w, uint32_t bg) {
  lv_obj_t *b = lv_button_create(carousel);
  lv_obj_set_size(b, w, 132);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 16, 0);
  pressStyle(b, T_R_TILE);
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
    bool on = !strcmp(st->url, curSt.url);                   // playing: tertiary fill, like Home's station tile
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? T->tertiary : T->surface), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? T->onTertiary : T->text), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), lv_color_hex(on ? T->onTertiary : T->muted), 0);
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
  lv_obj_t *n = mkLabel(b, st.name, F_M, C_TEXT);
  lv_obj_set_size(n, 106, 60);
  lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
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
      lv_obj_set_size(h, 76, 132);
      lv_obj_set_user_data(h, (void *)(intptr_t)CARD_NONE);
      lv_obj_t *l = mkLabel(h, LUCIDE_HISTORY "\nRecent", I_20, C_MUTED);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(l);
    }
    stationCard(recents[i], CARD_RECENT + i);
  }
  lv_obj_t *f = carouselCard(170, T->primary);
  lv_obj_set_user_data(f, (void *)(intptr_t)CARD_NONE);
  lv_obj_t *fl = mkLabel(f, LUCIDE_SEARCH "\nFind\nstations", I_24, T->onPrimary);
  lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(fl);
  lv_obj_add_event_cb(f, onFindStations, LV_EVENT_CLICKED, nullptr);
  lv_obj_update_layout(carousel);
  carouselHighlight();
}

// Tonal slider: 8 px track (selected), primary fill, a 6 x 28 handle (primary in dark, text in light) with a big
// invisible touch area, so it's still easy with floury fingers
static lv_obj_t *mkSlider(lv_obj_t *parent, int x, int y, int w, int lo, int hi, int val) {
  lv_obj_t *s = lv_slider_create(parent);
  lv_slider_set_range(s, lo, hi);
  lv_slider_set_value(s, val, LV_ANIM_OFF);
  lv_obj_set_size(s, w, 8);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_style_bg_color(s, lv_color_hex(T->selected), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(T->primary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(themeMode == THEME_MODE_DARK ? T->primary : T->text), LV_PART_KNOB);
  lv_obj_set_style_radius(s, 3, LV_PART_KNOB);
  lv_obj_set_style_pad_hor(s, -1, LV_PART_KNOB);              // 8 - 2 = 6 px wide
  lv_obj_set_style_pad_ver(s, 10, LV_PART_KNOB);              // 8 + 20 = 28 px tall
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  lv_obj_set_ext_click_area(s, 20);
  return s;
}
static void radioBuild(lv_obj_t *pg) {
  pg = pageBody(pg, "Radio");
  carousel = lv_obj_create(pg);
  lv_obj_remove_style_all(carousel);
  lv_obj_set_pos(carousel, 0, 0);
  lv_obj_set_size(carousel, PAGE_W, 140);
  lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(carousel, T_GAP, 0);
  lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *c = container(pg, 0, 152, PAGE_W, PAGE_H - 152, T->surface);
  npCard = c;
  npArt = artImage(c, 24, 24, curSt.logo);
  lv_obj_t *cap = mkLabel(c, "NOW PLAYING", F_XS, C_ACCENT);
  lv_obj_set_style_text_letter_space(cap, 2, 0);
  lv_obj_set_pos(cap, 104, 22);
  lblStation = mkLabel(c, curSt.name, F_T22, C_TEXT);
  oneLine(lblStation, 288, true);
  lv_obj_set_pos(lblStation, 104, 40);
  lblTitle = mkLabel(c, curSt.tag, F_S, C_MUTED);
  oneLine(lblTitle, 288, true);
  lv_obj_set_pos(lblTitle, 104, 72);
  lblStatus = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_pos(lblStatus, 24, 104);

  lblSleep = btnLabel(textBtn(c, 400, 24, 96, 48, LUCIDE_MOON, I_20, T->surfaceHi, onSleep));   // sleep timer: tap to cycle
  textBtn(c, 504, 24, 56, 48, LUCIDE_PENCIL, I_20, T->surfaceHi, onRenameBtn);
  lblFav = btnLabel(textBtn(c, 568, 24, 160, 48, "", I_20, T->surfaceHi, onFavBtn));
  favBtnUpdate();

  lblPrev = btnLabel(textBtn(c, 24, 144, 96, 64, LUCIDE_SKIP_BACK, I_24, T->surfaceHi, onPrev));
  lv_obj_t *play = textBtn(c, 128, 140, 120, 72, LUCIDE_PLAY, I_28, T->primary, onPlayPause);
  pressStyle(play, 22);
  lblPlay = btnLabel(play);
  lblNext = btnLabel(textBtn(c, 256, 144, 96, 64, LUCIDE_SKIP_FORWARD, I_24, T->surfaceHi, onNext));

  lblTime = mkLabel(c, "", F_XS, C_MUTED);
  lv_obj_set_pos(lblTime, 384, 128);
  sldSeek = mkSlider(c, 384, 156, 344, 0, 1, 0);
  lv_obj_add_event_cb(sldSeek, onSeek, LV_EVENT_RELEASED, nullptr);
  transportUpdate();

  lv_obj_t *vlo = mkLabel(c, LUCIDE_VOLUME_1, I_20, C_MUTED);
  lv_obj_set_pos(vlo, 384, 188);
  lv_obj_t *vhi = mkLabel(c, LUCIDE_VOLUME_2, I_20, C_MUTED);
  lv_obj_set_pos(vhi, 704, 188);
  sldVol = mkSlider(c, 420, 196, 268, 0, 21, volume);
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
  else if (audioMode == 1)    { st = "Speaking"; sc = C_ACCENT; }
  else switch (playState) {
    case 1:  st = "Connecting to stream..."; sc = C_WARN; break;
    case 2:  st = curSt.kind == 1 ? "Playing" : "Playing \xC2\xB7 Live"; sc = C_GOOD; break;
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
  // episodes: jump to where we left off once it's playing and its length is known; keep the place every 30 s;
  // forget it once it has played to the end
  if (curSt.kind == 1) {
    if (resumeSeekTo && playState == 2 && durSec && posSec < resumeSeekTo) {
      audioCmd(AC_SEEK, (int)(resumeSeekTo - posSec));
      char t[12], m[48]; fmtTime(t, sizeof(t), resumeSeekTo);
      snprintf(m, sizeof(m), "Picking up at %s", t);
      postUi(UM_TOAST, m, C_MUTED);
      resumeSeekTo = 0;
    }
    static uint32_t lastSave = 0, savedPos = 0;
    if (playState == 2 && !resumeSeekTo && millis() - lastSave > 30000 && posSec >= savedPos + 10) {
      lastSave = millis(); savedPos = posSec;
      epPosSave(curSt.url, posSec, durSec);
    }
    if (posSec < savedPos) savedPos = posSec;                  // jumped back / another episode
    static uint8_t lastState = 0;
    if (playState == 4 && lastState != 4) { epPosSave(curSt.url, durSec, durSec); setWasPlaying(false); }
    lastState = playState;
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
  if (playIcon != lastPlayIcon) { lastPlayIcon = playIcon; lv_label_set_text(lblPlay, playIcon ? LUCIDE_PAUSE : LUCIDE_PLAY); }

  if (sleepAt && (int32_t)(millis() - sleepAt) >= 0) {
    sleepAt = 0;
    radioStop();
    lv_label_set_text(lblSleep, LUCIDE_MOON);
    postUi(UM_TOAST, "Sleep timer - radio off. Night night!", C_ACCENT);
  }
}
