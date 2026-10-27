#pragma once
// Home Assistant tiles: one batched template request polls every state; taps call a service.

#define N_TILES ((int)(sizeof(HA_TILES) / sizeof(HA_TILES[0])))
#define HA_ENABLED (HA_URL[0] != 0 && HA_TOKEN[0] != 0)

struct HaState { char state[24]; char unit[10]; bool known; };
static HaState haSt[sizeof(HA_TILES) / sizeof(HA_TILES[0])];
static volatile bool reqHa = false;
static volatile bool haReachable = false;
struct HaCall { char dom[24]; char svc[24]; char ent[72]; };
static QueueHandle_t haQ;

static bool strHas(const char *s, const char *sub) { return strstr(s, sub) != nullptr; }

static bool haPoll() {
  if (!HA_ENABLED) return false;
  String tpl = "{\"template\":\"";
  for (int i = 0; i < N_TILES; i++) {
    tpl += "{{states('"; tpl += HA_TILES[i].entity; tpl += "')}}~{{state_attr('"; tpl += HA_TILES[i].entity;
    tpl += "','unit_of_measurement') or ''}}|";
  }
  tpl += "\"}";
  String url = String(HA_URL) + "/api/template";
  String out;
  int code = httpPostJson(url.c_str(), tpl.c_str(), &out, HA_TOKEN, 6000);
  haReachable = (code == 200);
  if (code != 200) { Serial.printf("HA poll -> %d\n", code); haVer++; return false; }
  DATA_LOCK();
  int i = 0, pos = 0;
  while (i < N_TILES) {
    int bar = out.indexOf('|', pos);
    if (bar < 0) break;
    String seg = out.substring(pos, bar);
    int tl = seg.indexOf('~');
    String st = tl >= 0 ? seg.substring(0, tl) : seg;
    String un = tl >= 0 ? seg.substring(tl + 1) : "";
    st.trim(); un.trim();
    copyText(haSt[i].state, sizeof(haSt[i].state), st.c_str());
    copyText(haSt[i].unit, sizeof(haSt[i].unit), un.c_str());
    haSt[i].known = true;
    pos = bar + 1; i++;
  }
  DATA_UNLOCK();
  haVer++;
  return true;
}

static void haCallNow(const HaCall &c) {
  char url[200], body[120];
  snprintf(url, sizeof(url), "%s/api/services/%s/%s", HA_URL, c.dom, c.svc);
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", c.ent);
  String out;
  int code = httpPostJson(url, body, &out, HA_TOKEN, 6000);
  if (code != 200) { Serial.printf("HA call %s/%s -> %d\n", c.dom, c.svc, code); postUi(UM_TOAST, "Home Assistant didn't answer", C_BAD); }
  delay(250);
  reqHa = true;                       // re-poll to show the result
}

// queue a service call from anywhere (UI, voice, remote)
static void haService(const char *dom, const char *svc, const char *ent) {
  if (!HA_ENABLED || !haQ) return;
  HaCall c;
  strlcpy(c.dom, dom, sizeof(c.dom)); strlcpy(c.svc, svc, sizeof(c.svc)); strlcpy(c.ent, ent, sizeof(c.ent));
  xQueueSend(haQ, &c, 0);
}

static void haTapAction(const HaTile &t) {
  const char *e = t.entity;
  if (t.kind == 'T') haService("homeassistant", "toggle", e);
  else if (t.kind == 'B') {
    if (!strncmp(e, "scene.", 6)) haService("scene", "turn_on", e);
    else if (!strncmp(e, "script.", 7)) haService("script", "turn_on", e);
    else if (!strncmp(e, "automation.", 11)) haService("automation", "trigger", e);
    else if (!strncmp(e, "button.", 7)) haService("button", "press", e);
    else if (!strncmp(e, "input_button.", 13)) haService("input_button", "press", e);
    else haService("homeassistant", "turn_on", e);
  }
}

// ── page ──
static lv_obj_t *haGrid, *haTile[sizeof(HA_TILES) / sizeof(HA_TILES[0])], *haLblState[sizeof(HA_TILES) / sizeof(HA_TILES[0])];
static lv_obj_t *haStatus;

static bool haIsOn(const char *s) { return !strcmp(s, "on") || !strcmp(s, "open") || !strcmp(s, "unlocked") || !strcmp(s, "playing") || !strcmp(s, "home"); }

static void onHaTile(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  haTapAction(HA_TILES[i]);
  if (HA_TILES[i].kind == 'T') {                      // optimistic flip so it feels instant
    DATA_LOCK(); bool on = haIsOn(haSt[i].state); strlcpy(haSt[i].state, on ? "off" : "on", sizeof(haSt[i].state)); DATA_UNLOCK();
    haVer++;
  } else if (HA_TILES[i].kind == 'B') postUi(UM_TOAST, HA_TILES[i].label, C_GOOD);
}

// a Lucide icon for a tile, from its entity's domain (and name)
static const char *haIcon(const HaTile &t) {
  const char *e = t.entity;
  if (!strncmp(e, "light.", 6)) return LUCIDE_LIGHTBULB;
  if (!strncmp(e, "switch.", 7)) return LUCIDE_PLUG;
  if (!strncmp(e, "lock.", 5)) return LUCIDE_LOCK;
  if (!strncmp(e, "media_player.", 13)) return LUCIDE_TV;
  if (!strncmp(e, "fan.", 4)) return LUCIDE_FAN;
  if (strHas(e, "door") || strHas(e, "window")) return LUCIDE_DOOR_OPEN;
  if (strHas(e, "temp") || strHas(e, "climate")) return LUCIDE_THERMOMETER;
  if (strHas(e, "humid")) return LUCIDE_DROPLETS;
  if (t.kind == 'B') return LUCIDE_PLAY;
  return t.kind == 'T' ? LUCIDE_POWER : LUCIDE_ZAP;
}

static void houseBuild(lv_obj_t *pg) {
  pg = pageBody(pg, "Home");
  if (!HA_ENABLED) {
    lv_obj_t *c = card(pg, 0, 0, PAGE_W, 230);
    lv_obj_t *t = mkLabel(c, LUCIDE_HOUSE "  Home Assistant isn't connected yet", I_24, C_TEXT);
    lv_obj_set_pos(t, 24, 22);
    lv_obj_t *d = mkLabel(c, "1. In Home Assistant: your profile > Security > Long-lived access tokens > Create.\n"
                             "2. Copy secrets.example.h to secrets.h and fill HA_URL (e.g. http://homeassistant.local:8123) and HA_TOKEN.\n"
                             "3. List the lights / switches / sensors you want in HA_TILES, then re-flash.\n\n"
                             "Your tiles will appear here as big touch buttons.", F_S, C_MUTED);
    lv_obj_set_width(d, 640);
    lv_obj_set_pos(d, 24, 78);
    haGrid = nullptr;
    return;
  }
  haStatus = mkLabel(pg, "Connecting to Home Assistant...", I_20, C_MUTED);
  lv_obj_set_pos(haStatus, 4, 0);
  haGrid = lv_obj_create(pg);
  lv_obj_set_pos(haGrid, 0, 32);
  lv_obj_set_size(haGrid, PAGE_W, PAGE_H - 32);
  lv_obj_set_style_bg_opa(haGrid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(haGrid, 0, 0);
  lv_obj_set_style_pad_all(haGrid, 0, 0);
  lv_obj_set_style_pad_column(haGrid, T_GAP, 0);
  lv_obj_set_style_pad_row(haGrid, T_GAP, 0);
  lv_obj_set_flex_flow(haGrid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_scrollbar_mode(haGrid, LV_SCROLLBAR_MODE_OFF);
  const int tw = (PAGE_W - 3 * T_GAP) / 4;
  for (int i = 0; i < N_TILES; i++) {
    lv_obj_t *b = lv_button_create(haGrid);
    haTile[i] = b;
    lv_obj_set_size(b, tw, 128);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    pressStyle(b, T_R_TILE);
    char k = HA_TILES[i].kind;
    lv_obj_t *ic = mkLabel(b, haIcon(HA_TILES[i]), I_28, C_TEXT);
    lv_obj_set_pos(ic, 16, 14);
    lv_obj_t *nm = mkLabel(b, HA_TILES[i].label, F_M, C_TEXT);
    lv_obj_set_width(nm, tw - 32);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(nm, 16, 66);
    haLblState[i] = mkLabel(b, "...", F_S, C_MUTED);
    lv_obj_set_width(haLblState[i], tw - 32);
    lv_label_set_long_mode(haLblState[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(haLblState[i], 16, 92);
    if (k != 'S') lv_obj_add_event_cb(b, onHaTile, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    else lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
  }
}

static void houseRefreshUi() {
  if (!HA_ENABLED || !haGrid) return;
  lv_label_set_text(haStatus, haReachable ? LUCIDE_CHECK "  Home Assistant connected" : LUCIDE_TRIANGLE_ALERT "  Can't reach Home Assistant");
  lv_obj_set_style_text_color(haStatus, lv_color_hex(haReachable ? C_GOOD : C_BAD), 0);
  DATA_LOCK();
  for (int i = 0; i < N_TILES; i++) {
    char k = HA_TILES[i].kind;
    bool on = (k == 'T') && haSt[i].known && haIsOn(haSt[i].state);
    char txt[40];
    if (k == 'B') strlcpy(txt, "Tap to run", sizeof(txt));
    else if (!haSt[i].known) strlcpy(txt, "...", sizeof(txt));
    else if (k == 'S' && haSt[i].unit[0]) snprintf(txt, sizeof(txt), "%s %s", haSt[i].state, haSt[i].unit);
    else if (!strcmp(haSt[i].state, "on"))  strlcpy(txt, strHas(HA_TILES[i].entity, "door") || strHas(HA_TILES[i].entity, "window") ? "Open" : (strHas(HA_TILES[i].entity, "motion") ? "Motion" : "On"), sizeof(txt));
    else if (!strcmp(haSt[i].state, "off")) strlcpy(txt, strHas(HA_TILES[i].entity, "door") || strHas(HA_TILES[i].entity, "window") ? "Closed" : (strHas(HA_TILES[i].entity, "motion") ? "Clear" : "Off"), sizeof(txt));
    else strlcpy(txt, haSt[i].state, sizeof(txt));
    lv_label_set_text(haLblState[i], txt);
    // on: a primary container; scenes / scripts: surfaceHi; everything else: surface
    uint32_t bg = on ? T->primary : (k == 'B' ? T->surfaceHi : T->surface), fg = on ? T->onPrimary : T->text;
    lv_obj_set_style_bg_color(haTile[i], lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(haTile[i], 0), lv_color_hex(fg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(haTile[i], 1), lv_color_hex(fg), 0);
    lv_obj_set_style_text_color(haLblState[i], lv_color_hex(on ? T->onPrimary : (k == 'S' ? T->text : T->muted)), 0);
  }
  DATA_UNLOCK();
}
