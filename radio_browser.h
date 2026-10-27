#pragma once
// Online station data, fetched by netTask: radio-browser.info (search, genres, countries, logos; no key) and the
// BBC's now-playing feed. Hand-over rule: netTask never touches UI-owned data. A request carries a number (want);
// netTask fills its own buffer and sets done = want; the UI copies the buffer only when done == want, i.e. it is the
// answer to the latest request and nothing is writing it.

// ───────────── station lists: search / genre / country ─────────────
struct RbHit { Station s; char detail[64]; };
#define RB_MAX 40
enum RbReq : uint8_t { RB_SEARCH, RB_BY_GENRE, RB_BY_PLACE };
static RbHit *rbIn = nullptr;                                 // netTask's buffer (PSRAM)
static volatile int rbInN = 0;                                // -1 = request failed
static RbHit *rbHits = nullptr;                               // the UI's copy (PSRAM)
static int rbN = 0;
static char rbQuery[48];                                      // search text, genre tag or country code
static volatile uint8_t rbType = RB_SEARCH;
static volatile uint16_t rbWant = 0, rbDone = 0;

static void rbRequest(uint8_t type, const char *q) {          // UI
  strlcpy(rbQuery, q, sizeof(rbQuery));
  rbType = type;
  rbWant++;
}
static bool rbTake() {                                        // UI: true once when the latest results have arrived
  static uint16_t seen = 0;
  if (rbDone != rbWant || rbDone == seen) return false;
  seen = rbDone;
  rbN = rbInN;
  if (rbN > 0) memcpy(rbHits, rbIn, sizeof(RbHit) * rbN);
  return true;
}

static int rbGet(const char *path, const char *arg, const char *params, String &body) {
  String url = String("https://de1.api.radio-browser.info/json/") + path;
  if (arg) urlEncodeAppend(url, arg);
  url += params;
  return httpGet(url.c_str(), body);
}
// what the audio library can play. HLS (.m3u8, e.g. all BBC stations) is listed with codec "UNKNOWN" but works.
static bool rbIsHls(const char *url) { return strstr(url, ".m3u8") != nullptr; }
static bool rbCodecOk(const char *c, const char *url) {
  return !strcasecmp(c, "MP3") || !strcasecmp(c, "AAC") || !strcasecmp(c, "AAC+") || !strcasecmp(c, "OGG") ||
         !strcasecmp(c, "FLAC") || rbIsHls(url);
}
static int rbFetch(const char *endpoint, const char *q, int n) {   // appends stations to rbIn, returns the new count (or -1)
  String body;
  if (rbGet(endpoint, q, "?limit=60&hidebroken=true&order=clickcount&reverse=true", body) != 200) return n ? n : -1;
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
    for (int i = 0; i < n; i++) if (!strcmp(rbIn[i].s.url, u)) dup = true;
    if (dup) continue;
    RbHit &h = rbIn[n++];
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
    copyText(h.detail, sizeof(h.detail), det);
  }
  return n;
}
static void radioStationsFetch() {                            // netTask, when rbWant != rbDone
  uint16_t want = rbWant;
  uint8_t type = rbType;
  char q[48]; strlcpy(q, rbQuery, sizeof(q));
  if (!rbIn) rbIn = (RbHit *)heap_caps_calloc(RB_MAX, sizeof(RbHit), MALLOC_CAP_SPIRAM);
  if (!rbHits) rbHits = (RbHit *)heap_caps_calloc(RB_MAX, sizeof(RbHit), MALLOC_CAP_SPIRAM);
  int n = -1;
  if (rbIn && rbHits) {
    if (type == RB_SEARCH) {
      n = rbFetch("stations/byname/", q, 0);
      if (n >= 0 && n < 5) n = rbFetch("stations/bytag/", q, n);   // "jazz", "ambient"...: genres too
    } else if (type == RB_BY_GENRE) n = rbFetch("stations/bytagexact/", q, 0);
    else n = rbFetch("stations/bycountrycodeexact/", q, 0);
  }
  rbInN = n;
  rbDone = want;
}

// ───────────── genres: hand-picked (radio-browser tags are free text and the popular ones are noisy), 20+ stations each ─────────────
static const struct { const char *name, *tag; } GENRES[] = {
  {"Pop", "pop"}, {"Rock", "rock"}, {"Jazz", "jazz"}, {"Classical", "classical"}, {"Dance", "dance"},
  {"Electronic", "electronic"}, {"House", "house"}, {"Deep House", "deep house"}, {"Techno", "techno"},
  {"Trance", "trance"}, {"Drum & Bass", "drum and bass"}, {"Dubstep", "dubstep"}, {"Jungle", "jungle"},
  {"Breakbeat", "breakbeat"}, {"Ambient", "ambient"}, {"Chillout", "chillout"}, {"Chill", "chill"},
  {"Lounge", "lounge"}, {"Downtempo", "downtempo"}, {"Lo-fi", "lofi"}, {"Trip-Hop", "trip-hop"},
  {"Indie", "indie"}, {"Indie Rock", "indie rock"}, {"Alternative", "alternative"}, {"Punk", "punk"},
  {"Hardcore", "hardcore"}, {"Grunge", "grunge"}, {"Metal", "metal"}, {"Hard Rock", "hard rock"},
  {"Classic Rock", "classic rock"}, {"Blues", "blues"}, {"Blues Rock", "blues rock"}, {"Rockabilly", "rockabilly"},
  {"Soul", "soul"}, {"Motown", "motown"}, {"Funk", "funk"}, {"R&B", "rnb"}, {"Hip Hop", "hip hop"}, {"Rap", "rap"},
  {"Reggae", "reggae"}, {"Dancehall", "dancehall"}, {"Ska", "ska"}, {"Dub", "dub"}, {"Afrobeats", "afrobeats"},
  {"Latin", "latin"}, {"Salsa", "salsa"}, {"Reggaeton", "reggaeton"}, {"Cumbia", "cumbia"}, {"Bachata", "bachata"},
  {"Merengue", "merengue"}, {"Bossa Nova", "bossa nova"}, {"Samba", "samba"}, {"MPB", "mpb"},
  {"Sertanejo", "sertanejo"}, {"Tango", "tango"}, {"Mariachi", "mariachi"}, {"Country", "country"},
  {"Americana", "americana"}, {"Bluegrass", "bluegrass"}, {"Folk", "folk"}, {"Celtic", "celtic"},
  {"World", "world music"}, {"Arabic", "arabic"}, {"Greek", "greek"}, {"Bollywood", "bollywood"},
  {"K-Pop", "k-pop"}, {"J-Pop", "j-pop"}, {"Japanese", "japanese"}, {"Chanson", "chanson"},
  {"Schlager", "schlager"}, {"Oldies", "oldies"}, {"50s", "50s"}, {"60s", "60s"}, {"70s", "70s"}, {"80s", "80s"},
  {"90s", "90s"}, {"2000s", "2000s"}, {"Top 40", "top 40"}, {"Hits", "hits"}, {"Disco", "disco"},
  {"New Wave", "new wave"}, {"Synthwave", "synthwave"}, {"EDM", "edm"}, {"Hardstyle", "hardstyle"},
  {"Garage", "garage"}, {"Gospel", "gospel"}, {"Christian", "christian"}, {"Soundtrack", "soundtrack"},
  {"Piano", "piano"}, {"Opera", "opera"}, {"Baroque", "baroque"}, {"Smooth Jazz", "smooth jazz"},
  {"Swing", "swing"}, {"Big Band", "big band"}, {"Easy Listening", "easy listening"},
  {"Instrumental", "instrumental"}, {"Acoustic", "acoustic"}, {"Singer-Songwriter", "singer-songwriter"},
  {"Eclectic", "eclectic"}, {"Experimental", "experimental"}, {"Psychedelic", "psychedelic"},
  {"New Age", "new age"}, {"Meditation", "meditation"}, {"Nature", "nature"}, {"Kids", "kids"},
  {"News", "news"}, {"Talk", "talk"}, {"Sports", "sports"}, {"Comedy", "comedy"}, {"Public Radio", "public radio"},
};
static const int N_GENRES = sizeof(GENRES) / sizeof(GENRES[0]);

// ───────────── countries: loaded once on demand; the weather location's country first ─────────────
struct RbCat { char name[36]; char key[36]; int count; };     // a country (key = ISO code)
#define RB_CATS 80
static RbCat *rbPlaces = nullptr;                             // written once by netTask, then read-only for the UI
static volatile int nPlaces = -2;                             // -2 not loaded, -1 failed
static volatile uint8_t rbCatVer = 0;
static volatile bool reqPlaces = false;

static void radioPlacesFetch() {                              // netTask
  if (!rbPlaces) rbPlaces = (RbCat *)heap_caps_calloc(RB_CATS, sizeof(RbCat), MALLOC_CAP_SPIRAM);
  String body;
  int n = -1;
  if (rbPlaces && rbGet("countries", nullptr, "?order=stationcount&reverse=true&hidebroken=true", body) == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      n = 1;                                                  // slot 0 = home country, if listed
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
        copyText(p.name, sizeof(p.name), nm.c_str());
        p.count = cnt;
      }
      if (!home) { memmove(&rbPlaces[0], &rbPlaces[1], sizeof(RbCat) * (n - 1)); n--; }
    }
  }
  nPlaces = n;
  rbCatVer++;
}

// ───────────── logos for stations that have none (favourites, recents, what's playing) ─────────────
// UI picks one and sets logoStage 1; netTask looks it up (by stream URL, then by name) and sets 2; UI applies it.
static char logoReqUrl[256], logoReqName[48], logoRes[160];
static volatile uint8_t logoStage = 0;                        // 0 idle, 1 netTask looking, 2 result for the UI
static uint32_t logoTried[64];
static int nLogoTried = 0;

static bool logoWanted(const Station &s) {                    // UI
  if (s.logo[0] || !s.url[0] || s.kind != 0 || nLogoTried >= 64) return false;
  uint32_t h = artHash(s.url);
  for (int i = 0; i < nLogoTried; i++) if (logoTried[i] == h) return false;
  return true;
}
static bool logoPick() {                                      // UI: start a lookup if any station needs one
  const Station *c = nullptr;
  if (logoWanted(curSt)) c = &curSt;
  for (int i = 0; !c && i < nFavs; i++) if (logoWanted(favs[i])) c = &favs[i];
  for (int i = 0; !c && i < nRecent; i++) if (logoWanted(recents[i])) c = &recents[i];
  if (!c) return false;
  strlcpy(logoReqUrl, c->url, sizeof(logoReqUrl));
  strlcpy(logoReqName, c->name, sizeof(logoReqName));
  logoTried[nLogoTried++] = artHash(c->url);
  logoStage = 1;
  return true;
}
static void radioLogoLookup() {                               // netTask, when logoStage == 1
  logoRes[0] = 0;
  JsonDocument filter;
  filter[0]["favicon"] = true;
  for (int pass = 0; pass < 2 && !logoRes[0]; pass++) {
    String body;
    int code = pass == 0 ? rbGet("stations/byurl?url=", logoReqUrl, "", body)
                         : rbGet("stations/byname/", logoReqName, "?limit=5&hidebroken=true&order=clickcount&reverse=true", body);
    if (code != 200) continue;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) continue;
    for (JsonObject r : doc.as<JsonArray>()) {
      const char *f = r["favicon"] | "";
      if (f[0] && strlen(f) < sizeof(logoRes)) { strlcpy(logoRes, f, sizeof(logoRes)); break; }
    }
  }
  logoStage = 2;
}

// ───────────── BBC now playing: HLS streams carry no track info, the BBC's own feed does ─────────────
static volatile bool reqNowPlaying = false;                    // station changed: ask soon
static bool bbcService(const char *url, char *out, size_t n) { // ".../bbc_6music/..." -> "bbc_6music"
  const char *p = strstr(url, "/bbc_");
  if (!p) return false;
  p++;
  size_t k = 0;
  while (k + 1 < n && ((p[k] >= 'a' && p[k] <= 'z') || (p[k] >= '0' && p[k] <= '9') || p[k] == '_')) { out[k] = p[k]; k++; }
  out[k] = 0;
  return k > 4;
}
static void nowPlayingPoll() {                                // netTask
  Station s;
  currentCopy(s);
  char svc[40];
  if (!bbcService(s.url, svc, sizeof(svc))) return;
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
  setNowPlayingText(t);
}
