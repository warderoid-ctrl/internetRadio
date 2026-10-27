#pragma once
// Podcasts: followed shows (/pods.json), Apple's podcast directory for search (no key), episodes read straight from
// the show's RSS feed. SoundCloud accounts publish such feeds too (Benji B, The Lot Radio). "New" merges the latest
// episodes of every followed show (refreshed every few hours).
// Threading: pods / eps / podHits are the UI's; netTask fills its own buffers and hands them over the same way as
// radio_browser.h (want / done request numbers), so the UI never sees a half-written list.

struct Pod { char name[64]; char author[48]; char feed[256]; char art[160]; };   // art: cover image URL
#define MAX_PODS 20
#define PODS_PATH "/pods.json"
static const struct { const char *name, *author, *feed; } DEFAULT_PODS[] = {
  {"Benji B",       "Weekly show (SoundCloud)",   "https://feeds.soundcloud.com/users/soundcloud:users:11122/sounds.rss"},
  {"The Lot Radio", "Brooklyn sets (SoundCloud)", "https://feeds.soundcloud.com/users/soundcloud:users:90833894/sounds.rss"},
};
static Pod *pods = nullptr;                                   // followed shows (PSRAM; changed by the UI under listMx)
static int nPods = 0;

// one show's episodes
struct Episode { char title[96]; char when[24]; char url[256]; };
#define MAX_EPS 30
static Episode *epIn = nullptr, *eps = nullptr;               // netTask's buffer / the UI's copy (PSRAM)
static volatile int epInN = 0;                                // -1 = feed failed
static int nEps = 0;
static char epFeed[256];                                      // request: the show's feed
static char epInArt[160], epArt[160];                         // cover found in that feed (netTask's / the UI's)
static volatile uint16_t epWant = 0, epDone = 0;

// directory search
#define POD_HITS 20
static Pod *podIn = nullptr, *podHits = nullptr;              // netTask's buffer / the UI's copy (PSRAM)
static volatile int podInN = 0;                               // -1 = search failed
static int nPodHits = 0;
static char podQuery[48];
static volatile uint16_t podWant = 0, podDone = 0;

// "New": latest episodes across followed shows, newest first
struct NewEp { Episode e; char show[48]; char art[160]; uint32_t day; };   // day = yyyymmdd, for sorting
#define MAX_NEW 40
#define NEW_PER_SHOW 3
#define NEW_PATH "/newEps.json"
static NewEp *newEps = nullptr;                               // PSRAM; netTask replaces it under DATA_LOCK
static volatile int nNew = 0;
static volatile uint8_t newVer = 0;
static volatile bool reqNewEps = false;
static volatile bool newLoading = false;
struct PodArt { char feed[256]; char art[160]; };             // covers found while refreshing "New", for the UI to keep
static PodArt *newArt = nullptr;
static volatile int nNewArt = 0;
static volatile bool newArtReady = false;

// played episodes: small ring of URL hashes in NVS, so "New" can mark what you haven't heard (UI only)
#define MAX_PLAYED 100
static uint32_t played[MAX_PLAYED];
static int nPlayed = 0, playedPos = 0;
static uint32_t urlHash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static bool podIsPlayed(const char *url) {
  uint32_t h = urlHash(url);
  for (int i = 0; i < nPlayed; i++) if (played[i] == h) return true;
  return false;
}
static void podMarkPlayed(const char *url) {
  if (podIsPlayed(url)) return;
  played[playedPos] = urlHash(url);
  playedPos = (playedPos + 1) % MAX_PLAYED;
  if (nPlayed < MAX_PLAYED) nPlayed++;
  prefs.putBytes("played", played, sizeof(uint32_t) * nPlayed);
  prefs.putUShort("playedPos", playedPos);
}

// ───────────── followed shows ─────────────
static void podsSave() {                                      // UI
  File f = LittleFS.open(PODS_PATH, "w");
  if (!f) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nPods; i++) { JsonObject o = a.add<JsonObject>(); o["n"] = pods[i].name; o["a"] = pods[i].author; o["f"] = pods[i].feed; if (pods[i].art[0]) o["i"] = pods[i].art; }
  serializeJson(d, f);
  f.close();
}
static void setPod(Pod &p, const char *name, const char *author, const char *feed) {
  copyText(p.name, sizeof(p.name), name);
  copyText(p.author, sizeof(p.author), author);
  strlcpy(p.feed, feed, sizeof(p.feed));
  p.art[0] = 0;
}
static int podFind(const char *feed) {
  for (int i = 0; i < nPods; i++) if (!strcmp(pods[i].feed, feed)) return i;
  return -1;
}
static bool podToggle(const Pod &p) {                        // UI: follow / unfollow; true if followed afterwards
  LIST_LOCK();
  int i = podFind(p.feed);
  bool ok = true;
  if (i >= 0) { memmove(&pods[i], &pods[i + 1], sizeof(Pod) * (nPods - i - 1)); nPods--; }
  else if (nPods < MAX_PODS) pods[nPods++] = p;
  else ok = false;
  LIST_UNLOCK();
  if (!ok) return false;
  podsSave();
  reqNewEps = true;                                           // refresh "New" for the changed follow list
  return i < 0;
}
static void podArtApply(const char *feed, const char *art) { // UI: keep a cover found in a show's feed
  if (!art[0]) return;
  LIST_LOCK();
  int i = podFind(feed);
  bool changed = i >= 0 && strcmp(pods[i].art, art);
  if (changed) strlcpy(pods[i].art, art, sizeof(pods[0].art));
  LIST_UNLOCK();
  if (changed) podsSave();
}

static uint32_t dayKey(const char *when) {                    // "21 Oct 2026" -> 20261021
  static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int d = 0, y = 0; char mon[4] = "";
  if (sscanf(when, "%d %3s %d", &d, mon, &y) != 3) return 0;
  const char *p = strstr(M, mon);
  return (uint32_t)y * 10000 + (p ? (p - M) / 3 + 1 : 0) * 100 + d;
}
static void newEpsSave() {                                    // netTask (its own file)
  File f = LittleFS.open(NEW_PATH, "w");
  if (!f) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nNew; i++) {
    JsonObject o = a.add<JsonObject>();
    o["t"] = newEps[i].e.title; o["w"] = newEps[i].e.when; o["u"] = newEps[i].e.url; o["s"] = newEps[i].show; o["d"] = newEps[i].day; o["a"] = newEps[i].art;
  }
  serializeJson(d, f);
  f.close();
}
static void newEpsLoad() {
  File f = LittleFS.open(NEW_PATH, "r");
  if (!f) return;
  JsonDocument d;
  int n = 0;
  if (!deserializeJson(d, f))
    for (JsonObject o : d.as<JsonArray>()) {
      if (n >= MAX_NEW) break;
      NewEp &x = newEps[n++];
      strlcpy(x.e.title, o["t"] | "", sizeof(x.e.title)); strlcpy(x.e.when, o["w"] | "", sizeof(x.e.when));
      strlcpy(x.e.url, o["u"] | "", sizeof(x.e.url)); strlcpy(x.show, o["s"] | "", sizeof(x.show));
      x.day = o["d"] | 0;
      strlcpy(x.art, o["a"] | "", sizeof(x.art));
    }
  f.close();
  nNew = n;
}
template <typename T> static T *psCalloc(size_t n) { return (T *)heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM); }
static void podsLoad() {                                      // after LittleFS.begin() and favsLoad() (listMx)
  pods = psCalloc<Pod>(MAX_PODS);
  epIn = psCalloc<Episode>(MAX_EPS);  eps = psCalloc<Episode>(MAX_EPS);
  podIn = psCalloc<Pod>(POD_HITS);    podHits = psCalloc<Pod>(POD_HITS);
  newEps = psCalloc<NewEp>(MAX_NEW);  newArt = psCalloc<PodArt>(MAX_PODS);
  size_t pb = prefs.getBytesLength("played");
  if (pb && pb <= sizeof(played)) { prefs.getBytes("played", played, pb); nPlayed = pb / sizeof(uint32_t); playedPos = prefs.getUShort("playedPos", 0) % MAX_PLAYED; }
  if (newEps) newEpsLoad();
  if (!pods) return;
  nPods = 0;
  File f = LittleFS.open(PODS_PATH, "r");
  if (f) {
    JsonDocument d;
    if (!deserializeJson(d, f))
      for (JsonObject o : d.as<JsonArray>()) { if (nPods >= MAX_PODS) break; setPod(pods[nPods], o["n"] | "", o["a"] | "", o["f"] | ""); strlcpy(pods[nPods++].art, o["i"] | "", sizeof(pods[0].art)); }
    f.close();
  } else {
    for (auto &p : DEFAULT_PODS) setPod(pods[nPods++], p.name, p.author, p.feed);
    podsSave();
  }
}

// ───────────── RSS feed reader: streams the feed and stops after `max` episodes (feeds can be megabytes) ─────────────
class RssSink : public Stream {
public:
  RssSink(Episode *out, int max) : out(out), max(max) { chanArt[0] = 0; }
  int n = 0;
  char chanArt[160];                                          // the show's cover image
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t len) override {
    if (n >= max) return 0;                                   // short write: HTTPClient stops reading
    for (size_t i = 0; i < len; i++) feed((char)b[i]);
    return len;
  }
private:
  Episode *out;
  int max;
  char tag[320]; int tl = 0; bool inTag = false;
  char text[200]; int xl = 0;
  enum Field : uint8_t { F_NONE, F_TITLE, F_DATE } field = F_NONE;
  bool inItem = false;
  Episode cur;
  void addText(const char *s, int k) { for (int i = 0; i < k && xl < (int)sizeof(text) - 1; i++) text[xl++] = s[i]; }
  static void unescape(char *s) {                             // the common XML entities
    static const struct { const char *e; char c; } ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
    char *w = s;
    for (char *r = s; *r;) {
      bool hit = false;
      for (auto &x : ents) { size_t l = strlen(x.e); if (!strncmp(r, x.e, l)) { *w++ = x.c; r += l; hit = true; break; } }
      if (!hit) *w++ = *r++;
    }
    *w = 0;
  }
  void endField() {
    text[xl] = 0;
    unescape(text);
    if (field == F_TITLE) copyText(cur.title, sizeof(cur.title), text);
    else if (field == F_DATE) {                               // "Wed, 21 Oct 2026 18:00:00 +0000" -> "21 Oct 2026"
      const char *p = strchr(text, ',');
      p = p ? p + 1 : text;
      while (*p == ' ') p++;
      strlcpy(cur.when, p, sizeof(cur.when));
      if (strlen(cur.when) > 11) cur.when[11] = 0;
    }
    field = F_NONE; xl = 0;
  }
  static void attr(const char *tag, const char *name, char *out, size_t n) {   // name="value" from a tag
    const char *u = strstr(tag, name);
    if (!u) return;
    u += strlen(name);
    const char *e = strchr(u, '"');
    if (e && (size_t)(e - u) < n) { memcpy(out, u, e - u); out[e - u] = 0; }
  }
  void onTag() {
    tag[tl] = 0;
    if (!strncmp(tag, "![CDATA[", 8)) {                       // <![CDATA[ text ]]> inside a field
      if (field != F_NONE) { int k = tl - 8; if (k >= 2 && tag[tl - 1] == ']' && tag[tl - 2] == ']') k -= 2; addText(tag + 8, k); }
      return;
    }
    if (field != F_NONE && tag[0] == '/') endField();
    if (!strcmp(tag, "item") || !strncmp(tag, "item ", 5)) { inItem = true; memset(&cur, 0, sizeof(cur)); return; }
    if (!strcmp(tag, "/item")) {
      inItem = false;
      if (cur.url[0] && n < max) { if (!cur.title[0]) strlcpy(cur.title, "Episode", sizeof(cur.title)); out[n++] = cur; }
      return;
    }
    if (!inItem) {
      if (!chanArt[0] && !strncmp(tag, "itunes:image ", 13)) attr(tag, "href=\"", chanArt, sizeof(chanArt));
      return;
    }
    if (!strcmp(tag, "title")) { field = F_TITLE; xl = 0; }
    else if (!strcmp(tag, "pubDate")) { field = F_DATE; xl = 0; }
    else if (!strncmp(tag, "enclosure ", 10)) { attr(tag, "url=\"", cur.url, sizeof(cur.url)); unescape(cur.url); }
  }
  void feed(char c) {
    if (inTag) {
      if (c == '>') { inTag = false; onTag(); }
      else if (tl < (int)sizeof(tag) - 1) tag[tl++] = c;
      return;
    }
    if (c == '<') { inTag = true; tl = 0; return; }
    if (field != F_NONE) addText(&c, 1);
  }
};

// ───────────── one show's episodes ─────────────
static void podEpisodesRequest(const char *feed) { strlcpy(epFeed, feed, sizeof(epFeed)); epWant++; }   // UI
static void podEpisodesFetch() {                              // netTask, when epWant != epDone
  uint16_t want = epWant;
  char feed[256]; strlcpy(feed, epFeed, sizeof(feed));
  RssSink sink(epIn, MAX_EPS);
  int code = (epIn && eps) ? httpStream(feed, sink) : -1;
  epInN = (code == 200 || sink.n > 0) ? sink.n : -1;          // stopping early reports a write error: that's fine
  strlcpy(epInArt, sink.chanArt, sizeof(epInArt));
  epDone = want;
}
static bool podEpisodesTake() {                               // UI: true once when the latest episodes have arrived
  static uint16_t seen = 0;
  if (epDone != epWant || epDone == seen) return false;
  seen = epDone;
  nEps = epInN;
  if (nEps > 0) memcpy(eps, epIn, sizeof(Episode) * nEps);
  strlcpy(epArt, epInArt, sizeof(epArt));
  podArtApply(epFeed, epArt);
  return true;
}

// ───────────── "New": the first few episodes of every followed show, merged newest first ─────────────
static void newEpsFetch() {                                   // netTask
  if (!newEps) return;
  LIST_LOCK();                                                // the follow list can change while we work: copy it
  int np = nPods;
  Pod *snap = np ? psCalloc<Pod>(np) : nullptr;
  if (snap) memcpy(snap, pods, sizeof(Pod) * np);
  LIST_UNLOCK();
  if (np == 0) { DATA_LOCK(); nNew = 0; DATA_UNLOCK(); newVer++; return; }
  NewEp *all = psCalloc<NewEp>(np * NEW_PER_SHOW);
  if (!snap || !all) { free(snap); free(all); return; }
  newLoading = true;
  int n = 0, na = 0;
  for (int i = 0; i < np; i++) {
    Episode buf[NEW_PER_SHOW];
    RssSink sink(buf, NEW_PER_SHOW);
    httpStream(snap[i].feed, sink);
    for (int k = 0; k < sink.n; k++) {
      NewEp &x = all[n++];
      x.e = buf[k];
      strlcpy(x.show, snap[i].name, sizeof(x.show));
      strlcpy(x.art, sink.chanArt[0] ? sink.chanArt : snap[i].art, sizeof(x.art));
      x.day = dayKey(buf[k].when);
    }
    if (sink.chanArt[0] && strcmp(sink.chanArt, snap[i].art) && !newArtReady && newArt) {
      strlcpy(newArt[na].feed, snap[i].feed, sizeof(newArt[0].feed));
      strlcpy(newArt[na].art, sink.chanArt, sizeof(newArt[0].art));
      na++;
    }
  }
  std::sort(all, all + n, [](const NewEp &a, const NewEp &b) { return a.day > b.day; });
  if (n > 0) {                                                // keep the old list if everything failed (no Wi-Fi)
    DATA_LOCK();
    nNew = std::min(n, MAX_NEW);
    memcpy(newEps, all, sizeof(NewEp) * nNew);
    DATA_UNLOCK();
    newEpsSave();
  }
  if (na) { nNewArt = na; newArtReady = true; }               // the UI keeps these covers (podsTick)
  free(snap); free(all);
  newLoading = false;
  newVer++;
}
static void podsTick() {                                      // UI: apply covers found while refreshing "New"
  if (!newArtReady) return;
  for (int i = 0; i < nNewArt; i++) podArtApply(newArt[i].feed, newArt[i].art);
  newArtReady = false;
}

// ───────────── Apple's podcast directory (free, no key) ─────────────
static void podSearchRequest(const char *q) { strlcpy(podQuery, q, sizeof(podQuery)); podWant++; }   // UI
static void podSearchFetch() {                                // netTask, when podWant != podDone
  uint16_t want = podWant;
  String url = "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=20&term=";
  urlEncodeAppend(url, podQuery);
  String body;
  int n = -1;
  if (podIn && podHits && httpGet(url.c_str(), body) == 200) {
    JsonDocument filter;
    filter["results"][0]["collectionName"] = true; filter["results"][0]["artistName"] = true; filter["results"][0]["feedUrl"] = true; filter["results"][0]["artworkUrl100"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      n = 0;
      for (JsonObject r : doc["results"].as<JsonArray>()) {
        if (n >= POD_HITS) break;
        const char *f = r["feedUrl"] | "";
        if (!f[0] || strlen(f) >= sizeof(Pod::feed)) continue;
        setPod(podIn[n], r["collectionName"] | "", r["artistName"] | "", f);
        const char *art = r["artworkUrl100"] | "";
        if (strlen(art) < sizeof(podIn[0].art)) strlcpy(podIn[n].art, art, sizeof(podIn[0].art));
        n++;
      }
    }
  }
  podInN = n;
  podDone = want;
}
static bool podSearchTake() {                                 // UI
  static uint16_t seen = 0;
  if (podDone != podWant || podDone == seen) return false;
  seen = podDone;
  nPodHits = podInN;
  if (nPodHits > 0) memcpy(podHits, podIn, sizeof(Pod) * nPodHits);
  return true;
}
