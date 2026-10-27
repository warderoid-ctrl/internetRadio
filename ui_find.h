#pragma once
// Find stations: Search / Genres / Places tabs + Feeling lucky. Results (radio-browser.info) share one list:
// tap to play, + to keep as a favourite.
// Part of the sketch: included from internetRadio.ino after the navigation helpers.

static lv_obj_t *scrFind, *findTa, *findKb, *findList, *findStatus, *findSearchBox, *findGenres, *findPlaces;
static lv_obj_t *findTabBtn[3];
static int findTab = 0;
static bool luckyPending = false;
static int luckyGenre = 0;

static void findLayout(bool kb) {
  bool srch = (findTab == 0);
  for (int t = 0; t < 3; t++) btnFill(findTabBtn[t], t == findTab ? T->secondary : T->surfaceHi);
  if (srch) lv_obj_remove_flag(findSearchBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findSearchBox, LV_OBJ_FLAG_HIDDEN);
  if (findTab == 1) lv_obj_remove_flag(findGenres, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findGenres, LV_OBJ_FLAG_HIDDEN);
  if (findTab == 2) lv_obj_remove_flag(findPlaces, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(findPlaces, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(findStatus, 24, srch ? 158 : 244);
  lv_obj_set_pos(findList, 32, srch ? 190 : 270);
  lv_obj_set_height(findList, srch ? 280 : 200);
  kbToggle(findKb, findList, srch && kb);
}
static bool findNetOk() {
  if (netState == NET_UP) return true;
  lv_label_set_text(findStatus, "Connect to Wi-Fi first");
  return false;
}
static void findRequest(uint8_t req, const char *q, const char *what) {
  rbRequest(req, q);
  lv_obj_clean(findList);
  lv_label_set_text_fmt(findStatus, "Loading %s...", what);
}
static void findSearch() {
  const char *q = lv_textarea_get_text(findTa);
  if (!q[0] || !findNetOk()) return;
  findRequest(RB_SEARCH, q, "stations");
  findLayout(false);
}
static void findFavLabel(lv_obj_t *btn, const Station &s) {
  lv_label_set_text(btnLabel(btn), favFind(s.url) >= 0 ? LUCIDE_HEART : LUCIDE_PLUS);
}
static void onFindFav(lv_event_t *e) {
  const Station &s = rbHits[(intptr_t)lv_event_get_user_data(e)].s;
  bool was = favFind(s.url) >= 0;
  if (!was && nFavs >= MAX_FAVS) { uiToast("Favourites are full - remove one first", C_WARN); return; }
  favToggle(s);
  findFavLabel((lv_obj_t *)lv_event_get_target(e), s);
  uiToast(was ? "Removed from favourites" : "Added to favourites", was ? C_MUTED : C_GOOD);
}
static void findPlay(const Station &s) {
  playStationData(s);
  lv_screen_load(scrMainRef);
  showPage(PG_RADIO);
}
static void onFindPlay(lv_event_t *e) { findPlay(rbHits[(intptr_t)lv_event_get_user_data(e)].s); }

// highlight one chip / card in a row of them: secondary fill, like a selected chip (restyle only: runs inside their
// click event)
static void findMark(lv_obj_t *parent, int sel) {
  for (int i = 0; i < (int)lv_obj_get_child_count(parent); i++) {
    lv_obj_t *b = lv_obj_get_child(parent, i);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == sel ? T->secondary : T->surface), 0);
    for (int k = 0; k < (int)lv_obj_get_child_count(b); k++)
      lv_obj_set_style_text_color(lv_obj_get_child(b, k), lv_color_hex(i == sel ? T->onSecondary : (k ? T->muted : T->text)), 0);
  }
}
static void onGenre(lv_event_t *e) {
  int g = (int)(intptr_t)lv_event_get_user_data(e);
  findMark(findGenres, g);
  if (!findNetOk()) return;
  char w[48]; snprintf(w, sizeof(w), "%s stations", GENRES[g].name);
  findRequest(RB_BY_GENRE, GENRES[g].tag, w);
}
static void onPlace(lv_event_t *e) {
  int p = (int)(intptr_t)lv_event_get_user_data(e);
  findMark(findPlaces, p);
  if (!findNetOk()) return;
  char w[56]; snprintf(w, sizeof(w), "stations in %s", rbPlaces[p].name);
  findRequest(RB_BY_PLACE, rbPlaces[p].key, w);
}
static lv_obj_t *findChip(lv_obj_t *parent, int w, int h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(T->surface), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  pressStyle(b, h >= 100 ? T_R_TILE : LV_RADIUS_CIRCLE);    // country cards: tiles; genre chips: pills
  return b;
}
static void findPlacesBuild() {
  lv_obj_clean(findPlaces);
  for (int i = 0; i < nPlaces; i++) {
    lv_obj_t *b = findChip(findPlaces, 196, 140);
    lv_obj_set_style_pad_all(b, 14, 0);
    lv_obj_t *n = mkLabel(b, rbPlaces[i].name, F_M, C_TEXT);
    lv_obj_set_width(n, 168);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(n, 80, 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *c = mkLabel(b, "", F_XS, C_MUTED);
    lv_label_set_text_fmt(c, i == 0 ? "Home \xC2\xB7 %d stations" : "%d stations", rbPlaces[i].count);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b, onPlace, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}
static void onFindTab(lv_event_t *e) {
  findTab = (int)(intptr_t)lv_event_get_user_data(e);
  lv_obj_clean(findList);
  if (findTab == 0) lv_label_set_text(findStatus, "Search 50,000 stations worldwide");
  else if (findTab == 1) lv_label_set_text(findStatus, "Pick a genre (swipe for more)");
  else {
    if (nPlaces < 0 && findNetOk()) { nPlaces = -2; reqPlaces = true; }
    lv_label_set_text(findStatus, nPlaces > 0 ? "Pick a country (swipe for more)" : "Loading countries...");
  }
  findLayout(findTab == 0);
}
static void onLucky(lv_event_t *) {
  if (!findNetOk()) return;
  luckyGenre = esp_random() % N_GENRES;
  luckyPending = true;
  char w[48]; snprintf(w, sizeof(w), "something %s", GENRES[luckyGenre].name);
  findRequest(RB_BY_GENRE, GENRES[luckyGenre].tag, w);
  findLayout(false);
}

static void findTick(lv_timer_t *) {
  static uint8_t seenCat = 0;
  if (rbCatVer != seenCat) {                             // country list arrived
    seenCat = rbCatVer;
    if (nPlaces > 0) findPlacesBuild();
    if (findTab == 2) lv_label_set_text(findStatus, nPlaces > 0 ? "Pick a country (swipe for more)" : "Couldn't load countries - tap Places again");
  }
  if (!rbTake()) return;
  lv_obj_clean(findList);
  if (luckyPending) {                                    // Feeling lucky: play a random one of the genre's stations
    luckyPending = false;
    if (rbN > 0) {
      Station s = rbHits[esp_random() % rbN].s;
      char m[96]; snprintf(m, sizeof(m), "Feeling lucky: %s (%s)", s.name, GENRES[luckyGenre].name);
      findPlay(s);
      uiToast(m, C_ACCENT);
      return;
    }
  }
  if (rbN < 0) { lv_label_set_text(findStatus, "Couldn't load stations - check Wi-Fi and try again"); return; }
  if (rbN == 0) { lv_label_set_text(findStatus, "Nothing found - try another name, town or genre"); return; }
  lv_label_set_text_fmt(findStatus, "%d stations: tap to play, + to keep", rbN);
  for (int i = 0; i < rbN; i++) {
    lv_obj_t *row = listRow(findList, 736, 72);
    artImage(row, 0, 0, rbHits[i].s.logo);
    rowText(row, 74, 560, rbHits[i].s.name, rbHits[i].detail);
    lv_obj_add_event_cb(row, onFindPlay, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *fb = textBtn(row, 660, 8, 56, 48, "", I_20, T->surfaceHi, onFindFav, (void *)(intptr_t)i);
    findFavLabel(fb, rbHits[i].s);
  }
}
static void onFindBack(lv_event_t *) { lv_screen_load(scrMainRef); }
static void onFindSearchBtn(lv_event_t *) { findSearch(); }
static void onFindTa(lv_event_t *) { findLayout(true); }
static void onFindKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) findSearch();
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) findLayout(false);
}
static lv_obj_t *findStrip(int flow) {                   // horizontally swiping area under the tabs
  lv_obj_t *o = lv_obj_create(scrFind);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, 0, 84);
  lv_obj_set_size(o, SCREEN_W, 152);
  lv_obj_set_flex_flow(o, (lv_flex_flow_t)flow);
  lv_obj_set_style_pad_hor(o, 20, 0);
  lv_obj_set_style_pad_row(o, 8, 0);
  lv_obj_set_style_pad_column(o, 10, 0);
  lv_obj_set_scroll_dir(o, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
  return o;
}
static void findBuild() {
  scrFind = subScreen(nullptr, nullptr);
  roundBtn(scrFind, T_PAD_X, 14, 48, LUCIDE_CHEVRON_LEFT, onFindBack);
  static const char *tabs[3] = {LUCIDE_SEARCH " Search", LUCIDE_MUSIC " Genres", LUCIDE_GLOBE " Places"};
  for (int t = 0; t < 3; t++) findTabBtn[t] = textBtn(scrFind, 88 + t * 142, 12, 134, 52, tabs[t], I_20, T->surfaceHi, onFindTab, (void *)(intptr_t)t);
  textBtn(scrFind, 520, 12, SCREEN_W - T_PAD_X - 520, 52, LUCIDE_SHUFFLE "  Feeling lucky", I_20, T->primary, onLucky);

  findSearchBox = clearBox(scrFind, 0, 80, SCREEN_W, 74);
  findTa = textField(findSearchBox, 20, 4, 644, 64, "Name, town or genre: KEXP, BBC, Paris, jazz...", onFindTa);
  textBtn(findSearchBox, 674, 4, 106, 64, LUCIDE_SEARCH, I_24, T->primary, onFindSearchBtn);

  findGenres = findStrip(LV_FLEX_FLOW_COLUMN_WRAP);       // 3 rows of genre chips, swipe sideways for more
  for (int g = 0; g < N_GENRES; g++) {
    lv_obj_t *b = findChip(findGenres, 168, 44);
    lv_obj_t *l = mkLabel(b, GENRES[g].name, F_S, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onGenre, LV_EVENT_CLICKED, (void *)(intptr_t)g);
  }
  findPlaces = findStrip(LV_FLEX_FLOW_ROW);                // carousel of countries
  lv_obj_set_flex_align(findPlaces, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_snap_x(findPlaces, LV_SCROLL_SNAP_CENTER);

  findStatus = mkLabel(scrFind, "", F_S, C_MUTED);
  findList = scrollCol(scrFind, 32, 190, 736, 280, 8);
  findKb = keyboardFor(scrFind, SCREEN_W, 270, findTa, onFindKb);
  lv_timer_create(findTick, 200, nullptr);
}
static void onFindStations(lv_event_t *) {
  if (!scrFind) findBuild();
  findTab = 0;
  lv_textarea_set_text(findTa, "");
  lv_obj_clean(findList);
  lv_label_set_text(findStatus, "Search 50,000 stations worldwide");
  findLayout(true);
  lv_screen_load(scrFind);
}
