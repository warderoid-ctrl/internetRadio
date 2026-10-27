#pragma once
// Podcasts page (Tonal): the episode player on the left; on the right New (latest from followed shows) / Following /
// Find chips over one list (episodes, or shows: tap a show for its episodes).
// Part of the sketch: included from internetRadio.ino after the navigation helpers.

static lv_obj_t *pcChip[3], *pcList, *pcStatus, *pcSearchBox, *pcTa, *pcKb;
static lv_obj_t *pcShow, *pcTitle, *pcWhen, *pcTileName, *pcSeek, *pcPos, *pcLeft, *pcPlayLbl, *pcSleepLbl, *pcCtrls;
static int pcTab = 0;                                   // 0 New, 1 Following, 2 Find
static bool pcInShow = false;                           // showing one show's episodes (from Following / Find)
static char pcShowName[64];
static char pcNowWhen[24], pcNowUrl[256];               // publish date of the episode pcPlay() started
static void pcNewList();

#define PC_LEFT_W  (SCREEN_W - 2 * T_PAD_X - 300 - T_GAP)
#define PC_RIGHT_X (T_PAD_X + PC_LEFT_W + T_GAP)
#define PC_RIGHT_W 300
#define PC_CARD_H  (SCREEN_H - 2 * T_PAD_TOP)
#define PC_LIST_Y  (T_PAD_TOP + 44 + 8)

static void pcLayout(bool kb) {
  for (int t = 0; t < 3; t++) chipSel(pcChip[t], t == pcTab && !pcInShow);
  bool find = (pcTab == 2 && !pcInShow);
  if (find) lv_obj_remove_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN);
  int y = find ? 72 : 0;
  lv_obj_set_y(pcStatus, y + 14);
  lv_obj_set_pos(pcList, 8, y + 40);
  lv_obj_set_height(pcList, SCREEN_H - T_PAD_TOP - PC_LIST_Y - y - 48);
  if (find && kb) lv_obj_remove_flag(pcKb, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pcKb, LV_OBJ_FLAG_HIDDEN);
}
static bool pcNetOk() {
  if (netState == NET_UP) return true;
  lv_label_set_text(pcStatus, "Connect to Wi-Fi first");
  return false;
}
// a list row: title over a detail line; the episode in the player is the selected row
static lv_obj_t *pcRow(const char *title, const char *detail, int textW, bool dot, const char *url) {
  lv_obj_t *row = listRow(pcList, PC_RIGHT_W - 16, 68);
  lv_obj_set_style_pad_all(row, 0, 0);
  if (url && curSt.kind == 1 && !strcmp(url, curSt.url)) lv_obj_set_style_bg_color(row, lv_color_hex(T->selected), 0);
  int x = 14;
  if (dot) {                                            // unplayed
    lv_obj_t *d = panel(row, x, 18, 8, 8, C_ACCENT, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    x += 14;
  }
  lv_obj_t *n = mkLabel(row, title, F_M, C_TEXT);
  oneLine(n, textW - x, false);
  lv_obj_set_pos(n, x, 12);
  char more[120];                                       // part-played: how much is left
  uint32_t dur = 0, pos = url ? epPosGet(url, &dur) : 0;
  if (pos && dur > pos) { snprintf(more, sizeof(more), "%s \xC2\xB7 %u min left", detail, (unsigned)((dur - pos + 59) / 60)); detail = more; }
  lv_obj_t *d = mkLabel(row, detail, F_XS, C_MUTED);
  oneLine(d, textW - 14, false);
  lv_obj_set_pos(d, 14, 38);
  return row;
}

// ───────────── the player ─────────────
static void pcPlayerUpdate() {                          // an episode's station is (title, show, url): tag = the show
  bool ep = (curSt.kind == 1);
  lv_label_set_text(pcTitle, ep ? curSt.name : "Pick an episode");
  lv_label_set_text(pcShow, ep ? curSt.tag : "Nothing playing");
  lv_label_set_text(pcTileName, ep ? curSt.tag : "Podcasts");
  if (ep && !strcmp(pcNowUrl, curSt.url)) lv_label_set_text_fmt(pcWhen, "Published %s", pcNowWhen);
  else lv_label_set_text(pcWhen, ep ? "" : "New episodes from your shows are on the right");
  if (ep) lv_obj_remove_flag(pcCtrls, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pcCtrls, LV_OBJ_FLAG_HIDDEN);
}
static void pcPlay(const Episode &ep, const char *show, const char *art) {
  Station s;
  setStation(s, ep.title, show, ep.url);
  strlcpy(s.logo, art, sizeof(s.logo));
  s.kind = 1;
  strlcpy(pcNowWhen, ep.when, sizeof(pcNowWhen));
  strlcpy(pcNowUrl, s.url, sizeof(pcNowUrl));
  podMarkPlayed(ep.url);
  playStationData(s);
  pcPlayerUpdate();
}
static void onPcSeek(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, (int)lv_slider_get_value(pcSeek) - (int)posSec); }

static void pcPlayerBuild(lv_obj_t *pg) {
  lv_obj_t *c = container(pg, T_PAD_X, T_PAD_TOP, PC_LEFT_W, PC_CARD_H, T->surface);
  roundBtn(c, 20, 20, 48, LUCIDE_CHEVRON_LEFT, onPageBack);
  lv_obj_t *cap = mkLabel(c, "Podcasts", F_M, C_MUTED);
  lv_obj_set_pos(cap, 78, 34);

  lv_obj_t *tile = panel(c, 20, 84, 148, 148, T->secondary, 44);
  mkLabel(tile, LUCIDE_PODCAST, I_28, T->onSecondary);
  lv_obj_set_pos(lv_obj_get_child(tile, 0), 18, 18);
  pcTileName = mkLabel(tile, "", F_T22, T->onSecondary);
  lv_obj_set_size(pcTileName, 148 - 36, 56);
  lv_label_set_long_mode(pcTileName, LV_LABEL_LONG_DOT);
  lv_obj_align(pcTileName, LV_ALIGN_BOTTOM_LEFT, 18, -14);

  int tx = 188, tw = PC_LEFT_W - tx - 20;
  pcShow = mkLabel(c, "", F_S, C_MUTED);
  oneLine(pcShow, tw, false);
  lv_obj_set_pos(pcShow, tx, 92);
  pcTitle = mkLabel(c, "", F_T26, C_TEXT);
  lv_obj_set_size(pcTitle, tw, 96);                     // up to three lines
  lv_label_set_long_mode(pcTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(pcTitle, tx, 116);
  pcWhen = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_width(pcWhen, tw);
  lv_obj_set_pos(pcWhen, tx, 214);

  // progress + controls (hidden until an episode plays)
  int w = PC_LEFT_W - 40;
  pcCtrls = clearBox(c, 20, PC_CARD_H - 20 - 64 - 16 - 44, w, 44 + 16 + 64);
  pcSeek = mkSlider(pcCtrls, 0, 10, w, 0, 1, 0);
  lv_obj_add_event_cb(pcSeek, onPcSeek, LV_EVENT_RELEASED, nullptr);
  pcPos = mkLabel(pcCtrls, "0:00", F_XS, C_MUTED);
  lv_obj_set_pos(pcPos, 0, 26);
  pcLeft = mkLabel(pcCtrls, "", F_XS, C_MUTED);
  lv_obj_align(pcLeft, LV_ALIGN_TOP_RIGHT, 0, 26);
  int by = 44 + 16, pw = w - 92 - 92 - 64 - 3 * 8;
  textBtn(pcCtrls, 0, by, 92, 64, LUCIDE_ROTATE_CCW " 15", I_20, T->surfaceHi, onPrev);
  lv_obj_t *play = textBtn(pcCtrls, 100, by, pw, 64, LUCIDE_PLAY "  Play", I_24, T->primary, onPlayPause);
  pressStyle(play, 22);
  pcPlayLbl = btnLabel(play);
  textBtn(pcCtrls, 108 + pw, by, 92, 64, "30 " LUCIDE_ROTATE_CW, I_20, T->surfaceHi, onNext);
  pcSleepLbl = btnLabel(textBtn(pcCtrls, w - 64, by, 64, 64, LUCIDE_MOON, I_24, T->surfaceHi, onSleep));
  pcPlayerUpdate();
}
static void pcPlayerTick() {
  static bool lastIcon = false, first = true;
  bool icon = (playState == 1 || playState == 2);
  if (icon != lastIcon || first) { lastIcon = icon; first = false; lv_label_set_text(pcPlayLbl, icon ? LUCIDE_PAUSE "  Pause" : LUCIDE_PLAY "  Play"); }
  static uint32_t lastSleepMin = 1;
  uint32_t sm = sleepAt ? (sleepAt - millis()) / 60000 + 1 : 0;
  if (sm != lastSleepMin) {
    lastSleepMin = sm;
    if (sm) lv_label_set_text_fmt(pcSleepLbl, "%um", (unsigned)sm); else lv_label_set_text(pcSleepLbl, LUCIDE_MOON);
    lv_obj_set_style_text_font(pcSleepLbl, sm ? F_M : I_24, 0);
  }
  static char lastUrl[64] = "";
  if (strncmp(lastUrl, curSt.url, sizeof(lastUrl) - 1)) {   // something else started playing (radio, remote, voice)
    strlcpy(lastUrl, curSt.url, sizeof(lastUrl));
    pcPlayerUpdate();
  }
  if (curSt.kind != 1 || lv_obj_has_state(pcSeek, LV_STATE_PRESSED)) return;
  static uint32_t lastPos = 1, lastDur = 1;
  uint32_t p = posSec, d = durSec;
  if (p == lastPos && d == lastDur) return;
  lastPos = p; lastDur = d;
  lv_slider_set_range(pcSeek, 0, d ? (int32_t)d : 1);
  lv_slider_set_value(pcSeek, (int32_t)std::min(p, d ? d : 1), LV_ANIM_OFF);
  char a[12]; fmtTime(a, sizeof(a), p);
  lv_label_set_text(pcPos, a);
  fmtTime(a, sizeof(a), d > p ? d - p : 0);
  if (d) lv_label_set_text_fmt(pcLeft, "\xE2\x88\x92%s", a); else lv_label_set_text(pcLeft, "");   // "−1:13:09" left
}

// ───────────── the list ─────────────
static void onPcNewEp(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  DATA_LOCK();                                           // netTask replaces newEps under this lock
  bool ok = k < nNew;
  NewEp x;
  if (ok) x = newEps[k];
  DATA_UNLOCK();
  if (ok) { pcPlay(x.e, x.show, x.art); pcNewList(); }
}
static void pcNewList() {                               // "New": latest across followed shows
  lv_obj_clean(pcList);
  DATA_LOCK();
  for (int i = 0; i < nNew; i++) {
    char d[80]; snprintf(d, sizeof(d), "%s \xC2\xB7 %s", newEps[i].show, newEps[i].e.when);
    lv_obj_t *row = pcRow(newEps[i].e.title, d, PC_RIGHT_W - 16, !podIsPlayed(newEps[i].e.url), newEps[i].e.url);
    lv_obj_add_event_cb(row, onPcNewEp, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  int n = nNew;
  DATA_UNLOCK();
  if (newLoading && !n) lv_label_set_text(pcStatus, "Checking your shows...");
  else if (!n) lv_label_set_text(pcStatus, nPods ? "No episodes yet: tap New to check" : "Follow some shows in Find");
  else lv_label_set_text(pcStatus, newLoading ? "Latest from your shows (checking...)" : "Latest from your shows");
}
// rows carry an index: 0..99 followed shows, 100+ search results
static const Pod *pcPodOf(int k) { return k >= 100 ? &podHits[k - 100] : &pods[k]; }
static void pcOpenShow(const Pod &p) {
  if (!pcNetOk()) return;
  pcInShow = true;
  strlcpy(pcShowName, p.name, sizeof(pcShowName));
  podEpisodesRequest(p.feed);
  lv_obj_clean(pcList);
  lv_label_set_text_fmt(pcStatus, "%s: loading...", p.name);
  pcLayout(false);
}
static void onPcShow(lv_event_t *e) { Pod p = *pcPodOf((int)(intptr_t)lv_event_get_user_data(e)); pcOpenShow(p); }
static void onPcFollow(lv_event_t *e) {
  Pod p = *pcPodOf((int)(intptr_t)lv_event_get_user_data(e));
  bool was = podFind(p.feed) >= 0;
  if (!was && nPods >= MAX_PODS) { uiToast("Following too many shows - unfollow one first", C_WARN); return; }
  podToggle(p);
  lv_label_set_text(btnLabel((lv_obj_t *)lv_event_get_target(e)), was ? LUCIDE_PLUS : LUCIDE_CHECK);
  uiToast(was ? "Unfollowed" : "Following - new episodes show up in New", was ? C_MUTED : C_GOOD);
}
static void pcShowsList(bool results) {                 // followed shows, or search results
  lv_obj_clean(pcList);
  int n = results ? nPodHits : nPods;
  for (int i = 0; i < n; i++) {
    const Pod &p = results ? podHits[i] : pods[i];
    int k = results ? 100 + i : i;
    lv_obj_t *row = pcRow(p.name, p.author, PC_RIGHT_W - 16 - 56, false, nullptr);
    lv_obj_add_event_cb(row, onPcShow, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    textBtn(row, PC_RIGHT_W - 16 - 56, 12, 44, 44, podFind(p.feed) >= 0 ? LUCIDE_CHECK : LUCIDE_PLUS, I_20, T->surfaceHi,
            onPcFollow, (void *)(intptr_t)k);
  }
}
static void pcShowTab(int t) {
  pcTab = t;
  pcInShow = false;
  if (t == 0) pcNewList();
  else if (t == 1) { pcShowsList(false); lv_label_set_text(pcStatus, nPods ? "Tap a show for its episodes" : "Follow shows from Find"); }
  else { lv_obj_clean(pcList); lv_label_set_text(pcStatus, "Search podcasts (or a SoundCloud show)"); }
  pcLayout(t == 2);
}
static void onPcTab(lv_event_t *e) {
  int t = (int)(intptr_t)lv_event_get_user_data(e);
  if (t == 0 && pcTab == 0 && !pcInShow && netState == NET_UP) { reqNewEps = true; newLoading = true; }  // New again: check now
  pcShowTab(t);
}
static void onPcEpisode(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  if (k < nEps) { Episode ep = eps[k]; pcPlay(ep, pcShowName, epArt); }
}
static void pcEpisodesList() {
  lv_obj_clean(pcList);
  if (nEps < 0) { lv_label_set_text(pcStatus, "Couldn't load this show - try again"); return; }
  if (nEps == 0) { lv_label_set_text(pcStatus, "No playable episodes in this feed"); return; }
  lv_label_set_text(pcStatus, pcShowName);
  for (int i = 0; i < nEps; i++) {
    lv_obj_t *row = pcRow(eps[i].title, eps[i].when, PC_RIGHT_W - 16, !podIsPlayed(eps[i].url), eps[i].url);
    lv_obj_add_event_cb(row, onPcEpisode, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}
static void pcTick(lv_timer_t *) {
  pcPlayerTick();
  if (podEpisodesTake() && pcInShow) pcEpisodesList();
  if (podSearchTake() && !pcInShow && pcTab == 2) {
    if (nPodHits < 0) { lv_obj_clean(pcList); lv_label_set_text(pcStatus, "Search failed - check Wi-Fi and try again"); }
    else {
      pcShowsList(true);
      lv_label_set_text_fmt(pcStatus, nPodHits ? "%d shows: tap for episodes" : "No shows found", nPodHits);
    }
  }
  static uint8_t seenNew = 0;
  if (newVer != seenNew) {
    seenNew = newVer;
    if (pcTab == 0 && !pcInShow) pcNewList();
  }
}
static void pcSearch() {
  const char *q = lv_textarea_get_text(pcTa);
  if (!q[0] || !pcNetOk()) return;
  podSearchRequest(q);
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
  pcPlayerBuild(pg);

  lv_obj_t *chips = chipRow(pg, PC_RIGHT_X, T_PAD_TOP, PC_RIGHT_W);
  static const char *tabs[3] = {"New", "Following", LUCIDE_SEARCH " Find"};
  for (int t = 0; t < 3; t++) pcChip[t] = chip(chips, tabs[t], t == 0, onPcTab, (void *)(intptr_t)t);

  lv_obj_t *lc = panel(pg, PC_RIGHT_X, PC_LIST_Y, PC_RIGHT_W, SCREEN_H - T_PAD_TOP - PC_LIST_Y, T->surface, T_R_TILE);
  pcSearchBox = clearBox(lc, 8, 8, PC_RIGHT_W - 16, 60);
  pcTa = textField(pcSearchBox, 0, 0, PC_RIGHT_W - 16 - 68, 56, "Show, host or topic", onPcTa);
  textBtn(pcSearchBox, PC_RIGHT_W - 16 - 60, 0, 60, 56, LUCIDE_SEARCH, I_24, T->primary, onPcSearchBtn);
  pcStatus = mkLabel(lc, "", F_XS, C_MUTED);
  oneLine(pcStatus, PC_RIGHT_W - 32, false);
  lv_obj_set_x(pcStatus, 16);
  pcList = scrollCol(lc, 8, 40, PC_RIGHT_W - 16, 300, 4);
  // the keyboard covers the player while typing (the list stays in view, above it on the right)
  pcKb = keyboardFor(pg, PC_LEFT_W, 260, pcTa, onPcKb);
  lv_obj_align(pcKb, LV_ALIGN_BOTTOM_LEFT, T_PAD_X, -T_PAD_TOP);
  lv_timer_create(pcTick, 200, nullptr);
  pcShowTab(0);
}
