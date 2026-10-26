#pragma once
// Internet radio + the audio task. Also plays voice replies (Google/HA TTS) and chimes, then resumes the radio.

struct Station { const char *name; const char *tag; const char *url; };
static const Station STATIONS[] = {
  {"Radio Paradise",      "Eclectic mix",        "http://stream.radioparadise.com/mp3-128"},
  {"SomaFM Groove Salad", "Ambient / downtempo", "http://ice1.somafm.com/groovesalad-128-mp3"},
  {"SomaFM Secret Agent", "Spy-jazz lounge",     "http://ice1.somafm.com/secretagent-128-mp3"},
  {"NTS 1",               "Live from London",    "https://stream-relay-geo.ntslive.net/stream"},
  {"NTS 2",               "Live from London",    "https://stream-relay-geo.ntslive.net/stream2"},
  {"KEXP Seattle",        "Indie / world",       "https://kexp.streamguys1.com/kexp128.mp3"},
  {"Jazz24",              "Jazz",                "https://live.wostreaming.net/direct/ppm-jazz24mp3-ibc1"},
  {"Classic FM",          "Classical",           "http://media-ice.musicradio.com/ClassicFMMP3"},
};
static const int N_STATIONS = sizeof(STATIONS) / sizeof(STATIONS[0]);

Audio audio(I2S_NUM_1);       // speaker on I2S1: leaves I2S0 free for the PDM microphone (PDM RX only works on I2S0)

enum AudioCmdType : uint8_t { AC_STATION, AC_URL, AC_SPEECH, AC_FILE, AC_STOP, AC_VOLUME };
struct AudioCmd { AudioCmdType type; int arg; char str[200]; };
static QueueHandle_t cmdQ;
static SemaphoreHandle_t metaMx;
static char metaTitle[160];
static volatile bool metaDirty = false;
static volatile uint8_t playState = 0;     // 0 stopped, 1 connecting, 2 playing, 3 error
static volatile uint8_t audioMode = 0;     // 0 = radio, 1 = overlay (speech / chime / tts file)
static volatile int resumeStation = -1;    // radio to bring back when an overlay finishes
static volatile bool wantPlaying = false;  // user intent, drives auto-reconnect
static int curStation = 0;
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
  uint32_t connT = 0, retryT = 0;
  for (;;) {
    while (xQueueReceive(cmdQ, &c, 0) == pdTRUE) {
      switch (c.type) {
        case AC_STATION:
          audio.stopSong(); audioMode = 0; wantPlaying = true; playState = 1; connT = millis();
          if (!audio.connecttohost(STATIONS[c.arg].url)) playState = 3;
          break;
        case AC_URL: case AC_SPEECH: case AC_FILE: {
          bool wasRadio = (audioMode == 0 && (playState == 1 || playState == 2));
          if (wasRadio && resumeStation < 0) resumeStation = curStation;
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
      if (resumeStation >= 0) {
        int s = resumeStation; resumeStation = -1;
        audio.stopSong(); wantPlaying = true; playState = 1; connT = millis();
        if (!audio.connecttohost(STATIONS[s].url)) playState = 3;
      }
    }
    // reconnect watchdog: stuck connecting for 15 s, or dropped -> retry every 6 s
    if (wantPlaying && audioMode == 0) {
      if (playState == 1 && millis() - connT > 15000) playState = 3;
      if (playState == 3 && millis() - retryT > 6000) {
        retryT = millis(); playState = 1; connT = millis();
        audio.stopSong();
        if (!audio.connecttohost(STATIONS[curStation].url)) playState = 3;
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
  if (audioMode == 0 && (playState == 1 || playState == 2)) resumeStation = curStation;
  audioCmd(AC_STOP);
}
static void radioDuckEnd() {
  if (resumeStation >= 0) { int s = resumeStation; resumeStation = -1; audioCmd(AC_STATION, s); }
}
// speak text through the speaker (TTS) and resume the radio afterwards
static void saySpeech(const char *text) { audioCmd(AC_SPEECH, 0, text); }

// ───────────── radio page ─────────────
static lv_obj_t *lblStation, *lblTitle, *lblStatus, *lblPlay, *sldVol, *lblSleep;
static lv_obj_t *stRow[sizeof(STATIONS) / sizeof(STATIONS[0])];

static void highlightStation(int idx) {
  for (int i = 0; i < N_STATIONS; i++) {
    bool sel = (i == idx);
    lv_obj_set_style_bg_color(stRow[i], lv_color_hex(sel ? C_CARD3 : C_CARD2), 0);
    lv_obj_set_style_border_width(stRow[i], sel ? 6 : 0, 0);
    lv_obj_set_style_border_color(stRow[i], lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_side(stRow[i], LV_BORDER_SIDE_LEFT, 0);
  }
}

static Preferences prefs;

static void playStation(int i) {
  if (i < 0) i = N_STATIONS - 1;
  if (i >= N_STATIONS) i = 0;
  curStation = i;
  resumeStation = -1;
  prefs.putUChar("st", (uint8_t)i);
  lv_label_set_text(lblStation, STATIONS[i].name);
  highlightStation(i);
  if (xSemaphoreTake(metaMx, portMAX_DELAY) == pdTRUE) { metaTitle[0] = 0; metaDirty = true; xSemaphoreGive(metaMx); }
  playState = 1; wantPlaying = true;
  audioCmd(AC_STATION, i);
}
static void radioStop() { playState = 0; wantPlaying = false; resumeStation = -1; audioCmd(AC_STOP); }
static void setVolume(int v) {
  volume = constrain(v, 0, 21);
  audioCmd(AC_VOLUME, volume);
  if (sldVol) lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  prefs.putUChar("vol", (uint8_t)volume);
}

static void onStationClick(lv_event_t *e) { playStation((int)(intptr_t)lv_event_get_user_data(e)); }
static void onNext(lv_event_t *) { playStation(curStation + 1); }
static void onPrev(lv_event_t *) { playStation(curStation - 1); }
static void onPlayPause(lv_event_t *) {
  if (playState == 0 || playState == 3) playStation(curStation); else radioStop();
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

static void radioBuild(lv_obj_t *pg) {
  lv_obj_t *c = card(pg, 12, 4, 400, 408);
  lv_obj_t *cap = mkLabel(c, "NOW PLAYING", F_S, C_ACCENT);
  lv_obj_set_style_text_letter_space(cap, 3, 0);
  lv_obj_set_pos(cap, 24, 18);

  lblStation = mkLabel(c, STATIONS[curStation].name, F_L, C_TEXT);
  lv_obj_set_width(lblStation, 352);
  lv_label_set_long_mode(lblStation, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lblStation, 24, 44);

  lblTitle = mkLabel(c, STATIONS[curStation].tag, F_M, C_MUTED);
  lv_obj_set_width(lblTitle, 352);
  lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_pos(lblTitle, 24, 90);

  lblStatus = mkLabel(c, "", F_S, C_MUTED);
  lv_obj_set_pos(lblStatus, 24, 126);

  textBtn(c, 28, 170, 84, 84, LV_SYMBOL_PREV, F_L, C_CARD3, onPrev);
  lv_obj_t *play = textBtn(c, 144, 156, 112, 112, LV_SYMBOL_PLAY, F_XL, C_ACCENT, onPlayPause);
  lblPlay = btnLabel(play);
  lv_obj_set_style_bg_grad_color(play, lv_color_hex(C_ACCENT2), 0);
  lv_obj_set_style_bg_grad_dir(play, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_shadow_width(play, 40, 0);
  lv_obj_set_style_shadow_color(play, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(play, LV_OPA_40, 0);
  textBtn(c, 288, 170, 84, 84, LV_SYMBOL_NEXT, F_L, C_CARD3, onNext);

  lv_obj_t *sb = textBtn(c, 24, 288, 190, 46, LV_SYMBOL_BELL "  Sleep: off", F_S, C_CARD2, onSleep);
  lblSleep = btnLabel(sb);

  mkLabel(c, LV_SYMBOL_VOLUME_MID, F_M, C_MUTED);
  lv_obj_t *vlo = mkLabel(c, LV_SYMBOL_VOLUME_MID, F_M, C_MUTED);
  lv_obj_set_pos(vlo, 24, 358);
  lv_obj_t *vhi = mkLabel(c, LV_SYMBOL_VOLUME_MAX, F_M, C_MUTED);
  lv_obj_set_pos(vhi, 352, 358);
  sldVol = lv_slider_create(c);
  lv_slider_set_range(sldVol, 0, 21);
  lv_slider_set_value(sldVol, volume, LV_ANIM_OFF);
  lv_obj_set_size(sldVol, 262, 14);
  lv_obj_set_pos(sldVol, 68, 364);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_CARD3), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sldVol, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sldVol, 12, LV_PART_KNOB);        // fat knob: easy with floury fingers
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sldVol, onVolume, LV_EVENT_RELEASED, nullptr);

  lv_obj_t *side = card(pg, 424, 4, 276, 408);
  lv_obj_t *list = scrollCol(side, 12, 12, 252, 384, 10);
  for (int i = 0; i < N_STATIONS; i++) {
    lv_obj_t *r = lv_button_create(list);
    stRow[i] = r;
    lv_obj_set_size(r, 252, 70);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_t *n = mkLabel(r, STATIONS[i].name, F_M, C_TEXT);
    lv_obj_set_width(n, 215);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(n, 8, 6);
    lv_obj_t *t = mkLabel(r, STATIONS[i].tag, F_XS, C_MUTED);
    lv_obj_set_width(t, 215);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 8, 36);
    lv_obj_add_event_cb(r, onStationClick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  highlightStation(curStation);
}

// called from the 5 Hz tick. Returns the status text so the home page can mirror it.
static const char *radioStatusText = "";
static void radioTick(bool netUp) {
  if (metaDirty && xSemaphoreTake(metaMx, 0) == pdTRUE) {
    char t[160];
    strlcpy(t, metaTitle, sizeof(t));
    metaDirty = false;
    xSemaphoreGive(metaMx);
    lv_label_set_text(lblTitle, t[0] ? t : STATIONS[curStation].tag);
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
