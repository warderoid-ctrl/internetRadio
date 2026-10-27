#pragma once
// Weather location: type a town, pick from the matches (Open-Meteo geocoding); saved on the hub.
// Part of the sketch: included from internetRadio.ino.

static lv_obj_t *scrLoc, *locTa, *locKb, *locList, *locStatus;
static void locSearch() {
  const char *q = lv_textarea_get_text(locTa);
  if (!q[0]) return;
  if (netState != NET_UP) { lv_label_set_text(locStatus, "Connect to Wi-Fi first"); return; }
  geoRequest(q);
  lv_obj_clean(locList);
  lv_label_set_text(locStatus, "Searching...");
  kbToggle(locKb, locList, false);
}
static void onLocPick(lv_event_t *e) {
  const GeoHit &g = geoHits[(intptr_t)lv_event_get_user_data(e)];
  weatherSaveLocation(g.lat, g.lon, g.name, g.cc);
  lv_label_set_text(homeWxPlace, wxPlace);
  lv_label_set_text(wxLblPlace, wxPlace);
  lv_label_set_text_fmt(moreLocLbl, LV_SYMBOL_GPS "  Weather: %s", wxPlace);
  lv_screen_load(scrMainRef);
  char m[64]; snprintf(m, sizeof(m), "Weather: %s", wxPlace);
  uiToast(m, C_GOOD);
}
static void locTick(lv_timer_t *) {                      // show the places when netTask has them
  if (!geoTake()) return;
  lv_obj_clean(locList);
  if (geoN < 0) { lv_label_set_text(locStatus, "Search failed - check Wi-Fi and try again"); return; }
  if (geoN == 0) { lv_label_set_text(locStatus, "No places found - try another spelling"); return; }
  lv_label_set_text(locStatus, "Tap your town");
  for (int i = 0; i < geoN; i++) {
    lv_obj_t *row = listRow(locList, 736, 64);
    lv_obj_t *name = mkLabel(row, geoHits[i].name, F_M, C_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *det = mkLabel(row, geoHits[i].detail, F_S, C_MUTED);
    oneLine(det, 400, false);
    lv_obj_set_style_text_align(det, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(det, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(row, onLocPick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}
static void onLocBack(lv_event_t *) { lv_screen_load(scrMainRef); }
static void onLocSearchBtn(lv_event_t *) { locSearch(); }
static void onLocTa(lv_event_t *) { kbToggle(locKb, locList, true); }
static void onLocKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) locSearch();
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) kbToggle(locKb, locList, false);
}
static void locBuild() {
  scrLoc = subScreen(onLocBack, LV_SYMBOL_GPS "  Weather location");
  locTa = textField(scrLoc, 20, 84, 644, 64, "Your town, e.g. Leeds", onLocTa);
  textBtn(scrLoc, 674, 84, 106, 64, "Search", F_M, C_ACCENT, onLocSearchBtn);
  locStatus = mkLabel(scrLoc, "", F_S, C_MUTED);
  lv_obj_set_pos(locStatus, 24, 158);
  locList = scrollCol(scrLoc, 32, 190, 736, 280, 8);
  locKb = keyboardFor(scrLoc, SCREEN_W, 270, locTa, onLocKb);
  lv_timer_create(locTick, 200, nullptr);
}
static void onLocBtn(lv_event_t *) {
  if (!scrLoc) locBuild();
  lv_textarea_set_text(locTa, "");
  lv_obj_clean(locList);
  lv_label_set_text_fmt(locStatus, "Now: %s", wxPlace);
  kbToggle(locKb, locList, true);
  lv_screen_load(scrLoc);
}
