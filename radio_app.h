#pragma once
// Internet radio + the audio task. Also plays voice replies (Google/HA TTS) and chimes, then resumes the radio.
// Stations: favourites saved on the hub (/favs.json, swipeable carousel on the radio page) plus a worldwide
// search (radio-browser.info, no key) on its own screen.

struct Station { char name[48]; char tag[48]; char url[256]; };

// first boot only: seeds the favourites, after that they live in /favs.json on the hub's flash
static const struct { const char *name, *tag, *url; } DEFAULT_FAVS[] = {
  {"KEXP Seattle",        "Indie / world",       "https://kexp.streamguys1.com/kexp128.mp3"},
  {"Radio Paradise",      "Eclectic mix",        "http://stream.radioparadise.com/mp3-128"},
  {"SomaFM Groove Salad", "Ambient / downtempo", "http://ice1.somafm.com/groovesalad-128-mp3"},
  {"SomaFM Secret Agent", "Spy-jazz lounge",     "http://ice1.somafm.com/secretagent-128-mp3"},
  {"NTS 1",               "Live from London",    "https://stream-relay-geo.ntslive.net/stream"},
  {"NTS 2",               "Live from London",    "https://stream-relay-geo.ntslive.net/stream2"},
  {"Jazz24",              "Jazz",                "https://live.wostreaming.net/direct/ppm-jazz24mp3-ibc1"},
  {"Classic FM",          "Classical",           "http://media-ice.musicradio.com/ClassicFMMP3"},
};
#define MAX_FAVS 30
#define FAVS_PATH "/favs.json"
static Station *favs = nullptr;            // PSRAM, MAX_FAVS entries
static int nFavs = 0;
static Station curSt;                      // what's playing: a favourite or a search result
static int curStation = -1;                // its index in favs, -1 = not a favourite

Audio audio(I2S_NUM_1);       // speaker on I2S1: leaves I2S0 free for the PDM microphone (PDM RX only works on I2S0)

enum AudioCmdType : uint8_t { AC_STATION, AC_URL, AC_SPEECH, AC_FILE, AC_STOP, AC_VOLUME };
struct AudioCmd { AudioCmdType type; int arg; char str[256]; };
static QueueHandle_t cmdQ;
static SemaphoreHandle_t metaMx;
static char metaTitle[160];
static volatile bool metaDirty = false;
static volatile uint8_t playState = 0;     // 0 stopped, 1 connecting, 2 playing, 3 error
static volatile uint8_t audioMode = 0;     // 0 = radio, 1 = overlay (speech / chime / tts file)
static volatile int resumeStation = -1;    // >= 0: bring the radio back when an overlay finishes
static volatile bool wantPlaying = false;  // user intent, drives auto-reconnect
static int volume = 10;                    // 0..21
static uint32_t sleepAt = 0;               // millis() when the sleep timer fires (0 = off)

static void audioCmd(AudioCmdType t, int a = 0, const char *s = nullptr) {
  AudioCmd c; c.type = t; c.arg = a; c.str[0] = 0;
  if (s) strlcpy(c.str, s, sizeof(c.str));
  xQueueSend(cmdQ, &c, 0);
}

// ───────────── audio callback (runs inside the audio task) ─────────────
static void onAudioEvent(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_info:
      if (m.msg && strstr(m.msg, "stream ready") && playState == 1) playState = 2;
      break;
    case Audio::evt_streamtitle:
      if (audioMode == 0 && xSemaphoreTake(metaMx, 50) == pdTRUE) {
        copyAscii(metaTitle, sizeof(metaTitle), m.msg ? m.msg : "");
        metaDirty = true;
        xSemaphoreGive(metaMx);
      }
      break;
    case Audio::evt_eof:
      if (audioMode == 1) { audioMode = 2; }                 // overlay finished -> task resumes radio
      else if (playState == 2) playState = 3;                // radio dropped
      break;
    default: break;
  }
}

static void audioTask(void *) {
  AudioCmd c;
  static char radioUrl[256];                                 // the station being played (for resume / reconnect)
  uint32_t connT = 0, retryT = 0;
  for (;;) {
    while (xQueueReceive(cmdQ, &c, 0) == pdTRUE) {
      switch (c.type) {
        case AC_STATION:
          strlcpy(radioUrl, c.str, sizeof(radioUrl));
          audio.stopSong(); audioMode = 0; wantPlaying = true; playState = 1; connT = millis();
          if (!audio.connecttohost(radioUrl)) playState = 3;
          break;
        case AC_URL: case AC_SPEECH: case AC_FILE: {
          bool wasRadio = (audioMode == 0 && (playState == 1 || playState == 2));
          if (wasRadio && resumeStation < 0) resumeStation = 1;
          audio.stopSong(); audioMode = 1;
          bool ok = false;
          if (c.type == AC_URL) ok = audio.connecttohost(c.str);
          else if (c.type == AC_SPEECH) ok = audio.connecttospeech(c.str, HA_LANGUAGE);
          else ok = audio.connecttoFS(LittleFS, c.str);
          if (!ok) audioMode = 2;
          break; }
        case AC_STOP:
          audio.stopSong(); wantPlaying = false; playState = 0; audioMode = 0; break;
        case AC_VOLUME:
          audio.setVolume(c.arg); break;
      }
    }
    if (audioMode == 2) {                                    // overlay done
      audioMode = 0;
      if (resumeStation >= 0 && radioUrl[0]) {
        resumeStation = -1;
        audio.stopSong(); wantPlaying = true; playState = 1; connT = millis();
        if (!audio.connecttohost(radioUrl)) playState = 3;
      }
    }
    // reconnect watchdog: stuck connecting for 15 s, or dropped -> retry every 6 s
    if (wantPlaying && audioMode == 0 && radioUrl[0]) {
      if (playState == 1 && millis() - connT > 15000) playState = 3;
      if (playState == 3 && millis() - retryT > 6000) {
        retryT = millis(); playState = 1; connT = millis();
        audio.stopSong();
        if (!audio.connecttohost(radioUrl)) playState = 3;
      }
    }
    audio.loop();
    vTaskDelay(1);
  }
}

static void audioBegin() {
  metaMx = xSemaphoreCreateMutex();
  cmdQ = xQueueCreate(8, sizeof(AudioCmd));
  Audio::audio_info_callback = onAudioEvent;
#if AUDIO_OUT_EXTERNAL
  audio.setPinout(DAC_BCLK, DAC_LRC, DAC_DOUT);
  stc(STC_SPK_OFF);                        // onboard amp stays muted
#else
  audio.setPinout(5, 6, 4);                // BCLK, LRC, DOUT: fixed by the board
  stc(STC_SPK_ON);
  audio.forceMono(true);                   // one speaker: mix both channels instead of losing one
  audio.setTone(SPK_TONE_LOW, SPK_TONE_MID, SPK_TONE_HIGH);
#endif
  audio.setVolume(volume);
  xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 3, nullptr, 0);
}

// "duck" the radio while the mic listens, then bring it back (voice.h uses these)
static void radioDuckBegin() {
  if (audioMode == 0 && (playState == 1 || playState == 2)) resumeStation = 1;
  audioCmd(AC_STOP);
}
static void radioDuckEnd() {
  if (resumeStation >= 0) { resumeStation = -1; audioCmd(AC_STATION, 0, curSt.url); }
}
// speak text through the speaker (TTS) and resume the radio afterwards
static void saySpeech(const char *text) { audioCmd(AC_SPEECH, 0, text); }

// ───────────── favourites ─────────────
static Preferences prefs;

static void setStation(Station &s, const char *name, const char *tag, const char *url) {
  copyAscii(s.name, sizeof(s.name), name);
  copyAscii(s.tag, sizeof(s.tag), tag);
  strlcpy(s.url, url, sizeof(s.url));
}
static int favFind(const char *url) {
  for (int i = 0; i < nFavs; i++) if (!strcmp(favs[i].url, url)) return i;
  return -1;
}
static void favsSave() {
  File f = LittleFS.open(FAVS_PATH, "w");
  if (!f) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nFavs; i++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = favs[i].name; o["t"] = favs[i].tag; o["u"] = favs[i].url;
  }
  serializeJson(d, f);
  f.close();
}
// after LittleFS.begin(): load favourites (seed on first boot) and what was playing last
static void favsLoad() {
  if (!favs) favs = (Station *)heap_caps_calloc(MAX_FAVS, sizeof(Station), MALLOC_CAP_SPIRAM);
  if (!favs) return;
  nFavs = 0;
  if (LittleFS.exists(FAVS_PATH)) {
    File f = LittleFS.open(FAVS_PATH, "r");
    JsonDocument d;
    if (f && !deserializeJson(d, f))
      for (JsonObject o : d.as<JsonArray>()) {
        if (nFavs >= MAX_FAVS) break;
        setStation(favs[nFavs++], o["n"] | "", o["t"] | "", o["u"] | "");
      }
    if (f) f.close();
  } else {
    for (auto &s : DEFAULT_FAVS) setStation(favs[nFavs++], s.name, s.tag, s.url);
    favsSave();
  }
  if (prefs.getBytesLength("cur") == sizeof(Station)) prefs.getBytes("cur", &curSt, sizeof(Station));
  else if (nFavs) curSt = favs[0];
  curStation = favFind(curSt.url);
}

// ───────────── radio page ─────────────
static lv_obj_t *lblStation, *lblTitle, *lblStatus, *lblPlay, *sldVol, *lblSleep, *lblFav, *carousel;
static void carouselRebuild();
static void carouselHighlight();
static void favBtnUpdate() {
  if (lblFav) lv_label_set_text(lblFav, curStation >= 0 ? LV_SYMBOL_OK "  Favourite" : LV_SYMBOL_PLUS "  Favourite");
}

// play any station (favIdx = its index in favs, or -1)
static void playStationData(const Station &s, int favIdx) {
  if (!s.url[0]) return;
  if (&s != &curSt) curSt = s;
  curStation = favIdx;
  resumeStation = -1;
  prefs.putBytes("cur", &curSt, sizeof(Station));
  lv_label_set_text(lblStation, curSt.name);
  lv_label_set_text(lblTitle, curSt.tag);
  favBtnUpdate();
  carouselHighlight();                                       // restyle only: this can run inside a card's click event
  if (xSemaphoreTake(metaMx, portMAX_DELAY) == pdTRUE) { metaTitle[0] = 0; metaDirty = true; xSemaphoreGive(metaMx); }
  playState = 1; wantPlaying = true;
  audioCmd(AC_STATION, 0, curSt.url);
}
static void playStation(int i) {                             // favourite i, wrapping round
  if (nFavs == 0) return;
  if (i < 0) i = nFavs - 1;
  if (i >= nFavs) i = 0;
  playStationData(favs[i], i);
}
static void playCurrent() { if (curSt.url[0]) playStationData(curSt, favFind(curSt.url)); else playStation(0); }

// add / remove a station from the favourites (returns true if it is a favourite afterwards)
static bool favToggle(const Station &s) {
  int i = favFind(s.url);
  if (i >= 0) {
    memmove(&favs[i], &favs[i + 1], sizeof(Station) * (nFavs - i - 1));
    nFavs--;
  } else {
    if (nFavs >= MAX_FAVS) return false;
    favs[nFavs++] = s;
  }
  favsSave();
  curStation = favFind(curSt.url);
  favBtnUpdate();
  carouselRebuild();
  return i < 0;
}

static void radioStop() { playState = 0; wantPlaying = false; resumeStation = -1; audioCmd(AC_STOP); }
static void setVolume(int v) {
  volume = constrain(v, 0, 21);
  audioCmd(AC_VOLUME, volume);
  if (sldVol) lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  prefs.putUChar("vol", (uint8_t)volume);
}

static void onFavCard(lv_event_t *e) { playStation((int)(intptr_t)lv_event_get_user_data(e)); }
static void onFindStations(lv_event_t *);                     // opens the search screen (in the .ino)
static void onFavBtn(lv_event_t *) { favToggle(curSt); }
static void onNext(lv_event_t *) { playStation(curStation + 1); }
static void onPrev(lv_event_t *) { playStation(curStation - 1); }
static void onPlayPause(lv_event_t *) {
  if (playState == 0 || playState == 3) playCurrent(); else radioStop();
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
  if (mins[idx]) { sleepAt = millis() + (uint32_t)mins[idx] * 60000UL; lv_label_set_text_fmt(lblSleep, LV_SYMBOL_BELL "  Sleep %d min", mins[idx]); }
  else { sleepAt = 0; lv_label_set_text(lblSleep, LV_SYMBOL_BELL "  Sleep: off"); }
}

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
// mark the playing favourite and bring it into view (card i = favs[i]; the last card is "Find stations")
static void carouselHighlight() {
  if (!carousel) return;
  lv_obj_t *sel = nullptr;
  for (int i = 0; i < nFavs && i < (int)lv_obj_get_child_count(carousel) - 1; i++) {
    lv_obj_t *b = lv_obj_get_child(carousel, i);
    bool on = (i == curStation);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_CARD3 : C_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(b, on ? 4 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), lv_color_hex(on ? C_ACCENT : C_MUTED), 0);
    if (on) sel = b;
  }
  if (sel) lv_obj_scroll_to_view(sel, LV_ANIM_ON);
}
// favourites as big swipeable cards, "Find stations" at the end. Never call from a carousel card's own event.
static void carouselRebuild() {
  if (!carousel) return;
  lv_obj_clean(carousel);
  for (int i = 0; i < nFavs; i++) {
    lv_obj_t *b = carouselCard(210, C_CARD);
    lv_obj_t *n = mkLabel(b, favs[i].name, F_M, C_TEXT);
    lv_obj_set_width(n, 178);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(n, 56, 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *t = mkLabel(b, favs[i].tag, F_XS, C_MUTED);
    lv_obj_set_width(t, 178);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b, onFavCard, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  lv_obj_t *f = carouselCard(170, C_ACCENT);
  lv_obj_set_style_bg_grad_color(f, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
  lv_obj_t *fl = mkLabel(f, LV_SYMBOL_PLUS "\nFind\nstations", F_M, C_TEXT);
  lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(fl);
  lv_obj_add_event_cb(f, onFindStations, LV_EVENT_CLICKED, nullptr);
  lv_obj_update_layout(carousel);
  carouselHighlight();
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
  lv_obj_t *cap = mkLabel(c, "NOW PLAYING", F_XS, C_ACCENT);
  lv_obj_set_style_text_letter_space(cap, 3, 0);
  lv_obj_set_pos(cap, 24, 14);

  lblStation = mkLabel(c, curSt.name, F_L, C_TEXT);
  lv_obj_set_width(lblStation, 430);
  lv_label_set_long_mode(lblStation, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lblStation, 24, 34);

  lblTitle = mkLabel(c, curSt.tag, F_M, C_MUTED);
  lv_obj_set_width(lblTitle, 430);
  lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_pos(lblTitle, 24, 74);

  lblStatus = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_pos(lblStatus, 24, 106);

  lv_obj_t *fb = textBtn(c, 474, 18, 190, 52, "", F_S, C_CARD3, onFavBtn);
  lblFav = btnLabel(fb);
  favBtnUpdate();

  textBtn(c, 24, 150, 76, 76, LV_SYMBOL_PREV, F_L, C_CARD3, onPrev);
  lv_obj_t *play = textBtn(c, 116, 138, 100, 100, LV_SYMBOL_PLAY, F_XL, C_ACCENT, onPlayPause);
  lblPlay = btnLabel(play);
  lv_obj_set_style_bg_grad_color(play, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(play, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(play, 40, 0);
  lv_obj_set_style_shadow_color(play, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(play, LV_OPA_40, 0);
  textBtn(c, 232, 150, 76, 76, LV_SYMBOL_NEXT, F_L, C_CARD3, onNext);

  lv_obj_t *sb = textBtn(c, 340, 140, 190, 46, LV_SYMBOL_BELL "  Sleep: off", F_S, C_CARD2, onSleep);
  lblSleep = btnLabel(sb);

  lv_obj_t *vlo = mkLabel(c, LV_SYMBOL_VOLUME_MID, F_M, C_MUTED);
  lv_obj_set_pos(vlo, 340, 202);
  lv_obj_t *vhi = mkLabel(c, LV_SYMBOL_VOLUME_MAX, F_M, C_MUTED);
  lv_obj_set_pos(vhi, 638, 202);
  sldVol = lv_slider_create(c);
  lv_slider_set_range(sldVol, 0, 21);
  lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  lv_obj_set_size(sldVol, 236, 14);
  lv_obj_set_pos(sldVol, 382, 208);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_CARD3), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sldVol, 12, LV_PART_KNOB);        // fat knob: easy with floury fingers
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_RELEASED, nullptr);

  carouselRebuild();
}

// ───────────── worldwide search (radio-browser.info): UI sets rbQuery + reqRadioSearch, netTask fills rbHits ─────────────
struct RbHit { Station s; char detail[64]; };
#define RB_MAX 20
static RbHit *rbHits = nullptr;                              // PSRAM
static volatile int rbN = 0;                                 // -1 = search failed
static volatile uint8_t rbVer = 0;
static char rbQuery[48];
static volatile bool reqRadioSearch = false;

static bool rbCodecOk(const char *c) {                       // what the audio library can play
  return !strcasecmp(c, "MP3") || !strcasecmp(c, "AAC") || !strcasecmp(c, "AAC+") || !strcasecmp(c, "OGG") || !strcasecmp(c, "FLAC");
}
static int rbFetch(const char *endpoint, int n) {          // appends to rbHits, returns the new count (or -1)
  String url = String("https://de1.api.radio-browser.info/json/stations/") + endpoint;
  for (const char *c = rbQuery; *c; c++) {
    if (isalnum((unsigned char)*c) || *c == '-' || *c == '.') url += *c;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (unsigned char)*c); url += h; }
  }
  url += "?limit=30&hidebroken=true&order=clickcount&reverse=true";
  String body;
  if (httpGet(url.c_str(), body) != 200) return n ? n : -1;
  JsonDocument filter;
  JsonObject fo = filter.add<JsonObject>();
  fo["name"] = true; fo["url_resolved"] = true; fo["countrycode"] = true; fo["tags"] = true; fo["codec"] = true; fo["bitrate"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return n ? n : -1;
  for (JsonObject r : doc.as<JsonArray>()) {
    if (n >= RB_MAX) break;
    const char *u = r["url_resolved"] | "";
    const char *codec = r["codec"] | "";
    if (!u[0] || strlen(u) >= sizeof(Station::url) || !rbCodecOk(codec)) continue;
    bool dup = false;
    for (int i = 0; i < n; i++) if (!strcmp(rbHits[i].s.url, u)) dup = true;
    if (dup) continue;
    RbHit &h = rbHits[n++];
    String tags = String((const char *)(r["tags"] | ""));
    tags.replace(",", ", ");
    String name = String((const char *)(r["name"] | ""));
    name.trim();
    setStation(h.s, name.c_str(), tags.length() ? tags.c_str() : (const char *)(r["countrycode"] | ""), u);
    char det[96];
    snprintf(det, sizeof(det), "%s  %s %d  %s", (const char *)(r["countrycode"] | ""), codec, (int)(r["bitrate"] | 0), tags.c_str());
    copyAscii(h.detail, sizeof(h.detail), det);
  }
  return n;
}
static void radioSearchFetch() {                             // netTask
  if (!rbHits) rbHits = (RbHit *)heap_caps_calloc(RB_MAX, sizeof(RbHit), MALLOC_CAP_SPIRAM);
  int n = rbHits ? rbFetch("byname/", 0) : -1;
  if (n >= 0 && n < 5) n = rbFetch("bytag/", n);              // "jazz", "ambient"...: genres too
  rbN = n;
  rbVer++;
}

// called from the 5 Hz tick. Returns the status text so the home page can mirror it.
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
    default: st = "Stopped"; break;
  }
  radioStatusText = st;
  static const char *lastSt = nullptr;
  if (st != lastSt) {
    lastSt = st;
    lv_label_set_text(lblStatus, st);
    lv_obj_set_style_text_color(lblStatus, lv_color_hex(sc), 0);
  }
  static bool lastPlayIcon = true;
  bool playIcon = (playState == 1 || playState == 2);
  if (playIcon != lastPlayIcon) { lastPlayIcon = playIcon; lv_label_set_text(lblPlay, playIcon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); }

  if (sleepAt && (int32_t)(millis() - sleepAt) >= 0) {
    sleepAt = 0;
    radioStop();
    lv_label_set_text(lblSleep, LV_SYMBOL_BELL "  Sleep: off");
    postUi(UM_TOAST, "Sleep timer - radio off. Night night!", C_ACCENT);
  }
}
