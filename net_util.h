#pragma once
// HTTP helpers used by the background network task (never call these from the UI thread).

struct HttpOpts {
  const char *bearer = nullptr;       // "Authorization: Bearer ..."
  const char *contentType = nullptr;
  const char *hdrName = nullptr;      // one extra header
  const char *hdrVal = nullptr;
  uint32_t timeout = 10000;
};

static void httpPrep(HTTPClient &http, const HttpOpts &o) {
  http.setTimeout(o.timeout);
  http.setConnectTimeout(o.timeout);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("CrowHub/1.0");
  if (o.bearer && o.bearer[0]) http.addHeader("Authorization", String("Bearer ") + o.bearer);
  if (o.contentType) http.addHeader("Content-Type", o.contentType);
  if (o.hdrName) http.addHeader(o.hdrName, o.hdrVal ? o.hdrVal : "");
}

// returns HTTP status (200...) or a negative HTTPClient error. Body goes to *out (if given).
static int httpDo(const char *method, const char *url, const uint8_t *body, size_t blen,
                  const HttpOpts &o, String *out) {
  if (WiFi.status() != WL_CONNECTED) return -100;
  bool https = strncmp(url, "https", 5) == 0;
  WiFiClientSecure sec;
  WiFiClient plain;
  HTTPClient http;
  bool began;
  if (https) { sec.setInsecure(); began = http.begin(sec, url); }     // LAN/hobby use: no cert pinning
  else began = http.begin(plain, url);
  if (!began) return -101;
  httpPrep(http, o);
  int code = http.sendRequest(method, (uint8_t *)body, blen);
  if (code > 0 && out) *out = http.getString();
  http.end();
  return code;
}

static int httpGet(const char *url, String &out, const HttpOpts &o = HttpOpts()) {
  return httpDo("GET", url, nullptr, 0, o, &out);
}
static int httpPostJson(const char *url, const char *json, String *out, const char *bearer = nullptr, uint32_t timeout = 10000) {
  HttpOpts o; o.bearer = bearer; o.contentType = "application/json"; o.timeout = timeout;
  return httpDo("POST", url, (const uint8_t *)json, strlen(json), o, out);
}

// stream a (possibly large, possibly chunked) response into a Stream sink as it arrives
static int httpStream(const char *url, Stream &sink, const HttpOpts &o = HttpOpts()) {
  if (WiFi.status() != WL_CONNECTED) return -100;
  bool https = strncmp(url, "https", 5) == 0;
  WiFiClientSecure sec;
  WiFiClient plain;
  HTTPClient http;
  bool began;
  if (https) { sec.setInsecure(); began = http.begin(sec, url); } else began = http.begin(plain, url);
  if (!began) return -101;
  httpPrep(http, o);
  int code = http.GET();
  if (code == 200) http.writeToStream(&sink);
  http.end();
  return code;
}

static bool timeIsValid() { return time(nullptr) > 1700000000; }
