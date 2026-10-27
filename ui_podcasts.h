#pragma once
// Podcasts page: New (latest from followed shows) / Following / Find, and one show's episodes.
// Part of the sketch: included from internetRadio.ino after the navigation helpers.

static lv_obj_t *pcTabBtn[3], *pcList, *pcStatus, *pcSearchBox, *pcTa, *pcKb;
static int pcTab = 0;                                   // 0 New, 1 Following, 2 Find
static bool pcInShow = false;                           // showing one show's episodes (from Following / Find)
static char pcShowName[64];

static void pcLayout(bool kb) {
  for (int t = 0; t < 3; t++) btnFill(pcTabBtn[t], t == pcTab ? C_ACCENT : C_CARD3);
  bool find = (pcTab == 2 && !pcInShow);
  if (find) lv_obj_remove_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pcSearchBox, LV_OBJ_FLAG_HIDDEN);
  int y = find ? 150 : 90;
  lv_obj_set_pos(pcStatus, 16, y - 28);
  lv_obj_set_pos(pcList, 12, y);
  lv_obj_set_height(pcList, PAGE_H - 4 - y);
  kbToggle(pcKb, pcList, find && kb);
}
static bool pcNetOk() {
  if (netState == NET_UP) return true;
  lv_label_set_text(pcStatus, "Connect to Wi-Fi first");
  return false;
}
// a list row: cover, optional "unplayed" dot, title over a detail line
static lv_obj_t *pcRow(const char *title, const char *detail, int w, bool dot, const char *art) {
  lv_obj_t *row = listRow(pcList, 688, 72);
  artImage(row, 0, 0, art);
  int x = 74;
  if (dot) {
    lv_obj_t *d = panel(row, x, 14, 10, 10, C_ACCENT, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    x += 18;
  }
  rowText(row, x, w - x, title, detail);
  return row;
}
static void pcPlay(const Episode &ep, const char *show, const char *art) {
  Station s;
  setStation(s, ep.title, show, ep.url);
  strlcpy(s.logo, art, sizeof(s.logo));
  s.kind = 1;
  podMarkPlayed(ep.url);
  playStationData(s);
  showPage(PG_RADIO);
}
static void onPcNewEp(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  DATA_LOCK();                                           // netTask replaces newEps under this lock
  bool ok = k < nNew;
  NewEp x;
  if (ok) x = newEps[k];
  DATA_UNLOCK();
  if (ok) pcPlay(x.e, x.show, x.art);
}
static void pcNewList() {                               // "New": latest across followed shows
  lv_obj_clean(pcList);
  DATA_LOCK();
  for (int i = 0; i < nNew; i++) {
    char d[80]; snprintf(d, sizeof(d), "%s  -  %s", newEps[i].show, newEps[i].e.when);
    lv_obj_t *row = pcRow(newEps[i].e.title, d, 680, !podIsPlayed(newEps[i].e.url), newEps[i].art);
    lv_obj_add_event_cb(row, onPcNewEp, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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
  if (!pcNetOk()) return;
  pcInShow = true;
  strlcpy(pcShowName, p.name, sizeof(pcShowName));
  podEpisodesRequest(p.feed);
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
    lv_obj_t *row = pcRow(p.name, p.author, 600, false, p.art);
    lv_obj_add_event_cb(row, onPcShow, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    textBtn(row, 610, 9, 64, 46, podFind(p.feed) >= 0 ? LV_SYMBOL_OK : LV_SYMBOL_PLUS, F_M, C_CARD3, onPcFollow, (void *)(intptr_t)k);
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
  if (!pcNetOk()) return;
  reqNewEps = true;
  newLoading = true;
  pcShowTab(0);
}
static void onPcEpisode(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  if (k < nEps) { Episode ep = eps[k]; pcPlay(ep, pcShowName, epArt); }
}
static void pcTick(lv_timer_t *) {
  if (podEpisodesTake() && pcInShow) {
    lv_obj_clean(pcList);
    if (nEps < 0) lv_label_set_text(pcStatus, "Couldn't load this show - try again");
    else if (nEps == 0) lv_label_set_text(pcStatus, "No playable episodes in this feed");
    else {
      lv_label_set_text_fmt(pcStatus, "%s - latest %d episodes", pcShowName, nEps);
      for (int i = 0; i < nEps; i++) {
        lv_obj_t *row = pcRow(eps[i].title, eps[i].when, 680, !podIsPlayed(eps[i].url), epArt);
        lv_obj_add_event_cb(row, onPcEpisode, LV_EVENT_CLICKED, (void *)(intptr_t)i);
      }
    }
  }
  if (podSearchTake() && !pcInShow && pcTab == 2) {
    if (nPodHits < 0) { lv_obj_clean(pcList); lv_label_set_text(pcStatus, "Search failed - check Wi-Fi and try again"); }
    else {
      pcShowsList(true);
      lv_label_set_text_fmt(pcStatus, nPodHits ? "%d shows - tap for episodes, " LV_SYMBOL_PLUS " to follow" : "No shows found", nPodHits);
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
  static const char *tabs[3] = {"New", "Following", LV_SYMBOL_EDIT " Find"};
  for (int t = 0; t < 3; t++) pcTabBtn[t] = textBtn(pg, 12 + t * 172, 4, 164, 50, tabs[t], F_S, C_CARD3, onPcTab, (void *)(intptr_t)t);
  textBtn(pg, 628, 4, 72, 50, LV_SYMBOL_REFRESH, F_M, C_CARD3, onPcRefresh);
  pcSearchBox = clearBox(pg, 0, 60, PAGE_W, 64);
  pcTa = textField(pcSearchBox, 12, 2, 580, 58, "Show, host or topic", onPcTa);
  textBtn(pcSearchBox, 600, 2, 100, 58, "Search", F_M, C_ACCENT, onPcSearchBtn);
  pcStatus = mkLabel(pg, "", F_S, C_MUTED);
  pcList = scrollCol(pg, 12, 90, 688, 320, 8);
  pcKb = keyboardFor(pg, PAGE_W, 240, pcTa, onPcKb);
  lv_timer_create(pcTick, 200, nullptr);
  pcShowTab(0);
}
