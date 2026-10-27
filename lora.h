#pragma once
// "When the world ends" mode: an off-grid LoRa text mesh. Needs Elecrow's SX1262 (Meshtastic) wireless module in the
// socket and switches S1 S0 = 01. Messages flood across every hub in range and get repeated up to 3 hops, so a few
// of these across a street make a neighbourhood network that needs no internet, no phone mast, no mains-powered router.
// Boot mode is stored in flash; the on-screen buttons tell you when to flip the switches.

SX1262 radio = new Module(8, 20, 19, 2, SPI);               // NSS, DIO1, RESET, BUSY (Elecrow lesson-07)

static bool loraOk = false;
static int loraErr = 0;
static volatile bool loraRxFlag = false;
static void IRAM_ATTR loraIsr() { loraRxFlag = true; }

struct LoraNode { char name[12]; uint32_t last; int rssi; bool used; };
static LoraNode lnodes[8];
static uint32_t seenIds[24]; static int seenN = 0;
static float loraRssi = 0, loraSnr = 0;
static int loraTx = 0, loraRx = 0;
static bool beaconOn = true;
static uint32_t nextBeacon = 0, lastTxMs = 0;
struct LoraPending { char pkt[150]; uint32_t at; bool used; };
static LoraPending lpend[4];

static lv_obj_t *scrLora, *loraLog, *loraStatus, *loraNodesLbl, *loraClock, *loraKb, *loraTa, *loraCompose, *loraBeaconLbl;
static void loraLogAdd(const char *who, const char *text, bool mine, int rssi);

static bool wasSeen(uint32_t id) { for (int i = 0; i < seenN && i < 24; i++) if (seenIds[i] == id) return true; return false; }
static void markSeen(uint32_t id) { seenIds[seenN % 24] = id; seenN++; }

static void loraTransmit(const char *pkt) {
  uint32_t wait = millis() - lastTxMs;
  if (wait < 1500) delay(1500 - wait);                       // be polite to the band
  radio.standby();
  int st = radio.transmit((const uint8_t *)pkt, strlen(pkt));
  if (st == RADIOLIB_ERR_NONE) loraTx++;
  lastTxMs = millis();
  radio.startReceive();
}

static void loraSend(const char *text, int ttl = 3) {
  if (!loraOk) { postUi(UM_TOAST, "LoRa radio not running", C_BAD); return; }
  uint32_t id = esp_random();
  markSeen(id);
  char pkt[150];
  snprintf(pkt, sizeof(pkt), "C1|%d|%08lx|%s|%.100s", ttl, (unsigned long)id, LORA_NODE_NAME, text);
  loraTransmit(pkt);
  if (strcmp(text, "BEACON")) loraLogAdd(LORA_NODE_NAME, text, true, 0);
}

static void loraBegin() {
  SPI.begin(5, 4, 6, 8);                                      // SCLK, MISO, MOSI, SS
  loraErr = radio.begin(LORA_FREQ_MHZ, 125.0, 9, 5, 0x12, LORA_POWER_DBM, 8, 3.3f);
  if (loraErr == RADIOLIB_ERR_NONE) {
    radio.setCurrentLimit(140.0);
    radio.setDio1Action(loraIsr);
    loraOk = (radio.startReceive() == RADIOLIB_ERR_NONE);
  }
  nextBeacon = millis() + 20000;
}

static void touchNode(const char *name, int rssi) {
  int free = -1;
  for (int i = 0; i < 8; i++) {
    if (lnodes[i].used && !strcmp(lnodes[i].name, name)) { lnodes[i].last = millis(); lnodes[i].rssi = rssi; return; }
    if (!lnodes[i].used && free < 0) free = i;
  }
  if (free < 0) free = 0;
  lnodes[free].used = true; strlcpy(lnodes[free].name, name, sizeof(lnodes[free].name)); lnodes[free].last = millis(); lnodes[free].rssi = rssi;
}

static void loraHandleRx() {
  size_t len = radio.getPacketLength();
  uint8_t buf[200];
  if (len == 0 || len >= sizeof(buf)) { radio.startReceive(); return; }
  int st = radio.readData(buf, len);
  loraRssi = radio.getRSSI(); loraSnr = radio.getSNR();
  radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) return;
  buf[len] = 0;
  char *p = (char *)buf;
  if (strncmp(p, "C1|", 3)) return;                          // not ours
  char *f[4]; int n = 0; f[n++] = p + 3;
  for (char *q = p + 3; *q && n < 4; q++) if (*q == '|') { *q = 0; f[n++] = q + 1; }
  if (n < 4) return;
  int ttl = atoi(f[0]);
  uint32_t id = strtoul(f[1], nullptr, 16);
  if (wasSeen(id)) return;
  markSeen(id);
  loraRx++;
  char who[12]; copyText(who, sizeof(who), f[2]);
  char text[110]; copyText(text, sizeof(text), f[3]);
  touchNode(who, (int)loraRssi);
  if (strcmp(text, "BEACON")) { loraLogAdd(who, text, false, (int)loraRssi); beep(2); }
  if (ttl > 0) {                                              // repeat it, after a random pause so repeaters don't collide
    for (int i = 0; i < 4; i++) if (!lpend[i].used) {
      snprintf(lpend[i].pkt, sizeof(lpend[i].pkt), "C1|%d|%08lx|%s|%s", ttl - 1, (unsigned long)id, who, text);
      lpend[i].at = millis() + random(400, 2200); lpend[i].used = true; break;
    }
  }
}

static void loraTick() {
  if (!loraOk) return;
  if (loraRxFlag) { loraRxFlag = false; loraHandleRx(); }
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) if (lpend[i].used && (int32_t)(now - lpend[i].at) >= 0) { lpend[i].used = false; loraTransmit(lpend[i].pkt); }
  if (beaconOn && (int32_t)(now - nextBeacon) >= 0) { nextBeacon = now + 300000UL; loraSend("BEACON", 1); }
}

// ───────────── UI ─────────────
static void loraLogAdd(const char *who, const char *text, bool mine, int rssi) {
  if (!loraLog) return;
  struct tm t; char hm[8] = "--:--";
  if (nowTm(&t)) snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  lv_obj_t *r = panel(loraLog, 0, 0, 484, 56, mine ? T->selected : T->surfaceHi, T_R_ROW);
  lv_obj_t *a = mkLabel(r, hm, F_XS, C_MUTED); lv_obj_set_pos(a, 10, 6);
  char meta[40]; if (mine) snprintf(meta, sizeof(meta), "%s (you)", who); else snprintf(meta, sizeof(meta), "%s  %d dBm", who, rssi);
  lv_obj_t *b = mkLabel(r, meta, F_XS, mine ? C_ACCENT : C_GOOD); lv_obj_set_pos(b, 60, 6);
  lv_obj_t *c = mkLabel(r, text, F_S, C_TEXT);
  lv_obj_set_width(c, 464); lv_label_set_long_mode(c, LV_LABEL_LONG_DOT); lv_obj_set_pos(c, 10, 28);
  while (lv_obj_get_child_count(loraLog) > 40) lv_obj_delete(lv_obj_get_child(loraLog, 0));
  lv_obj_scroll_to_y(loraLog, LV_COORD_MAX, LV_ANIM_ON);
}

static void onQuick(lv_event_t *e) { static const char *Q[] = {"I'm OK", "Need help", "Meet at base", "Have water", "Have food", "All clear"}; loraSend(Q[(int)(intptr_t)lv_event_get_user_data(e)]); }
static void onBeacon(lv_event_t *) { beaconOn = !beaconOn; lv_label_set_text(loraBeaconLbl, beaconOn ? "Beacon: ON" : "Beacon: off"); }
static void onType(lv_event_t *) { lv_textarea_set_text(loraTa, ""); lv_obj_remove_flag(loraCompose, LV_OBJ_FLAG_HIDDEN); }
static void onKbLora(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_READY) { const char *s = lv_textarea_get_text(loraTa); if (s[0]) loraSend(s); lv_obj_add_flag(loraCompose, LV_OBJ_FLAG_HIDDEN); }
  else if (c == LV_EVENT_CANCEL) lv_obj_add_flag(loraCompose, LV_OBJ_FLAG_HIDDEN);
}
static void showModal(const char *title, const char *body);
static void setBootMode(int m);
static void onExitHub(lv_event_t *) {
  setBootMode(0);
  showModal("Back to hub mode", "Power the hub OFF, set switches S1 S0 to 00 (MIC & SPK), then power it on again.");
}

static void loraBuild() {
  scrLora = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrLora, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scrLora, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = mkLabel(scrLora, LUCIDE_SATELLITE_DISH "  Survival mesh", I_24, C_GOOD);
  lv_obj_set_pos(t, 16, 14);
  loraClock = mkLabel(scrLora, "--:--", F_T26, C_TEXT);
  lv_obj_align(loraClock, LV_ALIGN_TOP_RIGHT, -20, 12);

  lv_obj_t *lc = card(scrLora, 12, 60, 508, 300);
  loraLog = scrollCol(lc, 12, 8, 484, 284, 6);
  lv_obj_t *sc = card(scrLora, 532, 60, 256, 120);
  loraStatus = mkLabel(sc, "starting radio...", F_S, C_TEXT);
  lv_obj_set_width(loraStatus, 232); lv_obj_set_pos(loraStatus, 12, 10);
  lv_obj_t *nc = card(scrLora, 532, 188, 256, 172);
  mkLabel(nc, "HEARD RECENTLY", F_XS, C_MUTED); lv_obj_set_pos(lv_obj_get_child(nc, 0), 14, 8);
  loraNodesLbl = mkLabel(nc, "nobody yet", F_S, C_TEXT);
  lv_obj_set_width(loraNodesLbl, 230); lv_obj_set_pos(loraNodesLbl, 14, 32);

  const char *Q[] = {"I'm OK", "Need help", "Meet at base", "Have water", "Have food", "All clear"};
  for (int i = 0; i < 6; i++) textBtn(scrLora, 12 + (i % 3) * 176, 370 + (i / 3) * 56, 168, 48, Q[i], F_M, i == 1 ? C_BAD : T->surfaceHi, onQuick, (void *)(intptr_t)i);
  textBtn(scrLora, 540, 370, 118, 48, "Type...", F_M, T->primary, onType);
  lv_obj_t *bb = textBtn(scrLora, 664, 370, 124, 48, "Beacon: ON", F_M, T->surfaceHi, onBeacon);
  loraBeaconLbl = btnLabel(bb);
  textBtn(scrLora, 540, 426, 248, 48, "Back to hub mode", F_M, T->surfaceHi, onExitHub);

  loraCompose = panel(scrLora, 0, 0, SCREEN_W, SCREEN_H, T->bg, 0);
  loraTa = textField(loraCompose, 12, 12, 776, 64, "Type a message to everyone in range", nullptr);
  lv_textarea_set_max_length(loraTa, 100);
  loraKb = keyboardFor(loraCompose, SCREEN_W, 270, loraTa, onKbLora);
  lv_obj_add_flag(loraCompose, LV_OBJ_FLAG_HIDDEN);
}

static void loraUiTick() {
  struct tm t;
  if (nowTm(&t)) { char b[8]; snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min); lv_label_set_text(loraClock, b); }
  char s[160];
  if (loraOk) snprintf(s, sizeof(s), "%.1f MHz \xC2\xB7 SF9\nsent %d \xC2\xB7 heard %d\nlast %d dBm \xC2\xB7 SNR %.0f", LORA_FREQ_MHZ, loraTx, loraRx, (int)loraRssi, loraSnr);
  else snprintf(s, sizeof(s), "No radio (error %d)\nIs the LoRa module fitted and\nS1 S0 set to 01?", loraErr);
  lv_label_set_text(loraStatus, s);
  lv_obj_set_style_text_color(loraStatus, lv_color_hex(loraOk ? C_TEXT : C_BAD), 0);
  char n[200] = ""; int c = 0;
  for (int i = 0; i < 8; i++) if (lnodes[i].used) { char one[40]; snprintf(one, sizeof(one), "%s  %d dBm  %lum\n", lnodes[i].name, lnodes[i].rssi, (unsigned long)((millis() - lnodes[i].last) / 60000)); strlcat(n, one, sizeof(n)); c++; }
  lv_label_set_text(loraNodesLbl, c ? n : "nobody yet");
}
