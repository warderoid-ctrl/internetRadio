#pragma once
// Stations: what's playing, favourites (/favs.json) and recently played (/recent.json).
// Threading rule: only the UI task changes favs / recents / curSt (under listMx); other tasks copy what they need
// under listMx and hand results back to the UI (see radio_browser.h, podcasts.h).

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
#define MAX_RECENT 10
#define RECENT_PATH "/recent.json"
static Station *favs = nullptr;            // PSRAM, MAX_FAVS entries
static int nFavs = 0;
static Station *recents = nullptr;         // last played, newest first (PSRAM)
static int nRecent = 0;
static Station curSt;                      // what's playing: a favourite or a search result
static int curStation = -1;                // its index in favs, -1 = not a favourite
static char wxCC[3] = "GB";                // ISO country of the weather location (weather.h sets it; Places lists it first)

static Preferences prefs;
static SemaphoreHandle_t listMx;           // guards favs / recents / curSt (and podcasts.h's pods)
#define LIST_LOCK()   xSemaphoreTake(listMx, portMAX_DELAY)
#define LIST_UNLOCK() xSemaphoreGive(listMx)

static void setStation(Station &s, const char *name, const char *tag, const char *url) {
  copyText(s.name, sizeof(s.name), name);
  copyText(s.tag, sizeof(s.tag), tag);
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

// after LittleFS.begin(): load favourites (seed on first boot), recents and what was playing last
static void favsLoad() {
  if (!listMx) listMx = xSemaphoreCreateMutex();
  if (!favs) favs = (Station *)heap_caps_calloc(MAX_FAVS, sizeof(Station), MALLOC_CAP_SPIRAM);
  if (!recents) recents = (Station *)heap_caps_calloc(MAX_RECENT, sizeof(Station), MALLOC_CAP_SPIRAM);
  if (!favs || !recents) return;
  nFavs = 0;
  if (LittleFS.exists(FAVS_PATH)) nFavs = stationsLoad(FAVS_PATH, favs, MAX_FAVS);
  else {
    for (auto &s : DEFAULT_FAVS) setStation(favs[nFavs++], s.name, s.tag, s.url);
    favsSave();
  }
  if (LittleFS.exists(RECENT_PATH)) nRecent = stationsLoad(RECENT_PATH, recents, MAX_RECENT);
  if (prefs.getBytesLength("cur") == sizeof(Station)) prefs.getBytes("cur", &curSt, sizeof(Station));
  else if (nFavs) curSt = favs[0];
  curStation = favFind(curSt.url);
}

// ── mutations: UI task only ──
static void setCurrent(const Station &s) {                    // remember what's playing (and that it survives a reboot)
  LIST_LOCK();
  if (&s != &curSt) curSt = s;
  curStation = favFind(curSt.url);
  LIST_UNLOCK();
  prefs.putBytes("cur", &curSt, sizeof(Station));
}
// put a station at the front of the recently played list (no duplicates); podcast episodes don't count
static void recentPush(const Station &st) {
  if (!recents || !st.url[0] || st.kind == 1) return;
  LIST_LOCK();
  int i = 0;
  while (i < nRecent && strcmp(recents[i].url, st.url)) i++;
  if (i == nRecent) i = (nRecent < MAX_RECENT) ? nRecent++ : MAX_RECENT - 1;   // new: drop the oldest if full
  memmove(&recents[1], &recents[0], sizeof(Station) * i);
  recents[0] = st;
  LIST_UNLOCK();
  stationsSave(RECENT_PATH, recents, nRecent);
}
// add / remove a favourite; false if the list is full
static bool favToggleData(const Station &s) {
  LIST_LOCK();
  int i = favFind(s.url);
  bool ok = true;
  if (i >= 0) { memmove(&favs[i], &favs[i + 1], sizeof(Station) * (nFavs - i - 1)); nFavs--; }
  else if (nFavs < MAX_FAVS) favs[nFavs++] = s;
  else ok = false;
  curStation = favFind(curSt.url);
  LIST_UNLOCK();
  if (ok) favsSave();
  return ok;
}
// rename what's playing, its favourite and its recent entry
static void renameData(const char *name) {
  LIST_LOCK();
  copyText(curSt.name, sizeof(curSt.name), name);
  bool f = curStation >= 0, r = false;
  if (f) strlcpy(favs[curStation].name, curSt.name, sizeof(favs[0].name));
  for (int i = 0; i < nRecent; i++) if (!strcmp(recents[i].url, curSt.url)) { strlcpy(recents[i].name, curSt.name, sizeof(recents[0].name)); r = true; }
  LIST_UNLOCK();
  prefs.putBytes("cur", &curSt, sizeof(Station));
  if (f) favsSave();
  if (r) stationsSave(RECENT_PATH, recents, nRecent);
}
// a logo was found for url: store it everywhere that station appears
static void logoApply(const char *url, const char *logo) {
  bool c = false, f = false, r = false;
  LIST_LOCK();
  if (!strcmp(curSt.url, url)) { strlcpy(curSt.logo, logo, sizeof(curSt.logo)); c = true; }
  for (int i = 0; i < nFavs; i++) if (!strcmp(favs[i].url, url)) { strlcpy(favs[i].logo, logo, sizeof(favs[0].logo)); f = true; }
  for (int i = 0; i < nRecent; i++) if (!strcmp(recents[i].url, url)) { strlcpy(recents[i].logo, logo, sizeof(recents[0].logo)); r = true; }
  LIST_UNLOCK();
  if (c) prefs.putBytes("cur", &curSt, sizeof(Station));
  if (f) favsSave();
  if (r) stationsSave(RECENT_PATH, recents, nRecent);
}
// other tasks: a consistent copy of what's playing
static void currentCopy(Station &out) { LIST_LOCK(); out = curSt; LIST_UNLOCK(); }
