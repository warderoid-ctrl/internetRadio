#pragma once
// The audio task: plays the radio / podcast episodes, voice replies (Google / HA TTS) and chimes, then resumes the
// radio. Other tasks talk to it only through audioCmd() and the volatile state below.

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

static void audioCmd(AudioCmdType t, int a = 0, const char *s = nullptr) {
  AudioCmd c; c.type = t; c.arg = a; c.str[0] = 0;
  if (s) strlcpy(c.str, s, sizeof(c.str));
  xQueueSend(cmdQ, &c, 0);
}
static void setNowPlayingText(const char *t) {               // any task: the "artist - title" line
  if (xSemaphoreTake(metaMx, 100) == pdTRUE) {
    copyAscii(metaTitle, sizeof(metaTitle), t);
    metaDirty = true;
    xSemaphoreGive(metaMx);
  }
}

// ───────────── audio callback (runs inside the audio task) ─────────────
static void onAudioEvent(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_info:
      if (m.msg && strstr(m.msg, "stream ready") && playState == 1) playState = 2;
      if (m.msg && playState == 1) Serial.printf("audio: %s\n", m.msg);   // connection progress, for debugging streams
      break;
    case Audio::evt_streamtitle:
      if (audioMode == 0) setNowPlayingText(m.msg ? m.msg : "");
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

// "duck" the radio while the mic listens, then bring it back (voice.h uses these, from the voice task)
static void radioDuckBegin() {
  if (audioMode == 0 && (playState == 1 || playState == 2)) resumeStation = 1;
  audioCmd(AC_STOP);
}
static void radioDuckEnd() {
  if (resumeStation < 0) return;
  resumeStation = -1;
  Station s;
  currentCopy(s);
  audioCmd(AC_STATION, s.kind, s.url);
}
// speak text through the speaker (TTS) and resume the radio afterwards
static void saySpeech(const char *text) { audioCmd(AC_SPEECH, 0, text); }
