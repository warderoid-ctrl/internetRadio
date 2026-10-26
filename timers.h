#pragma once
// Kitchen timers (3 at once), buzzer beeps and the big "it's done" alert.

#define N_SLOTS 3
struct TimerSlot { bool active; bool ringing; uint32_t endMs; uint32_t totalMs; char label[24]; };
static TimerSlot slots[N_SLOTS];
static lv_obj_t *slotCard[N_SLOTS], *slotArc[N_SLOTS], *slotTime[N_SLOTS], *slotName[N_SLOTS];
static lv_obj_t *alarmOv, *alarmLbl;

struct Preset { const char *name; int secs; };
static const Preset PRESETS[] = {
  {"Tea", 180}, {"Soft egg", 360}, {"Hard egg", 600}, {"Pasta", 600},
  {"Rice", 720}, {"Steak rest", 300}, {"1 min", 60}, {"Pizza", 1200},
};

// ── buzzer beeps without blocking the UI (advanced by the 5 Hz tick) ──
static int beepLeft = 0;
static bool beepOn = false;
static void beep(int count) { beepLeft = count * 2; }
static void beepTick() {
  if (beepLeft > 0) { beepOn = !beepOn; stc(beepOn ? STC_BUZZ_ON : STC_BUZZ_OFF); beepLeft--; if (beepLeft == 0 && beepOn) { stc(STC_BUZZ_OFF); beepOn = false; } }
}

static void fmtMMSS(uint32_t ms, char *b, size_t n) {
  uint32_t s = (ms + 999) / 1000;
  if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  else snprintf(b, n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static int timerStart(int secs, const char *label) {
  for (int i = 0; i < N_SLOTS; i++) if (!slots[i].active) {
    slots[i].active = true; slots[i].ringing = false;
    slots[i].totalMs = (uint32_t)secs * 1000UL;
    slots[i].endMs = millis() + slots[i].totalMs;
    strlcpy(slots[i].label, label && label[0] ? label : "Timer", sizeof(slots[i].label));
    lv_label_set_text(slotName[i], slots[i].label);
    return i;
  }
  return -1;
}
static void timerClear(int i) { slots[i].active = false; slots[i].ringing = false; lv_label_set_text(slotName[i], "Free"); }

static void onPreset(lv_event_t *e) {
  const Preset &p = PRESETS[(int)(intptr_t)lv_event_get_user_data(e)];
  if (timerStart(p.secs, p.name) < 0) postUi(UM_TOAST, "All 3 timers are busy", C_WARN);
}
static void onSlotPlus(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (!slots[i].active) { timerStart(60, "Timer"); return; }
  slots[i].endMs += 60000; slots[i].totalMs += 60000;
}
static void onSlotStop(lv_event_t *e) { timerClear((int)(intptr_t)lv_event_get_user_data(e)); }
static void onAlarmDismiss(lv_event_t *) {
  for (int i = 0; i < N_SLOTS; i++) if (slots[i].ringing) timerClear(i);
  lv_obj_add_flag(alarmOv, LV_OBJ_FLAG_HIDDEN);
}

static void timersBuild(lv_obj_t *pg) {
  for (int i = 0; i < 8; i++) {
    int c = i % 4, r = i / 4;
    lv_obj_t *b = lv_button_create(pg);
    lv_obj_set_pos(b, 12 + c * 174, 4 + r * 64);
    lv_obj_set_size(b, 166, 56);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    char t[48]; snprintf(t, sizeof(t), "%s  %d:%02d", PRESETS[i].name, PRESETS[i].secs / 60, PRESETS[i].secs % 60);
    lv_obj_t *l = mkLabel(b, t, F_S, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onPreset, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  for (int i = 0; i < N_SLOTS; i++) {
    lv_obj_t *c = card(pg, 12 + i * 232, 136, 224, 276);
    slotCard[i] = c;
    lv_obj_t *a = lv_arc_create(c);
    slotArc[i] = a;
    lv_obj_set_size(a, 160, 160);
    lv_obj_set_pos(a, 32, 10);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(C_CARD3), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    slotTime[i] = mkLabel(c, "--:--", F_L, C_MUTED);
    lv_obj_align(slotTime[i], LV_ALIGN_TOP_MID, 0, 74);
    slotName[i] = mkLabel(c, "Free", F_S, C_MUTED);
    lv_obj_set_width(slotName[i], 200);
    lv_obj_set_style_text_align(slotName[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(slotName[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(slotName[i], 12, 180);
    textBtn(c, 14, 216, 100, 48, "+1 min", F_S, C_CARD3, onSlotPlus, (void *)(intptr_t)i);
    textBtn(c, 122, 216, 88, 48, "Stop", F_S, C_CARD3, onSlotStop, (void *)(intptr_t)i);
  }
}

// full-screen alert, lives on the top layer so it shows over every page
static void alarmBuild() {
  alarmOv = lv_obj_create(lv_layer_top());
  lv_obj_set_size(alarmOv, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(alarmOv, 0, 0);
  lv_obj_set_style_bg_color(alarmOv, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(alarmOv, LV_OPA_80, 0);
  lv_obj_set_style_border_width(alarmOv, 0, 0);
  lv_obj_remove_flag(alarmOv, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alarmOv, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *c = panel(alarmOv, 120, 100, 560, 280, C_CARD, 36);
  lv_obj_set_style_border_width(c, 4, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT), 0);
  lv_obj_t *bell = mkLabel(c, LV_SYMBOL_BELL, F_XL, C_ACCENT);
  lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 22);
  alarmLbl = mkLabel(c, "Timer done!", F_L, C_TEXT);
  lv_obj_set_width(alarmLbl, 520);
  lv_obj_set_style_text_align(alarmLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(alarmLbl, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_t *b = textBtn(c, 130, 168, 300, 90, "DISMISS", F_L, C_ACCENT, onAlarmDismiss);
  lv_obj_set_style_bg_grad_color(b, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
}

// 5 Hz: update arcs, ring when done. Returns true if any timer is active (for the header chip)
static bool timersTick(char *chip, size_t chipN) {
  uint32_t now = millis();
  bool any = false; int32_t soonest = INT32_MAX; const char *soonName = "";
  static uint32_t lastRing = 0;
  bool anyRinging = false;
  for (int i = 0; i < N_SLOTS; i++) {
    TimerSlot &s = slots[i];
    if (!s.active) {
      lv_label_set_text(slotTime[i], "--:--");
      lv_obj_set_style_text_color(slotTime[i], lv_color_hex(C_MUTED), 0);
      lv_arc_set_value(slotArc[i], 0);
      continue;
    }
    int32_t left = (int32_t)(s.endMs - now);
    if (left <= 0 && !s.ringing) {
      s.ringing = true;
      char m[60]; snprintf(m, sizeof(m), "%s is done!", s.label);
      lv_label_set_text(alarmLbl, m);
      lv_obj_remove_flag(alarmOv, LV_OBJ_FLAG_HIDDEN);
    }
    if (s.ringing) {
      anyRinging = true;
      lv_label_set_text(slotTime[i], "DONE");
      lv_obj_set_style_text_color(slotTime[i], lv_color_hex(C_ACCENT), 0);
      lv_arc_set_value(slotArc[i], 1000);
      continue;
    }
    any = true;
    if (left < soonest) { soonest = left; soonName = s.label; }
    char b[12]; fmtMMSS((uint32_t)left, b, sizeof(b));
    lv_label_set_text(slotTime[i], b);
    lv_obj_set_style_text_color(slotTime[i], lv_color_hex(C_TEXT), 0);
    lv_arc_set_value(slotArc[i], (int)(1000L - (int64_t)left * 1000 / (int32_t)s.totalMs));
  }
  if (anyRinging && now - lastRing > 1800) { lastRing = now; beep(3); }
  if (any) { char b[12]; fmtMMSS((uint32_t)soonest, b, sizeof(b)); snprintf(chip, chipN, LV_SYMBOL_BELL " %s", b); (void)soonName; }
  return any;
}

static void timersSentence(char *out, size_t n) {
  uint32_t now = millis(); int cnt = 0; int32_t best = INT32_MAX; const char *nm = "";
  for (int i = 0; i < N_SLOTS; i++) if (slots[i].active && !slots[i].ringing) { cnt++; int32_t l = (int32_t)(slots[i].endMs - now); if (l < best) { best = l; nm = slots[i].label; } }
  if (!cnt) { snprintf(out, n, "You have no timers running."); return; }
  int m = best / 60000, s = (best / 1000) % 60;
  snprintf(out, n, "Your %s timer has %d minutes %d seconds left.", nm, m, s);
}
