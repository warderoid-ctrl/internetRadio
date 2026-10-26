#pragma once
// Internet radio + the audio task. Also plays voice replies (Google/HA TTS) and chimes, then resumes the radio.
// Stations: favourites saved on the hub (/favs.json, swipeable carousel on the radio page) plus a worldwide
// search (radio-browser.info, no key) on its own screen.

struct Station { char name[48]; char tag[48]; char url[256]; char logo[160]; uint8_t kind; };   // kind: 0 live, 1 podcast episode

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
#define MAX_RECENT 10
#define RECENT_PATH "/recent.json"
static Station *recents = nullptr;         // last played, newest first (PSRAM)
static int nRecent = 0;
static Station curSt;                      // what's playing: a favourite or a search result
static int curStation = -1;                // its index in favs, -1 = not a favourite
static char wxCC[3] = "GB";                // ISO country of the weather location (weather.h sets it; Places lists it first)
static volatile bool reqNowPlaying = false; // station changed: fetch now-playing info soon (BBC)

Audio audio(I2S_NUM_1);       // speaker on I2S1: leaves I2S0 free for the PDM microphone (PDM RX only works on I2S0)

enum AudioCmdType : uint8_t { AC_STATION, AC_URL, AC_SPEECH, AC_FILE, AC_STOP, AC_VOLUME, AC_SEEK };
struct AudioCmd { AudioCmdType type; int arg; char str[256]; };
static QueueHandle_t cmdQ;
static SemaphoreHandle_t metaMx;
static char metaTitle[160];
static volatile bool metaDirty = false;
static volatile uint8_t playState = 0;     // 0 stopped, 1 connecting, 2 playing, 3 error, 4 episode finished
static volatile uint8_t audioMode = 0;     // 0 = radio, 1 = overlay (speech / chime / tts file)
static volatile int resumeStation = -1;    // >= 0: bring the radio back when an overlay finishes
static volatile bool wantPlaying = false;  // user intent, drives auto-reconnect
static volatile uint8_t playKind = 0;      // Station::kind of what the audio task plays (episodes end, streams reconnect)
static volatile uint32_t posSec = 0, durSec = 0;  // episode position / length (audio task keeps them current)
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
      if (m.msg && playState == 1) Serial.printf("audio: %s\n", m.msg);   // connection progress, for debugging streams
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
      else if (playState == 2) {
        if (playKind == 1) { playState = 4; wantPlaying = false; }   // podcast episode reached its end
        else playState = 3;                                  // radio dropped
      }
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
          playKind = (uint8_t)c.arg;
          posSec = durSec = 0;
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
        case AC_SEEK:                                        // episodes only: relative seconds (range request)
          if (playKind == 1 && audioMode == 0) audio.setTimeOffset(c.arg);
          break;
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
    static uint32_t posT = 0;
    if (playKind == 1 && millis() - posT > 500) { posT = millis(); posSec = audio.getAudioCurrentTime(); durSec = audio.getAudioFileDuration(); }
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
  if (resumeStation >= 0) { resumeStation = -1; audioCmd(AC_STATION, curSt.kind, curSt.url); }
}
// speak text through the speaker (TTS) and resume the radio afterwards
static void saySpeech(const char *text) { audioCmd(AC_SPEECH, 0, text); }

// ───────────── favourites ─────────────
static Preferences prefs;

static void setStation(Station &s, const char *name, const char *tag, const char *url) {
  copyAscii(s.name, sizeof(s.name), name);
  copyAscii(s.tag, sizeof(s.tag), tag);
  strlcpy(s.url, url, sizeof(s.url));
  s.logo[0] = 0;
  s.kind = 0;
}
static int favFind(const char *url) {
  for (int i = 0; i < nFavs; i++) if (!strcmp(favs[i].url, url)) return i;
  return -1;
}
static void stationsSave(const char *path, const Station *list, int n) {
  File f = LittleFS.open(path, "w");
  if (!f) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = list[i].name; o["t"] = list[i].tag; o["u"] = list[i].url;
    if (list[i].kind) o["k"] = list[i].kind;
    if (list[i].logo[0]) o["l"] = list[i].logo;
  }
  serializeJson(d, f);
  f.close();
}
static int stationsLoad(const char *path, Station *list, int max) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  JsonDocument d;
  int n = 0;
  if (!deserializeJson(d, f))
    for (JsonObject o : d.as<JsonArray>()) {
      if (n >= max) break;
      setStation(list[n], o["n"] | "", o["t"] | "", o["u"] | "");
      strlcpy(list[n].logo, o["l"] | "", sizeof(list[0].logo));
      list[n++].kind = o["k"] | 0;
    }
  f.close();
  return n;
}
static void favsSave() { stationsSave(FAVS_PATH, favs, nFavs); }
// put a station at the front of the recently played list (no duplicates)
static void recentPush(const Station &st) {
  if (!recents || !st.url[0] || st.kind == 1) return;        // podcast episodes don't crowd the radio carousel
  int i = 0;
  while (i < nRecent && strcmp(recents[i].url, st.url)) i++;
  if (i == nRecent) i = (nRecent < MAX_RECENT) ? nRecent++ : MAX_RECENT - 1;   // new: drop the oldest if full
  memmove(&recents[1], &recents[0], sizeof(Station) * i);
  recents[0] = st;
  stationsSave(RECENT_PATH, recents, nRecent);
}
// after LittleFS.begin(): load favourites (seed on first boot) and what was playing last
static void favsLoad() {
  if (!favs) favs = (Station *)heap_caps_calloc(MAX_FAVS, sizeof(Station), MALLOC_CAP_SPIRAM);
  if (!favs) return;
  nFavs = 0;
  if (LittleFS.exists(FAVS_PATH)) nFavs = stationsLoad(FAVS_PATH, favs, MAX_FAVS);
  else {
    for (auto &s : DEFAULT_FAVS) setStation(favs[nFavs++], s.name, s.tag, s.url);
    favsSave();
  }
  if (!recents) recents = (Station *)heap_caps_calloc(MAX_RECENT, sizeof(Station), MALLOC_CAP_SPIRAM);
  if (recents && LittleFS.exists(RECENT_PATH)) nRecent = stationsLoad(RECENT_PATH, recents, MAX_RECENT);
  if (prefs.getBytesLength("cur") == sizeof(Station)) prefs.getBytes("cur", &curSt, sizeof(Station));
  else if (nFavs) curSt = favs[0];
  curStation = favFind(curSt.url);
}

// ───────────── radio page ─────────────
static lv_obj_t *lblStation, *lblTitle, *lblStatus, *lblPlay, *sldVol, *lblSleep, *lblFav, *carousel;
static lv_obj_t *lblPrev, *lblNext, *sldSeek, *lblTime;     // episodes: -30 / +30 s and a time bar
static lv_obj_t *npCard, *npArt;                              // now-playing card and its logo
static void carouselRebuild();
static void carouselHighlight();
static void transportUpdate();
static void npArtUpdate() {                                   // swap the now-playing logo (not part of any card event)
  if (!npCard) return;
  if (npArt) lv_obj_delete(npArt);
  npArt = artImage(npCard, 24, 16, curSt.logo);
}
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
  transportUpdate();
  npArtUpdate();
  recentPush(curSt);
  lv_async_call([](void *) { carouselRebuild(); }, nullptr); // after the current event: we may be inside a card's click
  if (xSemaphoreTake(metaMx, portMAX_DELAY) == pdTRUE) { metaTitle[0] = 0; metaDirty = true; xSemaphoreGive(metaMx); }
  playState = 1; wantPlaying = true;
  reqNowPlaying = true;
  audioCmd(AC_STATION, curSt.kind, curSt.url);
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

// rename what's playing (and its favourite card)
static void stationRename(const char *name) {
  if (!name[0]) return;
  copyAscii(curSt.name, sizeof(curSt.name), name);
  prefs.putBytes("cur", &curSt, sizeof(Station));
  if (curStation >= 0) { strlcpy(favs[curStation].name, curSt.name, sizeof(favs[0].name)); favsSave(); }
  for (int i = 0; i < nRecent; i++)
    if (!strcmp(recents[i].url, curSt.url)) { strlcpy(recents[i].name, curSt.name, sizeof(recents[0].name)); stationsSave(RECENT_PATH, recents, nRecent); }
  carouselRebuild();
  lv_label_set_text(lblStation, curSt.name);
}

// BBC streams (HLS) carry no track info; the BBC's own "now playing" feed does. Service id comes from the
// stream URL (".../bbc_6music/..."). netTask polls this while a BBC station plays.
static bool bbcService(const char *url, char *out, size_t n) {
  const char *p = strstr(url, "/bbc_");
  if (!p) return false;
  p++;
  size_t k = 0;
  while (k + 1 < n && ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= '0' && p[k] <= '9') || p[k] == '_')) { out[k] = p[k]; k++; }
  out[k] = 0;
  return k > 4;
}
static void nowPlayingPoll() {                               // netTask
  char svc[40];
  if (!bbcService(curSt.url, svc, sizeof(svc))) return;
  String url = String("https://rms.api.bbc.co.uk/v2/services/") + svc + "/segments/latest?experience=domestic&offset=0&limit=1";
  String body;
  if (httpGet(url.c_str(), body) != 200) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  char t[160] = "";
  JsonObject seg = doc["data"][0];
  if (seg["offset"]["now_playing"] | false) {
    const char *a = seg["titles"]["primary"] | "", *b = seg["titles"]["secondary"] | "";
    snprintf(t, sizeof(t), "%s%s%s", a, (a[0] && b[0]) ? " - " : "", b);
  }
  if (xSemaphoreTake(metaMx, 100) == pdTRUE) {
    copyAscii(metaTitle, sizeof(metaTitle), t);
    metaDirty = true;
    xSemaphoreGive(metaMx);
  }
}

static void radioStop() { playState = 0; wantPlaying = false; resumeStation = -1; audioCmd(AC_STOP); }
static void setVolume(int v) {
  volume = constrain(v, 0, 21);
  audioCmd(AC_VOLUME, volume);
  if (sldVol) lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  prefs.putUChar("vol", (uint8_t)volume);
}

static void onFindStations(lv_event_t *);                     // opens the search screen (in the .ino)
static void onRenameBtn(lv_event_t *);                        // rename dialog (in the .ino)
static void onFavBtn(lv_event_t *) { favToggle(curSt); }
static void onNext(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, 30); else playStation(curStation + 1); }
static void onPrev(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, -30); else playStation(curStation - 1); }
static void onSeek(lv_event_t *) { if (curSt.kind == 1) audioCmd(AC_SEEK, (int)lv_slider_get_value(sldSeek) - (int)posSec); }
static void fmtTime(char *b, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(b, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
// live radio: prev / next station; podcast episode: -30 s / +30 s and a draggable time bar
static void transportUpdate() {
  if (!lblPrev) return;
  bool ep = (curSt.kind == 1);
  lv_label_set_text(lblPrev, ep ? "-30" : LV_SYMBOL_PREV);
  lv_label_set_text(lblNext, ep ? "+30" : LV_SYMBOL_NEXT);
  lv_obj_set_style_text_font(lblPrev, ep ? F_M : F_L, 0);
  lv_obj_set_style_text_font(lblNext, ep ? F_M : F_L, 0);
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
  if (mins[idx]) { sleepAt = millis() + (uint32_t)mins[idx] * 60000UL; lv_label_set_text_fmt(lblSleep, "%dm", mins[idx]); }
  else { sleepAt = 0; lv_label_set_text(lblSleep, LV_SYMBOL_BELL); }
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
// Carousel cards carry what they play in user_data: fav index, or CARD_RECENT + recent index (others: CARD_NONE)
#define CARD_NONE   (-1)
#define CARD_RECENT 1000
static const Station *cardStation(lv_obj_t *b) {
  int k = (int)(intptr_t)lv_obj_get_user_data(b);
  if (k >= CARD_RECENT && k - CARD_RECENT < nRecent) return &recents[k - CARD_RECENT];
  if (k >= 0 && k < nFavs) return &favs[k];
  return nullptr;
}
// mark the playing station and bring it into view
static void carouselHighlight() {
  if (!carousel) return;
  lv_obj_t *sel = nullptr;
  for (int i = 0; i < (int)lv_obj_get_child_count(carousel); i++) {
    lv_obj_t *b = lv_obj_get_child(carousel, i);
    const Station *st = cardStation(b);
    if (!st) continue;
    bool on = !strcmp(st->url, curSt.url);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_CARD3 : C_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(b, on ? 4 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), lv_color_hex(on ? C_ACCENT : C_MUTED), 0);
    if (on && !sel) sel = b;
  }
  if (sel) lv_obj_scroll_to_view(sel, LV_ANIM_ON);
}
static void onCarouselCard(lv_event_t *e) {
  int k = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
  if (k >= CARD_RECENT) { const Station *st = cardStation((lv_obj_t *)lv_event_get_current_target(e)); if (st) { Station c = *st; playStationData(c, favFind(c.url)); } }
  else if (k >= 0) playStation(k);
}
static lv_obj_t *stationCard(const Station &st, int key) {
  lv_obj_t *b = carouselCard(210, C_CARD);
  lv_obj_set_user_data(b, (void *)(intptr_t)key);
  lv_obj_t *n = mkLabel(b, st.name, F_S, C_TEXT);
  lv_obj_set_width(n, 106);
  lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_height(n, 64, 0);
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
      lv_obj_set_size(h, 76, 140);
      lv_obj_set_user_data(h, (void *)(intptr_t)CARD_NONE);
      lv_obj_t *l = mkLabel(h, LV_SYMBOL_LOOP "\nRecent", F_XS, C_MUTED);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(l);
    }
    stationCard(recents[i], CARD_RECENT + i);
  }
  lv_obj_t *f = carouselCard(170, C_ACCENT);
  lv_obj_set_user_data(f, (void *)(intptr_t)CARD_NONE);
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
  npCard = c;
  npArt = artImage(c, 24, 16, curSt.logo);
  lv_obj_t *cap = mkLabel(c, "NOW PLAYING", F_XS, C_ACCENT);
  lv_obj_set_style_text_letter_space(cap, 3, 0);
  lv_obj_set_pos(cap, 100, 14);

  lblStation = mkLabel(c, curSt.name, F_L, C_TEXT);
  oneLine(lblStation, 224, true);
  lv_obj_set_pos(lblStation, 100, 34);

  lblTitle = mkLabel(c, curSt.tag, F_M, C_MUTED);
  oneLine(lblTitle, 564, true);
  lv_obj_set_pos(lblTitle, 100, 74);

  lblStatus = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_pos(lblStatus, 24, 106);

  lv_obj_t *sb = textBtn(c, 334, 18, 60, 52, LV_SYMBOL_BELL, F_S, C_CARD3, onSleep);   // sleep timer: tap to cycle
  lblSleep = btnLabel(sb);
  textBtn(c, 404, 18, 60, 52, LV_SYMBOL_EDIT, F_M, C_CARD3, onRenameBtn);
  lv_obj_t *fb = textBtn(c, 474, 18, 190, 52, "", F_S, C_CARD3, onFavBtn);
  lblFav = btnLabel(fb);
  favBtnUpdate();

  lblPrev = btnLabel(textBtn(c, 24, 150, 76, 76, LV_SYMBOL_PREV, F_L, C_CARD3, onPrev));
  lv_obj_t *play = textBtn(c, 116, 138, 100, 100, LV_SYMBOL_PLAY, F_XL, C_ACCENT, onPlayPause);
  lblPlay = btnLabel(play);
  lv_obj_set_style_bg_grad_color(play, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(play, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(play, 40, 0);
  lv_obj_set_style_shadow_color(play, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(play, LV_OPA_40, 0);
  lblNext = btnLabel(textBtn(c, 232, 150, 76, 76, LV_SYMBOL_NEXT, F_L, C_CARD3, onNext));

  lblTime = mkLabel(c, "", F_XS, C_MUTED);
  lv_obj_set_pos(lblTime, 340, 136);
  sldSeek = lv_slider_create(c);
  lv_slider_set_range(sldSeek, 0, 1);
  lv_obj_set_size(sldSeek, 324, 14);
  lv_obj_set_pos(sldSeek, 340, 166);
  lv_obj_set_style_bg_color(sldSeek, lv_color_hex(C_CARD3), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sldSeek, lv_color_hex(C_ACCENT2), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sldSeek, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sldSeek, 10, LV_PART_KNOB);
  lv_obj_add_event_cb(sldSeek, onSeek, LV_EVENT_RELEASED, nullptr);
  transportUpdate();

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

// ───────────── worldwide stations (radio-browser.info, no key) ─────────────
// The UI sets rbReq (+ rbQuery); netTask fills rbHits and bumps rbVer. The country list loads once, on demand.
struct RbHit { Station s; char detail[64]; };
#define RB_MAX 40
static RbHit *rbHits = nullptr;                              // PSRAM
static volatile int rbN = 0;                                 // -1 = request failed
static volatile uint8_t rbVer = 0;
static char rbQuery[48];                                     // search text, genre tag or country code
enum RbReq : uint8_t { RB_NONE, RB_SEARCH, RB_BY_GENRE, RB_BY_PLACE };
static volatile uint8_t rbReq = RB_NONE;

struct RbCat { char name[36]; char key[36]; int count; };    // a country (key = ISO code)
#define RB_CATS 80
static RbCat *rbPlaces = nullptr;
static volatile int nPlaces = -2;                            // -2 not loaded, -1 failed
static volatile uint8_t rbCatVer = 0;
static volatile bool reqPlaces = false;

static void rbAppendEnc(String &url, const char *q) {
  for (const char *c = q; *c; c++) {
    if (isalnum((unsigned char)*c) || *c == '-' || *c == '.') url += *c;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (unsigned char)*c); url += h; }
  }
}
static int rbGet(const char *path, const char *arg, const char *params, String &body) {
  String url = String("https://de1.api.radio-browser.info/json/") + path;
  if (arg) rbAppendEnc(url, arg);
  url += params;
  return httpGet(url.c_str(), body);
}

// what the audio library can play. HLS (.m3u8, e.g. all BBC stations) is listed with codec "UNKNOWN" but works.
static bool rbIsHls(const char *url) { return strstr(url, ".m3u8") != nullptr; }
static bool rbCodecOk(const char *c, const char *url) {
  return !strcasecmp(c, "MP3") || !strcasecmp(c, "AAC") || !strcasecmp(c, "AAC+") || !strcasecmp(c, "OGG") ||
         !strcasecmp(c, "FLAC") || rbIsHls(url);
}
static int rbFetch(const char *endpoint, int n) {          // appends stations to rbHits, returns the new count (or -1)
  String body;
  if (rbGet(endpoint, rbQuery, "?limit=60&hidebroken=true&order=clickcount&reverse=true", body) != 200) return n ? n : -1;
  JsonDocument filter;
  JsonObject fo = filter.add<JsonObject>();
  fo["name"] = true; fo["url_resolved"] = true; fo["countrycode"] = true; fo["tags"] = true; fo["codec"] = true; fo["bitrate"] = true; fo["favicon"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return n ? n : -1;
  for (JsonObject r : doc.as<JsonArray>()) {
    if (n >= RB_MAX) break;
    const char *u = r["url_resolved"] | "";
    const char *codec = r["codec"] | "";
    if (!u[0] || strlen(u) >= sizeof(Station::url) || !rbCodecOk(codec, u)) continue;
    if (rbIsHls(u)) codec = "HLS";
    bool dup = false;
    for (int i = 0; i < n; i++) if (!strcmp(rbHits[i].s.url, u)) dup = true;
    if (dup) continue;
    RbHit &h = rbHits[n++];
    String tags = String((const char *)(r["tags"] | ""));
    tags.replace(",", ", ");
    String name = String((const char *)(r["name"] | ""));
    name.trim();
    setStation(h.s, name.c_str(), tags.length() ? tags.c_str() : (const char *)(r["countrycode"] | ""), u);
    const char *logo = r["favicon"] | "";
    if (strlen(logo) < sizeof(h.s.logo)) strlcpy(h.s.logo, logo, sizeof(h.s.logo));
    char det[96];
    int kbps = r["bitrate"] | 0;
    if (kbps) snprintf(det, sizeof(det), "%s  %s %d  %s", (const char *)(r["countrycode"] | ""), codec, kbps, tags.c_str());
    else snprintf(det, sizeof(det), "%s  %s  %s", (const char *)(r["countrycode"] | ""), codec, tags.c_str());
    copyAscii(h.detail, sizeof(h.detail), det);
  }
  return n;
}
static uint32_t logoTried[64];
static int nLogoTried = 0;
static volatile bool logosChanged = false;                    // UI rebuilds the carousel + now-playing logo
static bool logoWanted(const Station &s) {
  if (s.logo[0] || !s.url[0] || s.kind != 0 || nLogoTried >= 64) return false;
  uint32_t h = artHash(s.url);
  for (int i = 0; i < nLogoTried; i++) if (logoTried[i] == h) return false;
  return true;
}
static void radioLogoTick() {                                 // netTask: one lookup per call
  const Station *c = nullptr;
  if (logoWanted(curSt)) c = &curSt;
  for (int i = 0; !c && i < nFavs; i++) if (logoWanted(favs[i])) c = &favs[i];
  for (int i = 0; !c && i < nRecent; i++) if (logoWanted(recents[i])) c = &recents[i];
  if (!c) return;
  char url[256], name[48];
  strlcpy(url, c->url, sizeof(url)); strlcpy(name, c->name, sizeof(name));
  logoTried[nLogoTried++] = artHash(url);
  char logo[160] = "";
  JsonDocument filter;
  filter[0]["favicon"] = true;
  for (int pass = 0; pass < 2 && !logo[0]; pass++) {          // by stream URL, then by station name
    String body;
    int code = pass == 0 ? rbGet("stations/byurl?url=", url, "", body)
                         : rbGet("stations/byname/", name, "?limit=5&hidebroken=true&order=clickcount&reverse=true", body);
    if (code != 200) continue;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) continue;
    for (JsonObject r : doc.as<JsonArray>()) {
      const char *f = r["favicon"] | "";
      if (f[0] && strlen(f) < sizeof(logo)) { strlcpy(logo, f, sizeof(logo)); break; }
    }
  }
  if (!logo[0]) return;
  if (!strcmp(curSt.url, url)) { strlcpy(curSt.logo, logo, sizeof(curSt.logo)); prefs.putBytes("cur", &curSt, sizeof(Station)); }
  bool f = false, r = false;
  for (int i = 0; i < nFavs; i++) if (!strcmp(favs[i].url, url)) { strlcpy(favs[i].logo, logo, sizeof(favs[0].logo)); f = true; }
  for (int i = 0; i < nRecent; i++) if (!strcmp(recents[i].url, url)) { strlcpy(recents[i].logo, logo, sizeof(recents[0].logo)); r = true; }
  if (f) favsSave();
  if (r) stationsSave(RECENT_PATH, recents, nRecent);
  logosChanged = true;
}

static void radioStationsFetch(uint8_t req) {               // netTask
  if (!rbHits) rbHits = (RbHit *)heap_caps_calloc(RB_MAX, sizeof(RbHit), MALLOC_CAP_SPIRAM);
  int n = -1;
  if (rbHits) {
    if (req == RB_SEARCH) {
      n = rbFetch("stations/byname/", 0);
      if (n >= 0 && n < 5) n = rbFetch("stations/bytag/", n);   // "jazz", "ambient"...: genres too
    } else if (req == RB_BY_GENRE) n = rbFetch("stations/bytagexact/", 0);
    else if (req == RB_BY_PLACE) n = rbFetch("stations/bycountrycodeexact/", 0);
  }
  rbN = n;
  rbVer++;
}

// genres: hand-picked (radio-browser tags are free text and the popular ones are noisy), each with 20+ stations
static const struct { const char *name, *tag; } GENRES[] = {
  {"Pop", "pop"},
  {"Rock", "rock"},
  {"Jazz", "jazz"},
  {"Classical", "classical"},
  {"Dance", "dance"},
  {"Electronic", "electronic"},
  {"House", "house"},
  {"Deep House", "deep house"},
  {"Techno", "techno"},
  {"Trance", "trance"},
  {"Drum & Bass", "drum and bass"},
  {"Dubstep", "dubstep"},
  {"Jungle", "jungle"},
  {"Breakbeat", "breakbeat"},
  {"Ambient", "ambient"},
  {"Chillout", "chillout"},
  {"Chill", "chill"},
  {"Lounge", "lounge"},
  {"Downtempo", "downtempo"},
  {"Lo-fi", "lofi"},
  {"Trip-Hop", "trip-hop"},
  {"Indie", "indie"},
  {"Indie Rock", "indie rock"},
  {"Alternative", "alternative"},
  {"Punk", "punk"},
  {"Hardcore", "hardcore"},
  {"Grunge", "grunge"},
  {"Metal", "metal"},
  {"Hard Rock", "hard rock"},
  {"Classic Rock", "classic rock"},
  {"Blues", "blues"},
  {"Blues Rock", "blues rock"},
  {"Rockabilly", "rockabilly"},
  {"Soul", "soul"},
  {"Motown", "motown"},
  {"Funk", "funk"},
  {"R&B", "rnb"},
  {"Hip Hop", "hip hop"},
  {"Rap", "rap"},
  {"Reggae", "reggae"},
  {"Dancehall", "dancehall"},
  {"Ska", "ska"},
  {"Dub", "dub"},
  {"Afrobeats", "afrobeats"},
  {"Latin", "latin"},
  {"Salsa", "salsa"},
  {"Reggaeton", "reggaeton"},
  {"Cumbia", "cumbia"},
  {"Bachata", "bachata"},
  {"Merengue", "merengue"},
  {"Bossa Nova", "bossa nova"},
  {"Samba", "samba"},
  {"MPB", "mpb"},
  {"Sertanejo", "sertanejo"},
  {"Tango", "tango"},
  {"Mariachi", "mariachi"},
  {"Country", "country"},
  {"Americana", "americana"},
  {"Bluegrass", "bluegrass"},
  {"Folk", "folk"},
  {"Celtic", "celtic"},
  {"World", "world music"},
  {"Arabic", "arabic"},
  {"Greek", "greek"},
  {"Bollywood", "bollywood"},
  {"K-Pop", "k-pop"},
  {"J-Pop", "j-pop"},
  {"Japanese", "japanese"},
  {"Chanson", "chanson"},
  {"Schlager", "schlager"},
  {"Oldies", "oldies"},
  {"50s", "50s"},
  {"60s", "60s"},
  {"70s", "70s"},
  {"80s", "80s"},
  {"90s", "90s"},
  {"2000s", "2000s"},
  {"Top 40", "top 40"},
  {"Hits", "hits"},
  {"Disco", "disco"},
  {"New Wave", "new wave"},
  {"Synthwave", "synthwave"},
  {"EDM", "edm"},
  {"Hardstyle", "hardstyle"},
  {"Garage", "garage"},
  {"Gospel", "gospel"},
  {"Christian", "christian"},
  {"Soundtrack", "soundtrack"},
  {"Piano", "piano"},
  {"Opera", "opera"},
  {"Baroque", "baroque"},
  {"Smooth Jazz", "smooth jazz"},
  {"Swing", "swing"},
  {"Big Band", "big band"},
  {"Easy Listening", "easy listening"},
  {"Instrumental", "instrumental"},
  {"Acoustic", "acoustic"},
  {"Singer-Songwriter", "singer-songwriter"},
  {"Eclectic", "eclectic"},
  {"Experimental", "experimental"},
  {"Psychedelic", "psychedelic"},
  {"New Age", "new age"},
  {"Meditation", "meditation"},
  {"Nature", "nature"},
  {"Kids", "kids"},
  {"News", "news"},
  {"Talk", "talk"},
  {"Sports", "sports"},
  {"Comedy", "comedy"},
  {"Public Radio", "public radio"},
};
static const int N_GENRES = sizeof(GENRES) / sizeof(GENRES[0]);

static void radioPlacesFetch() {                             // netTask; the weather location's country first
  if (!rbPlaces) rbPlaces = (RbCat *)heap_caps_calloc(RB_CATS, sizeof(RbCat), MALLOC_CAP_SPIRAM);
  String body;
  int n = -1;
  if (rbPlaces && rbGet("countries", nullptr, "?order=stationcount&reverse=true&hidebroken=true", body) == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      n = 1;                                                 // slot 0 = home country, if listed
      bool home = false;
      for (JsonObject c : doc.as<JsonArray>()) {
        const char *cc = c["iso_3166_1"] | "";
        int cnt = c["stationcount"] | 0;
        if (!cc[0] || cnt < 5) continue;
        bool isHome = !strcasecmp(cc, wxCC);
        if (!isHome && n >= RB_CATS) continue;
        RbCat &p = rbPlaces[isHome ? 0 : n++];
        if (isHome) home = true;
        strlcpy(p.key, cc, sizeof(p.key));
        String nm = String((const char *)(c["name"] | cc));
        if (nm.startsWith("The United Kingdom")) nm = "United Kingdom";
        else if (nm.startsWith("The United States")) nm = "United States";
        else if (nm.startsWith("The ")) nm = nm.substring(4);
        int paren = nm.indexOf(" (");
        if (paren > 0) nm = nm.substring(0, paren);
        copyAscii(p.name, sizeof(p.name), nm.c_str());
        p.count = cnt;
      }
      if (!home) { memmove(&rbPlaces[0], &rbPlaces[1], sizeof(RbCat) * (n - 1)); n--; }
    }
  }
  nPlaces = n;
  rbCatVer++;
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
  if (logosChanged) { logosChanged = false; carouselRebuild(); npArtUpdate(); }
  static bool lastPlayIcon = true;
  bool playIcon = (playState == 1 || playState == 2);
  if (playIcon != lastPlayIcon) { lastPlayIcon = playIcon; lv_label_set_text(lblPlay, playIcon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY); }

  if (sleepAt && (int32_t)(millis() - sleepAt) >= 0) {
    sleepAt = 0;
    radioStop();
    lv_label_set_text(lblSleep, LV_SYMBOL_BELL);
    postUi(UM_TOAST, "Sleep timer - radio off. Night night!", C_ACCENT);
  }
}
