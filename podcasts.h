#pragma once
// Podcasts: saved shows (/pods.json), Apple's podcast directory for search (no key), episodes read straight from the
// show's RSS feed. SoundCloud accounts publish such feeds too (Benji B, The Lot Radio). netTask does the fetching.

struct Pod { char name[64]; char author[48]; char feed[256]; };
#define MAX_PODS 20
#define PODS_PATH "/pods.json"
static const struct { const char *name, *author, *feed; } DEFAULT_PODS[] = {
  {"Benji B",       "Weekly show (SoundCloud)",   "https://feeds.soundcloud.com/users/soundcloud:users:11122/sounds.rss"},
  {"The Lot Radio", "Brooklyn sets (SoundCloud)", "https://feeds.soundcloud.com/users/soundcloud:users:90833894/sounds.rss"},
};
static Pod *pods = nullptr;                                   // PSRAM
static int nPods = 0;

struct Episode { char title[96]; char when[24]; char url[256]; };
#define MAX_EPS 30
static Episode *eps = nullptr;                                // PSRAM
static volatile int nEps = 0;                                 // -1 = feed failed
static volatile uint8_t epVer = 0;
static char epFeed[256];
static volatile bool reqEpisodes = false;

#define POD_HITS 20
static Pod *podHits = nullptr;                                // search results (PSRAM)
static volatile int nPodHits = 0;                             // -1 = search failed
static volatile uint8_t podHitVer = 0;
static char podQuery[48];
static volatile bool reqPodSearch = false;

static void podsSave() {
  File f = LittleFS.open(PODS_PATH, "w");
  if (!f) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nPods; i++) { JsonObject o = a.add<JsonObject>(); o["n"] = pods[i].name; o["a"] = pods[i].author; o["f"] = pods[i].feed; }
  serializeJson(d, f);
  f.close();
}
static void setPod(Pod &p, const char *name, const char *author, const char *feed) {
  copyAscii(p.name, sizeof(p.name), name);
  copyAscii(p.author, sizeof(p.author), author);
  strlcpy(p.feed, feed, sizeof(p.feed));
}
static void podsLoad() {                                      // after LittleFS.begin()
  if (!pods) pods = (Pod *)heap_caps_calloc(MAX_PODS, sizeof(Pod), MALLOC_CAP_SPIRAM);
  if (!eps) eps = (Episode *)heap_caps_calloc(MAX_EPS, sizeof(Episode), MALLOC_CAP_SPIRAM);
  if (!podHits) podHits = (Pod *)heap_caps_calloc(POD_HITS, sizeof(Pod), MALLOC_CAP_SPIRAM);
  if (!pods) return;
  nPods = 0;
  File f = LittleFS.open(PODS_PATH, "r");
  if (f) {
    JsonDocument d;
    if (!deserializeJson(d, f))
      for (JsonObject o : d.as<JsonArray>()) { if (nPods >= MAX_PODS) break; setPod(pods[nPods++], o["n"] | "", o["a"] | "", o["f"] | ""); }
    f.close();
  } else {
    for (auto &p : DEFAULT_PODS) setPod(pods[nPods++], p.name, p.author, p.feed);
    podsSave();
  }
}
static int podFind(const char *feed) {
  for (int i = 0; i < nPods; i++) if (!strcmp(pods[i].feed, feed)) return i;
  return -1;
}
static bool podToggle(const Pod &p) {                        // returns true if saved afterwards
  int i = podFind(p.feed);
  if (i >= 0) { memmove(&pods[i], &pods[i + 1], sizeof(Pod) * (nPods - i - 1)); nPods--; }
  else { if (nPods >= MAX_PODS) return false; pods[nPods++] = p; }
  podsSave();
  return i < 0;
}

// ───────────── RSS feed reader: streams the feed and stops after MAX_EPS episodes (feeds can be megabytes) ─────────────
class RssSink : public Stream {
public:
  int n = 0;
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t len) override {
    if (n >= MAX_EPS) return 0;                               // short write: HTTPClient stops reading
    for (size_t i = 0; i < len; i++) feed((char)b[i]);
    return len;
  }
private:
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
    if (field == F_TITLE) copyAscii(cur.title, sizeof(cur.title), text);
    else if (field == F_DATE) {                               // "Wed, 21 Oct 2026 18:00:00 +0000" -> "21 Oct 2026"
      const char *p = strchr(text, ',');
      p = p ? p + 1 : text;
      while (*p == ' ') p++;
      strlcpy(cur.when, p, sizeof(cur.when));
      if (strlen(cur.when) > 11) cur.when[11] = 0;
    }
    field = F_NONE; xl = 0;
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
      if (cur.url[0] && n < MAX_EPS) { if (!cur.title[0]) strlcpy(cur.title, "Episode", sizeof(cur.title)); eps[n++] = cur; }
      return;
    }
    if (!inItem) return;
    if (!strcmp(tag, "title")) { field = F_TITLE; xl = 0; }
    else if (!strcmp(tag, "pubDate")) { field = F_DATE; xl = 0; }
    else if (!strncmp(tag, "enclosure ", 10)) {
      const char *u = strstr(tag, "url=\"");
      if (u) { u += 5; const char *e = strchr(u, '"'); if (e && e - u < (int)sizeof(cur.url)) { memcpy(cur.url, u, e - u); cur.url[e - u] = 0; unescape(cur.url); } }
    }
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
static void podEpisodesFetch() {                              // netTask
  RssSink sink;
  int code = eps ? httpStream(epFeed, sink) : -1;
  nEps = (code == 200 || sink.n > 0) ? sink.n : -1;          // stopping early reports a write error: that's fine
  epVer++;
}

// Apple's podcast directory (free, no key)
static void podSearchFetch() {                                // netTask
  String url = "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=20&term=";
  for (const char *c = podQuery; *c; c++) {
    if (isalnum((unsigned char)*c) || *c == '-' || *c == '.') url += *c;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (unsigned char)*c); url += h; }
  }
  String body;
  int n = -1;
  if (podHits && httpGet(url.c_str(), body) == 200) {
    JsonDocument filter;
    filter["results"][0]["collectionName"] = true; filter["results"][0]["artistName"] = true; filter["results"][0]["feedUrl"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      n = 0;
      for (JsonObject r : doc["results"].as<JsonArray>()) {
        if (n >= POD_HITS) break;
        const char *f = r["feedUrl"] | "";
        if (!f[0] || strlen(f) >= sizeof(Pod::feed)) continue;
        setPod(podHits[n++], r["collectionName"] | "", r["artistName"] | "", f);
      }
    }
  }
  nPodHits = n;
  podHitVer++;
}
