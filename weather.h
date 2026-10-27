#pragma once
// Weather: Open-Meteo (free, no API key). Today + next hours + 7 days. Fetched by the network task.

struct WxHour { int hour; int temp; uint8_t code; int pop; };
struct WxDay  { char dow[6]; uint8_t wday; uint8_t code; int hi, lo; int pop; };
struct WxData {
  bool valid = false;
  int temp = 0, feels = 0, hum = 0, wind = 0, code = 0, uv = 0, popToday = 0;
  bool day = true;
  char sunrise[8] = "--:--", sunset[8] = "--:--";
  WxHour hours[8];
  WxDay days[7];
  WxHour dayHours[7][8];                        // 00:00, 03:00 ... 21:00 for each day (tap a day)
  uint32_t stamp = 0;
};
static WxData wx;
static volatile bool reqWeather = true;

// ───────────── location: set on the hub (More > Weather location), kept in NVS; config.h only has the default ─────────────
static float wxLat = WEATHER_LAT, wxLon = WEATHER_LON;
static char wxPlace[40] = WEATHER_PLACE;
static void weatherLoadLocation() {                    // after prefs.begin()
  if (!prefs.isKey("wxlat")) return;
  wxLat = prefs.getFloat("wxlat", WEATHER_LAT);
  wxLon = prefs.getFloat("wxlon", WEATHER_LON);
  prefs.getString("wxplace", wxPlace, sizeof(wxPlace));
  prefs.getString("wxcc", wxCC, sizeof(wxCC));
}
static void weatherSaveLocation(float lat, float lon, const char *place, const char *cc) {
  wxLat = lat; wxLon = lon; strlcpy(wxPlace, place, sizeof(wxPlace));
  if (cc[0]) strlcpy(wxCC, cc, sizeof(wxCC));
  prefs.putFloat("wxlat", lat); prefs.putFloat("wxlon", lon); prefs.putString("wxplace", wxPlace); prefs.putString("wxcc", wxCC);
  reqWeather = true;
}

// place search (Open-Meteo geocoding, no key): geoRequest() from the UI, geoFetch() in netTask, geoTake() back in the UI.
struct GeoHit { char name[40]; char detail[64]; char cc[3]; float lat, lon; };
#define GEO_MAX 6
static GeoHit geoIn[GEO_MAX], geoHits[GEO_MAX];         // netTask's buffer / the UI's copy (hand-over as radio_browser.h)
static volatile int geoInN = 0;                         // -1 = search failed
static int geoN = 0;
static char geoQuery[48];
static volatile uint16_t geoWant = 0, geoDone = 0;
static void geoRequest(const char *q) { strlcpy(geoQuery, q, sizeof(geoQuery)); geoWant++; }   // UI
static void geoFetch() {                                // netTask, when geoWant != geoDone
  uint16_t want = geoWant;
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=" + String(GEO_MAX) + "&language=en&format=json&name=";
  urlEncodeAppend(url, geoQuery);
  String body;
  int n = -1;
  if (httpGet(url.c_str(), body) == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      n = 0;
      for (JsonObject r : doc["results"].as<JsonArray>()) {
        if (n >= GEO_MAX) break;
        GeoHit &g = geoIn[n++];
        copyAscii(g.name, sizeof(g.name), r["name"] | "");
        String d = String((const char *)(r["admin1"] | ""));
        const char *country = r["country"] | "";
        if (country[0]) d += d.length() ? String(", ") + country : String(country);
        copyAscii(g.detail, sizeof(g.detail), d.c_str());
        g.lat = r["latitude"] | 0.0f; g.lon = r["longitude"] | 0.0f;
        strlcpy(g.cc, r["country_code"] | "", sizeof(g.cc));
      }
    }
  }
  geoInN = n;
  geoDone = want;
}
static bool geoTake() {                                 // UI: true once when the latest places have arrived
  static uint16_t seen = 0;
  if (geoDone != geoWant || geoDone == seen) return false;
  seen = geoDone;
  geoN = geoInN;
  if (geoN > 0) memcpy(geoHits, geoIn, sizeof(GeoHit) * geoN);
  return true;
}

static uint8_t wxKindOf(int c) {
  if (c == 0 || c == 1) return WX_CLEAR;
  if (c == 2) return WX_PARTLY;
  if (c == 3) return WX_CLOUD;
  if (c == 45 || c == 48) return WX_FOG;
  if (c >= 51 && c <= 57) return WX_DRIZZLE;
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
  if (c >= 95) return WX_STORM;
  return WX_CLOUD;
}
static const char *wxText(int c) {
  switch (c) {
    case 0: return "Clear sky";       case 1: return "Mainly clear";   case 2: return "Partly cloudy";
    case 3: return "Overcast";        case 45: case 48: return "Foggy";
    case 51: case 53: case 55: return "Drizzle";  case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";     case 63: return "Rain";          case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";     case 73: return "Snow";          case 75: return "Heavy snow";
    case 77: return "Snow grains";    case 80: return "Light showers"; case 81: return "Showers";
    case 82: return "Heavy showers";  case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";   case 96: case 99: return "Thunder and hail";
  }
  return "Cloudy";
}

static bool weatherFetch() {
  char url[760];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
    "&hourly=temperature_2m,weather_code,precipitation_probability"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
    "&forecast_days=7&timezone=auto&wind_speed_unit=%s&temperature_unit=%s",
    wxLat, wxLon, USE_MPH ? "mph" : "kmh", USE_CELSIUS ? "celsius" : "fahrenheit");
  String body;
  int code = httpGet(url, body);
  if (code != 200) { Serial.printf("weather HTTP %d\n", code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  WxData n;
  JsonObject cur = doc["current"];
  n.temp  = (int)lroundf(cur["temperature_2m"] | 0.0f);
  n.feels = (int)lroundf(cur["apparent_temperature"] | 0.0f);
  n.hum   = cur["relative_humidity_2m"] | 0;
  n.wind  = (int)lroundf(cur["wind_speed_10m"] | 0.0f);
  n.code  = cur["weather_code"] | 0;
  n.day   = (cur["is_day"] | 1) == 1;
  JsonObject hr = doc["hourly"];
  // hourly runs from 00:00 today (local time) for 7 days: the next hours start at the current hour
  const char *ct = cur["time"] | "";
  int start = (strlen(ct) >= 13) ? atoi(ct + 11) : 0;
  int nh = hr["time"].size();
  for (int d = 0; d < 7; d++)
    for (int s = 0; s < 8; s++) {
      int k = d * 24 + s * 3;
      WxHour &h = n.dayHours[d][s];
      h.hour = s * 3;
      h.temp = (k < nh) ? (int)lroundf(hr["temperature_2m"][k] | 0.0f) : 0;
      h.code = (k < nh) ? (hr["weather_code"][k] | 0) : 0;
      h.pop  = (k < nh) ? (hr["precipitation_probability"][k] | 0) : 0;
    }
  for (int i = 0; i < 8; i++) {
    int k = min(start + i * 2, max(nh - 1, 0));
    const char *t = hr["time"][k] | "";                 // "2026-10-26T16:00"
    n.hours[i].hour = (strlen(t) >= 13) ? atoi(t + 11) : 0;
    n.hours[i].temp = (int)lroundf(hr["temperature_2m"][k] | 0.0f);
    n.hours[i].code = hr["weather_code"][k] | 0;
    n.hours[i].pop  = hr["precipitation_probability"][k] | 0;
  }
  JsonObject dl = doc["daily"];
  for (int i = 0; i < 7; i++) {
    int y = 0, m = 0, d = 0;
    const char *t = dl["time"][i] | "";
    sscanf(t, "%d-%d-%d", &y, &m, &d);
    struct tm tm = {}; tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12;
    mktime(&tm);
    strlcpy(n.days[i].dow, i == 0 ? "Today" : DOW3[tm.tm_wday % 7], sizeof(n.days[i].dow));
    n.days[i].wday = tm.tm_wday % 7;
    n.days[i].code = dl["weather_code"][i] | 0;
    n.days[i].hi = (int)lroundf(dl["temperature_2m_max"][i] | 0.0f);
    n.days[i].lo = (int)lroundf(dl["temperature_2m_min"][i] | 0.0f);
    n.days[i].pop = dl["precipitation_probability_max"][i] | 0;
  }
  const char *sr = dl["sunrise"][0] | "", *ss = dl["sunset"][0] | "";
  if (strlen(sr) >= 16) strlcpy(n.sunrise, sr + 11, 6);
  if (strlen(ss) >= 16) strlcpy(n.sunset, ss + 11, 6);
  n.uv = (int)lroundf(dl["uv_index_max"][0] | 0.0f);
  n.popToday = n.days[0].pop;
  n.valid = true;
  n.stamp = millis();
  DATA_LOCK(); wx = n; DATA_UNLOCK();
  wxVer++;
  return true;
}

// a sentence for the voice assistant
static void wxSentence(char *out, size_t n) {
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  if (!w.valid) { snprintf(out, n, "I haven't got the weather yet."); return; }
  snprintf(out, n, "It's %d degrees and %s in %s. Today's high is %d, low %d, with a %d percent chance of rain.",
           w.temp, wxText(w.code), wxPlace, w.days[0].hi, w.days[0].lo, w.popToday);
}

// ───────────── weather page ─────────────
static lv_obj_t *wxBigIcon, *wxLblTemp, *wxLblCond, *wxLblPlace, *wxStat[6];
static lv_obj_t *wxHrTime[8], *wxHrIcon[8], *wxHrTemp[8];
static lv_obj_t *wxDyName[7], *wxDyIcon[7], *wxDyTemp[7], *wxDyPop[7];
static lv_obj_t *wxDetail, *wxDetTitle, *wxDetSub, *wxDetTime[8], *wxDetIcon[8], *wxDetTemp[8], *wxDetPop[8];

// ───────────── day detail: tap a day for its 3-hourly breakdown ─────────────
static void onWxDetailClose(lv_event_t *) { lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_HIDDEN); }
static void wxDetailBuild() {
  wxDetail = lv_obj_create(lv_layer_top());                    // dim the whole screen, tap anywhere to close
  lv_obj_remove_style_all(wxDetail);
  lv_obj_set_size(wxDetail, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(wxDetail, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(wxDetail, LV_OPA_70, 0);
  lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wxDetail, onWxDetailClose, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *p = card(wxDetail, 40, 60, 720, 360);
  lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
  wxDetTitle = mkLabel(p, "", F_L, C_TEXT);
  lv_obj_set_pos(wxDetTitle, 28, 20);
  wxDetSub = mkLabel(p, "", F_S, C_MUTED);
  lv_obj_set_pos(wxDetSub, 28, 62);
  lv_obj_t *x = textBtn(p, 640, 16, 60, 60, LV_SYMBOL_CLOSE, F_M, C_CARD3, onWxDetailClose);
  (void)x;
  for (int i = 0; i < 8; i++) {
    int cx = 16 + i * 86;
    wxDetTime[i] = mkLabel(p, "--", F_S, C_MUTED);
    lv_obj_set_width(wxDetTime[i], 86); lv_obj_set_style_text_align(wxDetTime[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDetTime[i], cx, 116);
    wxDetIcon[i] = wxIcon(p, cx + 18, 148, 50, WX_CLOUD, false);
    wxDetTemp[i] = mkLabel(p, "--", F_M, C_TEXT);
    lv_obj_set_width(wxDetTemp[i], 86); lv_obj_set_style_text_align(wxDetTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDetTemp[i], cx, 214);
    wxDetPop[i] = mkLabel(p, "", F_S, C_BLUE);
    lv_obj_set_width(wxDetPop[i], 86); lv_obj_set_style_text_align(wxDetPop[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDetPop[i], cx, 252);
  }
  lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_HIDDEN);
}
static void onWxDay(lv_event_t *e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  if (!w.valid) return;
  if (!wxDetail) wxDetailBuild();
  lv_label_set_text(wxDetTitle, d == 0 ? "Today" : d == 1 ? "Tomorrow" : DOWF[w.days[d].wday]);
  char sub[96];
  snprintf(sub, sizeof(sub), "%s   High %d\xC2\xB0  Low %d\xC2\xB0   Rain %d%%", wxText(w.days[d].code), w.days[d].hi, w.days[d].lo, w.days[d].pop);
  lv_label_set_text(wxDetSub, sub);
  for (int i = 0; i < 8; i++) {
    const WxHour &h = w.dayHours[d][i];
    lv_label_set_text_fmt(wxDetTime[i], "%02d:00", h.hour);
    wxDraw(wxDetIcon[i], wxKindOf(h.code), h.hour < 7 || h.hour >= 20);
    lv_label_set_text_fmt(wxDetTemp[i], "%d\xC2\xB0", h.temp);
    if (h.pop >= 10) lv_label_set_text_fmt(wxDetPop[i], LV_SYMBOL_TINT " %d%%", h.pop);
    else lv_label_set_text(wxDetPop[i], "");
  }
  lv_obj_remove_flag(wxDetail, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *wxLblAge;

static void wxStatMake(lv_obj_t *card, int idx, int x, int y, const char *cap) {
  lv_obj_t *c = mkLabel(card, cap, F_XS, C_MUTED);
  lv_obj_set_pos(c, x, y);
  wxStat[idx] = mkLabel(card, "--", F_M, C_TEXT);
  lv_obj_set_pos(wxStat[idx], x, y + 18);
}

static void weatherBuild(lv_obj_t *pg) {
  lv_obj_t *top = card(pg, 12, 4, 688, 178);
  wxBigIcon = wxIcon(top, 24, 28, 112, WX_PARTLY, false);
  wxLblTemp = mkLabel(top, "--", F_NUM, C_TEXT);
  lv_obj_set_pos(wxLblTemp, 156, 10);
  wxLblCond = mkLabel(top, "Fetching weather...", F_L, C_TEXT);
  lv_obj_set_width(wxLblCond, 350);
  lv_label_set_long_mode(wxLblCond, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(wxLblCond, 156, 108);
  wxLblPlace = mkLabel(top, wxPlace, F_S, C_MUTED);
  lv_obj_set_pos(wxLblPlace, 156, 146);
  wxLblAge = mkLabel(top, "", F_XS, C_MUTED);
  lv_obj_set_pos(wxLblAge, 24, 150);

  wxStatMake(top, 0, 400, 14,  "FEELS LIKE");
  wxStatMake(top, 1, 540, 14,  "HUMIDITY");
  wxStatMake(top, 2, 400, 70,  "WIND");
  wxStatMake(top, 3, 540, 70,  "RAIN TODAY");
  wxStatMake(top, 4, 400, 126, "SUNRISE");
  wxStatMake(top, 5, 540, 126, "SUNSET");

  lv_obj_t *hr = card(pg, 12, 190, 688, 100);
  for (int i = 0; i < 8; i++) {
    int x = 8 + i * 84;
    wxHrTime[i] = mkLabel(hr, "--", F_XS, C_MUTED);
    lv_obj_set_width(wxHrTime[i], 84); lv_obj_set_style_text_align(wxHrTime[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxHrTime[i], x, 8);
    wxHrIcon[i] = wxIcon(hr, x + 24, 28, 36, WX_CLOUD, false);
    wxHrTemp[i] = mkLabel(hr, "--", F_S, C_TEXT);
    lv_obj_set_width(wxHrTemp[i], 84); lv_obj_set_style_text_align(wxHrTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxHrTemp[i], x, 70);
  }
  lv_obj_t *dy = card(pg, 12, 298, 688, 114);
  for (int i = 0; i < 7; i++) {
    int x = 4 + i * 98;
    wxDyName[i] = mkLabel(dy, "--", F_S, i == 0 ? C_ACCENT : C_MUTED);
    lv_obj_set_width(wxDyName[i], 98); lv_obj_set_style_text_align(wxDyName[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDyName[i], x, 6);
    wxDyIcon[i] = wxIcon(dy, x + 29, 28, 40, WX_CLOUD, false);
    wxDyTemp[i] = mkLabel(dy, "--", F_S, C_TEXT);
    lv_obj_set_width(wxDyTemp[i], 98); lv_obj_set_style_text_align(wxDyTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDyTemp[i], x, 72);
    wxDyPop[i] = mkLabel(dy, "", F_XS, C_BLUE);
    lv_obj_set_width(wxDyPop[i], 98); lv_obj_set_style_text_align(wxDyPop[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(wxDyPop[i], x, 92);
  }
  for (int i = 0; i < 7; i++) {                              // tap targets on top of each whole day column
    lv_obj_t *tap = clearBox(dy, 4 + i * 98, 0, 98, 114);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap, onWxDay, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

static const char *UNIT_T = USE_CELSIUS ? "C" : "F";

static void weatherRefreshUi() {
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  if (!w.valid) return;
  wxDraw(wxBigIcon, wxKindOf(w.code), !w.day);
  lv_label_set_text_fmt(wxLblTemp, "%d" "\xC2\xB0", w.temp);
  lv_label_set_text(wxLblCond, wxText(w.code));
  lv_label_set_text_fmt(wxStat[0], "%d\xC2\xB0%s", w.feels, UNIT_T);
  lv_label_set_text_fmt(wxStat[1], "%d%%", w.hum);
  lv_label_set_text_fmt(wxStat[2], "%d %s", w.wind, USE_MPH ? "mph" : "km/h");
  lv_label_set_text_fmt(wxStat[3], "%d%%", w.popToday);
  lv_label_set_text(wxStat[4], w.sunrise);
  lv_label_set_text(wxStat[5], w.sunset);
  for (int i = 0; i < 8; i++) {
    bool night = (w.hours[i].hour < 7 || w.hours[i].hour >= 20);
    lv_label_set_text_fmt(wxHrTime[i], "%02d:00", w.hours[i].hour);
    wxDraw(wxHrIcon[i], wxKindOf(w.hours[i].code), night);
    lv_label_set_text_fmt(wxHrTemp[i], "%d\xC2\xB0", w.hours[i].temp);
  }
  for (int i = 0; i < 7; i++) {
    lv_label_set_text(wxDyName[i], w.days[i].dow);
    wxDraw(wxDyIcon[i], wxKindOf(w.days[i].code), false);
    lv_label_set_text_fmt(wxDyTemp[i], "%d\xC2\xB0 / %d\xC2\xB0", w.days[i].hi, w.days[i].lo);
    if (w.days[i].pop >= 20) lv_label_set_text_fmt(wxDyPop[i], LV_SYMBOL_TINT " %d%%", w.days[i].pop);
    else lv_label_set_text(wxDyPop[i], "");
  }
}
