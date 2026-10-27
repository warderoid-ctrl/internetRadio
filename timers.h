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
  pg = pageBody(pg, "Timers");
  const int pw = (PAGE_W - 3 * 8) / 4;
  for (int i = 0; i < 8; i++) {                               // presets: outlined pills, primary while pressed
    int c = i % 4, r = i / 4;
    char t[48]; snprintf(t, sizeof(t), "%s  %d:%02d", PRESETS[i].name, PRESETS[i].secs / 60, PRESETS[i].secs % 60);
    lv_obj_t *b = textBtn(pg, c * (pw + 8), r * 60, pw, 52, t, F_M, T->bg, onPreset, (void *)(intptr_t)i);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(T->outline), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(T->primary), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btnLabel(b), lv_color_hex(T->onPrimary), LV_STATE_PRESSED);
  }
  const int cw = (PAGE_W - 2 * T_GAP) / N_SLOTS, cy = 124, ch = PAGE_H - cy;
  for (int i = 0; i < N_SLOTS; i++) {
    lv_obj_t *c = container(pg, i * (cw + T_GAP), cy, cw, ch, T->surface);
    slotCard[i] = c;
    lv_obj_t *a = lv_arc_create(c);
    slotArc[i] = a;
    lv_obj_set_size(a, 150, 150);
    lv_obj_set_pos(a, (cw - 150) / 2, 12);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(T->selected), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(T->primary), LV_PART_INDICATOR);
    slotTime[i] = mkLabel(c, "--:--", F_T26, C_MUTED);
    lv_obj_align(slotTime[i], LV_ALIGN_TOP_MID, 0, 72);
    slotName[i] = mkLabel(c, "Free", F_S, C_MUTED);
    lv_obj_set_width(slotName[i], cw - 24);
    lv_obj_set_style_text_align(slotName[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(slotName[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(slotName[i], 12, 172);
    int bw = (cw - 32 - 8) / 2;
    textBtn(c, 16, ch - 16 - 48, bw, 48, LUCIDE_PLUS " 1 min", I_20, T->surfaceHi, onSlotPlus, (void *)(intptr_t)i);
    textBtn(c, 24 + bw, ch - 16 - 48, bw, 48, LUCIDE_X " Stop", I_20, T->surfaceHi, onSlotStop, (void *)(intptr_t)i);
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
  lv_obj_t *c = container(alarmOv, 120, 100, 560, 280, T->primary);
  uint32_t on = T->onPrimary;
  lv_obj_t *bell = mkLabel(c, LUCIDE_ALARM_CLOCK, I_28, on);
  lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 28);
  alarmLbl = mkLabel(c, "Timer done!", F_T34, on);
  lv_obj_set_width(alarmLbl, 520);
  lv_obj_set_style_text_align(alarmLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(alarmLbl, LV_ALIGN_TOP_MID, 0, 82);
  lv_obj_t *b = textBtn(c, 130, 164, 300, 84, "Dismiss", F_T22, T->surface, onAlarmDismiss);   // the one action, on the card
  pressStyle(b, 22);
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
  if (any) { fmtMMSS((uint32_t)soonest, chip, chipN); (void)soonName; }   // callers add the timer icon
  return any;
}

static void timersSentence(char *out, size_t n) {
  uint32_t now = millis(); int cnt = 0; int32_t best = INT32_MAX; const char *nm = "";
  for (int i = 0; i < N_SLOTS; i++) if (slots[i].active && !slots[i].ringing) { cnt++; int32_t l = (int32_t)(slots[i].endMs - now); if (l < best) { best = l; nm = slots[i].label; } }
  if (!cnt) { snprintf(out, n, "You have no timers running."); return; }
  int m = best / 60000, s = (best / 1000) % 60;
  snprintf(out, n, "Your %s timer has %d minutes %d seconds left.", nm, m, s);
}
