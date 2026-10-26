#pragma once
// "Hey Crow" voice assistant (tap-to-talk).
//   mic (onboard PDM) -> record until you stop talking -> Home Assistant speech-to-text (Whisper)
//   -> on-device commands (time, weather, timers, radio, conversions...) -> anything else goes to HA Assist
//   -> spoken reply via HA text-to-speech (Piper) or, failing that, Google TTS.

enum VAct : uint8_t { VA_TIMER, VA_TIMER_CLEAR, VA_STATION, VA_RESUME, VA_STOP, VA_VOL_DELTA, VA_VOL_SET, VA_NEXT, VA_PREV, VA_PAGE, VA_AMBIENT };
struct VoiceAct { uint8_t type; int a; char s[24]; };
static QueueHandle_t actQ;
static volatile bool voiceReq = false, voiceBusy = false;
static I2SClass micI2S;

static void act(uint8_t t, int a = 0, const char *s = "") {
  VoiceAct v; v.type = t; v.a = a; strlcpy(v.s, s, sizeof(v.s));
  xQueueSend(actQ, &v, 0);
}

// ───────────── recording ─────────────
#define MIC_RATE 16000
#define MIC_GAIN 8
#define MIC_MAX_SEC 8

static void wavHeader(uint8_t *h, uint32_t dataBytes) {
  uint32_t riff = dataBytes + 36, rate = MIC_RATE, br = MIC_RATE * 2; uint16_t ch = 1, bits = 16, fmt = 1, ba = 2; uint32_t sub = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &sub, 4);
  memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
  memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
}

// returns malloc'd WAV (44-byte header + PCM) or nullptr if nobody spoke
static uint8_t *recordUtterance(size_t *wavLen) {
  const size_t maxSamples = (size_t)MIC_RATE * MIC_MAX_SEC;
  uint8_t *wav = (uint8_t *)heap_caps_malloc(44 + maxSamples * 2, MALLOC_CAP_SPIRAM);
  if (!wav) return nullptr;
  int16_t *pcm = (int16_t *)(wav + 44);
  micI2S.setPinsPdmRx(19, 20);
  if (!micI2S.begin(I2S_MODE_PDM_RX, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) { free(wav); return nullptr; }
  const int CH = 320;                                         // 20 ms
  size_t n = 0; int chunk = 0, loud = 0, quiet = 0; bool started = false;
  float noise = 0; int thr = 400;
  while (n + CH <= maxSamples) {
    size_t got = micI2S.readBytes((char *)(pcm + n), CH * 2) / 2;
    if (got < (size_t)CH) break;
    int64_t sum = 0;
    for (int i = 0; i < CH; i++) {
      int32_t v = (int32_t)pcm[n + i] * MIC_GAIN;
      if (v > 32767) v = 32767; if (v < -32768) v = -32768;
      pcm[n + i] = (int16_t)v;
      sum += v < 0 ? -v : v;
    }
    int level = (int)(sum / CH);
    chunk++;
    if (chunk <= 10) { n += 0; continue; }                    // let the mic settle (first 200 ms discarded)
    if (chunk <= 25) { noise += level; if (chunk == 25) thr = (int)(noise / 15 * 2.2f) + 250; n += CH; continue; }
    n += CH;
    if (level > thr) { loud++; quiet = 0; if (loud >= 3) started = true; }
    else { loud = 0; if (started) quiet++; }
    if (started && quiet >= 40) break;                         // 0.8 s of silence after speech
    if (!started && n > (size_t)MIC_RATE * 6) break;           // nobody spoke
  }
  micI2S.end();
#if AUDIO_OUT_EXTERNAL
  // the amp's DIN shares the mic clock pin: hand the pin back to the speaker I2S (I2S1 data out)
  gpio_set_direction((gpio_num_t)DAC_DOUT, GPIO_MODE_OUTPUT);
  esp_rom_gpio_connect_out_signal(DAC_DOUT, I2S1O_SD_OUT_IDX, false, false);
#endif
  if (!started) { free(wav); return nullptr; }
  wavHeader(wav, (uint32_t)(n * 2));
  *wavLen = 44 + n * 2;
  return wav;
}

// ───────────── text helpers ─────────────
static void normalise(const char *in, char *out, size_t n) {
  size_t o = 0; bool sp = true;
  for (size_t i = 0; in[i] && o < n - 1; i++) {
    char c = tolower((unsigned char)in[i]);
    if (isalnum((unsigned char)c) || c == '.') { out[o++] = c; sp = false; }
    else if (!sp) { out[o++] = ' '; sp = true; }
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
}
static bool has(const char *t, const char *w) { return strstr(t, w) != nullptr; }
static bool hasAny(const char *t, std::initializer_list<const char *> ws) { for (auto w : ws) if (strstr(t, w)) return true; return false; }

static int wordNum(const char *w) {
  static const char *u[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
  static const char *tn[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
  if (isdigit((unsigned char)w[0])) return atoi(w);
  for (int i = 0; i < 20; i++) if (!strcmp(w, u[i])) return i;
  for (int i = 2; i < 10; i++) if (!strcmp(w, tn[i])) return i * 10;
  if (!strcmp(w, "a") || !strcmp(w, "an")) return 1;
  return -1;
}
struct Toks { char t[28][14]; int n; };
static void tokenize(const char *s, Toks &k) {
  k.n = 0;
  const char *p = s;
  while (*p && k.n < 28) {
    while (*p == ' ') p++;
    int l = 0;
    while (p[l] && p[l] != ' ') l++;
    if (!l) break;
    int c = l < 13 ? l : 13;
    memcpy(k.t[k.n], p, c); k.t[k.n][c] = 0; k.n++;
    p += l;
  }
}
// "10 minutes", "an hour and a half", "ninety seconds", "2 hours 30 minutes" -> seconds (0 = none)
static int parseDuration(const Toks &k) {
  int total = 0;
  for (int i = 0; i < k.n; i++) {
    int v = wordNum(k.t[i]);
    if (v < 0) continue;
    if (v >= 20 && v % 10 == 0 && i + 1 < k.n) { int w2 = wordNum(k.t[i + 1]); if (w2 > 0 && w2 < 10 && !isdigit((unsigned char)k.t[i + 1][0])) { v += w2; i++; } }
    if (i + 1 >= k.n) break;
    const char *u = k.t[i + 1];
    if (!strncmp(u, "hour", 4)) { total += v * 3600; i++; if (i + 2 < k.n && !strcmp(k.t[i + 1], "and") && !strcmp(k.t[i + 2], "a")) { total += 1800; } }
    else if (!strncmp(u, "min", 3)) { total += v * 60; i++; }
    else if (!strncmp(u, "sec", 3)) { total += v; i++; }
  }
  if (!total && has(k.t[0], "half")) total = 1800;
  return total;
}
static int firstNumber(const Toks &k, int *idx = nullptr) {
  for (int i = 0; i < k.n; i++) { int v = wordNum(k.t[i]); if (v >= 0 && strcmp(k.t[i], "a") && strcmp(k.t[i], "an")) { if (idx) *idx = i; return v; } }
  return -1;
}

// ───────────── conversions and sums for the kitchen ─────────────
static bool kitchenMaths(const char *t, const Toks &k, char *out, size_t n) {
  // plus / minus / times / divided
  if (hasAny(t, {" plus ", " minus ", " times ", " multiplied ", " divided ", " over "})) {
    float a = NAN, b = NAN; int found = 0;
    for (int i = 0; i < k.n; i++) { int v = wordNum(k.t[i]); if (v >= 0 && strcmp(k.t[i], "a") && strcmp(k.t[i], "an")) { if (!found) a = v; else if (found == 1) b = v; found++; } }
    if (found >= 2) {
      float r; const char *op;
      if (has(t, " plus ")) { r = a + b; op = "plus"; } else if (has(t, " minus ")) { r = a - b; op = "minus"; }
      else if (has(t, " times ") || has(t, " multiplied ")) { r = a * b; op = "times"; }
      else { if (b == 0) { snprintf(out, n, "I can't divide by zero."); return true; } r = a / b; op = "divided by"; }
      if (r == (int)r) snprintf(out, n, "%d %s %d is %d.", (int)a, op, (int)b, (int)r);
      else snprintf(out, n, "%d %s %d is about %.2f.", (int)a, op, (int)b, r);
      return true;
    }
  }
  int idx = -1; int v = firstNumber(k, &idx);
  if (v < 0 || idx + 1 >= k.n || !hasAny(t, {"convert", "how many", "how much", "in a", "to"})) return false;
  const char *u = k.t[idx + 1];
  float x = v; const char *res = nullptr; float y = 0; const char *unit = "";
  if (!strncmp(u, "cup", 3))       { y = x * 240; unit = "millilitres"; res = "x"; }
  else if (!strncmp(u, "tablesp", 7) || !strcmp(u, "tbsp")) { y = x * 15; unit = "millilitres"; res = "x"; }
  else if (!strncmp(u, "teasp", 5) || !strcmp(u, "tsp")) { y = x * 5; unit = "millilitres"; res = "x"; }
  else if (!strncmp(u, "ounce", 5) || !strcmp(u, "oz")) { y = x * 28.35f; unit = "grams"; res = "x"; }
  else if (!strncmp(u, "pound", 5) || !strcmp(u, "lb") || !strcmp(u, "lbs")) { y = x * 453.6f; unit = "grams"; res = "x"; }
  else if (!strncmp(u, "pint", 4)) { y = x * 568; unit = "millilitres"; res = "x"; }
  else if (!strncmp(u, "gram", 4) || !strcmp(u, "g")) { y = x / 28.35f; unit = "ounces"; res = "x"; }
  else if (!strncmp(u, "milli", 5) || !strcmp(u, "ml")) { y = x / 240; unit = "cups"; res = "x"; }
  else if (!strncmp(u, "fahr", 4) || !strcmp(u, "f")) { y = (x - 32) * 5 / 9; unit = "degrees Celsius"; res = "x"; }
  else if (!strncmp(u, "cels", 4) || !strcmp(u, "c") || (!strncmp(u, "degree", 6) && has(t, "celsius"))) { y = x * 9 / 5 + 32; unit = "degrees Fahrenheit"; res = "x"; }
  if (!res) return false;
  snprintf(out, n, "%d %s is about %d %s.", v, u, (int)lroundf(y), unit);
  return true;
}

static const char *JOKES[] = {
  "Why did the scarecrow win an award? He was outstanding in his field.",
  "I'm reading a book about anti-gravity. It's impossible to put down.",
  "Why don't eggs tell jokes? They'd crack each other up.",
  "What do you call cheese that isn't yours? Nacho cheese.",
  "I told my wife she was drawing her eyebrows too high. She looked surprised.",
  "Why did the tomato turn red? Because it saw the salad dressing.",
};


// ───────────── intents ─────────────
// returns true if answered locally. Fills reply. May queue UI actions.
static bool localIntent(const char *t, char *reply, size_t rn) {
  Toks k; tokenize(t, k);
  char padded[220]; snprintf(padded, sizeof(padded), " %s ", t);          // so " plus " style matches work at the edges
  struct tm tm; bool haveTime = nowTm(&tm);

  if (hasAny(t, {"what time", "time is it", "the time"}) && haveTime) {
    int h = tm.tm_hour % 12; if (!h) h = 12;
    snprintf(reply, rn, "It's %d:%02d %s.", h, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM"); return true;
  }
  if (hasAny(t, {"what day", "what's the date", "whats the date", "the date", "what date"}) && haveTime) {
    snprintf(reply, rn, "Today is %s %d %s.", DOWF[tm.tm_wday], tm.tm_mday, MON3[tm.tm_mon]); return true;
  }
  // timers
  if (has(t, "timer") || has(t, "remind me in") || has(t, "countdown")) {
    if (hasAny(t, {"cancel", "stop", "clear", "delete", "turn off"})) { act(VA_TIMER_CLEAR); snprintf(reply, rn, "Timers cleared."); return true; }
    int secs = parseDuration(k);
    if (secs > 0 && hasAny(t, {"set", "start", "timer for", "minute timer", "hour timer", "second timer", "remind", "countdown", "make"})) {
      const char *lbl = "Timer";
      if (has(t, "egg")) lbl = "Egg"; else if (has(t, "pasta")) lbl = "Pasta"; else if (has(t, "tea")) lbl = "Tea"; else if (has(t, "rice")) lbl = "Rice"; else if (has(t, "pizza")) lbl = "Pizza";
      act(VA_TIMER, secs, lbl);
      int m = secs / 60, s = secs % 60;
      if (m && s) snprintf(reply, rn, "Timer set for %d minutes %d seconds.", m, s);
      else if (m) snprintf(reply, rn, "Timer set for %d minute%s.", m, m == 1 ? "" : "s");
      else snprintf(reply, rn, "Timer set for %d seconds.", s);
      return true;
    }
    timersSentence(reply, rn); return true;
  }
  if (hasAny(t, {"weather", "temperature outside", "forecast", "will it rain", "is it going to rain", "umbrella", "how cold", "how hot", "how warm"})) {
    DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
    if (!w.valid) { snprintf(reply, rn, "I haven't got the weather yet."); return true; }
    if (has(t, "tomorrow")) snprintf(reply, rn, "Tomorrow: %s, high %d, low %d, %d percent chance of rain.", wxText(w.days[1].code), w.days[1].hi, w.days[1].lo, w.days[1].pop);
    else if (hasAny(t, {"week", "weekend", "next few days"})) {
      int best = 0; for (int i = 1; i < 7; i++) if (w.days[i].pop > w.days[best].pop) best = i;
      snprintf(reply, rn, "This week ranges from %d to %d. %s looks the wettest at %d percent.", w.days[0].lo, w.days[0].hi, w.days[best].dow, w.days[best].pop);
    }
    else if (hasAny(t, {"rain", "umbrella"})) snprintf(reply, rn, "There's a %d percent chance of rain today. %s", w.popToday, w.popToday >= 50 ? "Take a brolly." : "You should be fine.");
    else wxSentence(reply, rn);
    return true;
  }
  if (hasAny(t, {"appointment", "calendar", "schedule", "what's on", "whats on", "agenda", "meeting", "what do i have", "my day"})) {
    if (has(t, "today") || has(t, "my day") || has(t, "agenda")) {
      int c = 0; time_t now = time(nullptr); int64_t today = localDayIndex(now); char list[150] = "";
      DATA_LOCK();
      for (int i = 0; i < nAppts; i++) if (localDayIndex(appts[i].start) == today && appts[i].end > now) {
        c++; if (strlen(list) < 100) { struct tm a; localtime_r(&appts[i].start, &a); char one[60]; snprintf(one, sizeof(one), "%s%s at %d:%02d", c > 1 ? ", " : "", appts[i].title, a.tm_hour, a.tm_min); strlcat(list, one, sizeof(list)); }
      }
      DATA_UNLOCK();
      if (!agendaConfigured) agendaSentence(reply, rn);
      else if (!c) snprintf(reply, rn, "Nothing else on today.");
      else snprintf(reply, rn, "Today you have %d thing%s: %s.", c, c == 1 ? "" : "s", list);
    } else agendaSentence(reply, rn);
    act(VA_PAGE, PG_AGENDA);
    return true;
  }
  // radio
  if (hasAny(t, {"next station", "skip", "next song", "next channel"})) { act(VA_NEXT); snprintf(reply, rn, "Next station."); return true; }
  if (hasAny(t, {"previous station", "last station", "previous channel"})) { act(VA_PREV); snprintf(reply, rn, "Previous station."); return true; }
  if (hasAny(t, {"volume up", "louder", "turn it up", "turn up", "increase the volume"})) { act(VA_VOL_DELTA, 3); snprintf(reply, rn, "Louder."); return true; }
  if (hasAny(t, {"volume down", "quieter", "turn it down", "turn down", "lower the volume", "too loud"})) { act(VA_VOL_DELTA, -3); snprintf(reply, rn, "Quieter."); return true; }
  if (has(t, "volume") || has(t, "set the volume")) { int v = firstNumber(k); if (v >= 0) { act(VA_VOL_SET, constrain(v > 21 ? v / 5 : v, 0, 21)); snprintf(reply, rn, "Volume set."); return true; } }
  if (hasAny(t, {"stop the radio", "turn off the radio", "radio off", "stop the music", "pause", "be quiet", "shut up", "stop playing", "turn off the music"})) { act(VA_STOP); snprintf(reply, rn, ""); return true; }
  if (hasAny(t, {"play", "radio", "music", "listen"})) {
    for (int i = 0; i < N_STATIONS; i++) {
      char nm[40]; normalise(STATIONS[i].name, nm, sizeof(nm));
      char first[24]; strlcpy(first, nm, sizeof(first)); char *sp = strchr(first, ' '); if (sp && strlen(nm) > 5 && !strncmp(nm, "somafm", 6)) { /* "somafm groove salad" -> match tail words */ }
      const char *tail = !strncmp(nm, "somafm ", 7) ? nm + 7 : nm;
      if (strstr(t, tail) || (!strncmp(nm, "radio paradise", 14) && has(t, "paradise")) || (!strncmp(nm, "classic", 7) && has(t, "classic")) || (has(nm, "nts 1") && hasAny(t, {"nts one", "nts 1"})) || (has(nm, "nts 2") && hasAny(t, {"nts two", "nts 2"})) || (has(nm, "kexp") && has(t, "kexp")) || (has(nm, "jazz") && has(t, "jazz"))) {
        act(VA_STATION, i); snprintf(reply, rn, "Playing %s.", STATIONS[i].name); return true;
      }
    }
    act(VA_RESUME); snprintf(reply, rn, "Playing the radio."); return true;
  }
  // navigation / modes
  struct { const char *w; int p; const char *say; } nav[] = {{"weather", PG_WEATHER, ""}, {"timers", PG_TIMERS, ""}, {"house", PG_HOUSE, ""}, {"lights", PG_HOUSE, ""}, {"home screen", PG_HOME, ""}, {"home page", PG_HOME, ""}};
  if (hasAny(t, {"show", "go to", "open", "take me"})) for (auto &x : nav) if (has(t, x.w)) { act(VA_PAGE, x.p); snprintf(reply, rn, "Here you go."); return true; }
  if (hasAny(t, {"photo frame", "screensaver", "screen saver", "night mode", "good night", "goodnight", "clock mode", "ambient"})) { act(VA_AMBIENT); snprintf(reply, rn, "Good night."); return true; }
  // small talk
  if (hasAny(t, {"tell me a joke", "joke", "make me laugh"})) { snprintf(reply, rn, "%s", JOKES[random(sizeof(JOKES) / sizeof(JOKES[0]))]); return true; }
  if (hasAny(t, {"flip a coin", "heads or tails", "toss a coin"})) { snprintf(reply, rn, "%s!", random(2) ? "Heads" : "Tails"); return true; }
  if (hasAny(t, {"roll a dice", "roll a die", "roll the dice"})) { snprintf(reply, rn, "You rolled a %ld.", random(1, 7)); return true; }
  if (hasAny(t, {"thank you", "thanks"})) { snprintf(reply, rn, "You're welcome."); return true; }
  if (hasAny(t, {"who are you", "your name", "what are you"})) { snprintf(reply, rn, "I'm the Crow, the brains of this kitchen."); return true; }
  if (hasAny(t, {"how are you"})) { snprintf(reply, rn, "Running at a comfortable 240 megahertz, thanks."); return true; }
  if (hasAny(t, {"what can you do", "help"})) { snprintf(reply, rn, "Ask me the time, the weather, your appointments, to set timers, play a station, or convert cups and ounces."); return true; }
  if (kitchenMaths(padded, k, reply, rn)) return true;
  return false;
}

// Home Assistant Assist: lights, scenes, "is the front door locked?", etc.
static bool haConversation(const char *text, char *reply, size_t rn) {
  if (!HA_ENABLED) return false;
  char body[320]; snprintf(body, sizeof(body), "{\"text\":\"%s\",\"language\":\"%s\"}", text, HA_LANGUAGE);
  String url = String(HA_URL) + "/api/conversation/process", out;
  int code = httpPostJson(url.c_str(), body, &out, HA_TOKEN, 12000);
  if (code != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, out)) return false;
  const char *sp = doc["response"]["speech"]["plain"]["speech"] | "";
  if (!sp[0]) return false;
  copyAscii(reply, rn, sp);
  reqHa = true;
  return true;
}

// speak: HA TTS (Piper) if configured, else Google TTS via the audio library
static void voiceSpeak(const char *text) {
  if (!text[0]) return;
  if (HA_ENABLED && HA_TTS_ENTITY[0]) {
    char body[400]; snprintf(body, sizeof(body), "{\"engine_id\":\"%s\",\"message\":\"%s\",\"language\":\"%s\"}", HA_TTS_ENTITY, text, HA_LANGUAGE);
    String url = String(HA_URL) + "/api/tts_get_url", out;
    if (httpPostJson(url.c_str(), body, &out, HA_TOKEN, 12000) == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, out)) {
        const char *path = doc["path"] | "";
        if (path[0]) { String full = String(HA_URL) + path; audioCmd(AC_URL, 0, full.c_str()); return; }
      }
    }
  }
  saySpeech(text);
}

static bool sttViaHA(uint8_t *wav, size_t len, char *text, size_t tn) {
  if (!HA_ENABLED) return false;
  String url = String(HA_URL) + "/api/stt/" + HA_STT_ENTITY, out;
  String hdr = String("language=") + HA_LANGUAGE + "; format=wav; codec=pcm; sample_rate=16000; bit_rate=16; channel=1";
  HttpOpts o; o.bearer = HA_TOKEN; o.contentType = "audio/wav"; o.hdrName = "X-Speech-Content"; o.hdrVal = hdr.c_str(); o.timeout = 20000;
  int code = httpDo("POST", url.c_str(), wav, len, o, &out);
  if (code != 200) { Serial.printf("STT -> %d\n", code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, out)) return false;
  const char *tx = doc["text"] | "";
  if (!tx[0]) return false;
  copyAscii(text, tn, tx);
  return true;
}

// ───────────── the voice task ─────────────
static void voiceTask(void *) {
  for (;;) {
    if (!voiceReq) { vTaskDelay(50); continue; }
    voiceReq = false; voiceBusy = true;
    postUi(UM_VOICE_STATE, "Listening...", C_ACCENT);
    bool radioWasOn = (audioMode == 0 && (playState == 1 || playState == 2));
    radioDuckBegin();
    vTaskDelay(250);
    stc(STC_MIC_PATH); stc(STC_SPK_OFF);
    size_t wl = 0;
    uint8_t *wav = recordUtterance(&wl);
    stc(STC_AUDIO_DEF);
#if !AUDIO_OUT_EXTERNAL
    stc(STC_SPK_ON);
#endif
    char heard[160] = "", norm[170] = "", reply[240] = "";
    bool spoke = false;
    if (!wav) {
      postUi(UM_VOICE_RESULT, "I didn't hear anything.", C_MUTED);
      if (radioWasOn) radioDuckEnd();
    } else if (!HA_ENABLED) {
      free(wav);
      postUi(UM_VOICE_RESULT, "Voice needs Home Assistant's Whisper add-on - see the README.", C_WARN);
      if (radioWasOn) radioDuckEnd();
    } else {
      postUi(UM_VOICE_STATE, "Thinking...", C_BLUE);
      bool ok = sttViaHA(wav, wl, heard, sizeof(heard));
      free(wav);
      if (!ok) {
        postUi(UM_VOICE_RESULT, "Couldn't understand that - try again.", C_BAD);
        if (radioWasOn) radioDuckEnd();
      } else {
        normalise(heard, norm, sizeof(norm));
        // strip a leading wake-ish word
        const char *cmd = norm;
        if (!strncmp(cmd, "hey crow ", 9)) cmd += 9; else if (!strncmp(cmd, "crow ", 5)) cmd += 5;
        bool handled = localIntent(cmd, reply, sizeof(reply));
        if (!handled) handled = haConversation(cmd, reply, sizeof(reply));
        if (!handled) snprintf(reply, sizeof(reply), "Sorry, I don't know how to do that yet.");
        char shown[200]; snprintf(shown, sizeof(shown), "\"%s\"\n%s", heard, reply[0] ? reply : "Done.");
        postUi(UM_VOICE_RESULT, shown, C_TEXT);
        if (reply[0]) { voiceSpeak(reply); spoke = true; }
        if (!spoke) { vTaskDelay(150); }                       // let queued actions (station change, stop) land first
        if (!spoke && radioWasOn && resumeStation >= 0) radioDuckEnd();
      }
    }
    voiceBusy = false;
    vTaskDelay(100);
  }
}

static void voiceBegin() {
  actQ = xQueueCreate(8, sizeof(VoiceAct));
  xTaskCreatePinnedToCore(voiceTask, "voice", 16384, nullptr, 2, nullptr, 0);
}
static void voiceTrigger() { if (!voiceBusy) voiceReq = true; }

// UI thread: carry out what the voice task asked for
static void voiceActions() {
  VoiceAct v;
  while (actQ && xQueueReceive(actQ, &v, 0) == pdTRUE) {
    switch (v.type) {
      case VA_TIMER: if (timerStart(v.a, v.s) >= 0) showPage(PG_TIMERS); else postUi(UM_TOAST, "All 3 timers are busy", C_WARN); break;
      case VA_TIMER_CLEAR: for (int i = 0; i < N_SLOTS; i++) if (slots[i].active) timerClear(i); lv_obj_add_flag(alarmOv, LV_OBJ_FLAG_HIDDEN); break;
      case VA_STATION: resumeStation = -1; playStation(v.a); break;
      case VA_RESUME: resumeStation = -1; playStation(curStation); break;
      case VA_STOP: radioStop(); break;
      case VA_VOL_DELTA: setVolume(volume + v.a); break;
      case VA_VOL_SET: setVolume(v.a); break;
      case VA_NEXT: resumeStation = -1; playStation(curStation + 1); break;
      case VA_PREV: resumeStation = -1; playStation(curStation - 1); break;
      case VA_PAGE: showPage(v.a); break;
      case VA_AMBIENT: ambientEnter(); break;
    }
  }
}

// ───────────── on-screen feedback ─────────────
static lv_obj_t *voiceOv, *voiceTitle, *voiceBody, *voiceMicDot;
static uint32_t voiceHideAt = 0;

static void micPulse(void *o, int32_t v) { lv_obj_set_style_shadow_width((lv_obj_t *)o, v, 0); }

static void voiceUiBuild() {
  voiceOv = panel(lv_layer_top(), 110, 250, 640, 210, C_CARD, 32);
  lv_obj_set_style_border_width(voiceOv, 3, 0);
  lv_obj_set_style_border_color(voiceOv, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_width(voiceOv, 40, 0);
  lv_obj_set_style_shadow_opa(voiceOv, LV_OPA_50, 0);
  lv_obj_set_style_shadow_color(voiceOv, lv_color_hex(0x000000), 0);
  lv_obj_add_flag(voiceOv, LV_OBJ_FLAG_HIDDEN);
  voiceMicDot = panel(voiceOv, 24, 24, 76, 76, C_ACCENT, LV_RADIUS_CIRCLE);
  lv_obj_set_style_shadow_color(voiceMicDot, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_shadow_opa(voiceMicDot, LV_OPA_60, 0);
  drawMic(voiceMicDot, 38, 38, 56, C_TEXT);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, voiceMicDot); lv_anim_set_exec_cb(&a, micPulse);
  lv_anim_set_values(&a, 4, 36); lv_anim_set_duration(&a, 700);
  lv_anim_set_playback_duration(&a, 700); lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  voiceTitle = mkLabel(voiceOv, "Listening...", F_L, C_ACCENT);
  lv_obj_set_pos(voiceTitle, 120, 40);
  voiceBody = mkLabel(voiceOv, "", F_M, C_TEXT);
  lv_obj_set_width(voiceBody, 590);
  lv_label_set_long_mode(voiceBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(voiceBody, 24, 112);
}
static void voiceUiState(const char *text, uint32_t col) {
  lv_obj_remove_flag(voiceOv, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(voiceOv);
  lv_label_set_text(voiceTitle, text);
  lv_obj_set_style_text_color(voiceTitle, lv_color_hex(col), 0);
  lv_label_set_text(voiceBody, "");
  voiceHideAt = 0;
}
static void voiceUiResult(const char *text, uint32_t col) {
  lv_obj_remove_flag(voiceOv, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(voiceOv);
  lv_label_set_text(voiceTitle, col == C_TEXT ? "Crow" : "Crow");
  lv_obj_set_style_text_color(voiceTitle, lv_color_hex(C_ACCENT), 0);
  lv_label_set_text(voiceBody, text);
  lv_obj_set_style_text_color(voiceBody, lv_color_hex(col), 0);
  voiceHideAt = millis() + 7000;
}
static void voiceUiTick() {
  if (voiceHideAt && (int32_t)(millis() - voiceHideAt) >= 0) { voiceHideAt = 0; lv_obj_add_flag(voiceOv, LV_OBJ_FLAG_HIDDEN); }
}
