#pragma once
// Weather: Open-Meteo (free, no API key). Today + next hours + 7 days. Fetched by the network task.

// wind: speed / gusts in the configured unit (mph or km/h), dir = where it blows FROM, degrees (0 = north)
struct WxHour { int hour; int temp; uint8_t code; int pop; int16_t feels; uint8_t wind, gust; uint16_t dir; };
struct WxDay  { char dow[6]; uint8_t wday; uint8_t code; int hi, lo; int pop; };
struct WxData {
  bool valid = false;
  int temp = 0, feels = 0, hum = 0, wind = 0, windDir = 0, gust = 0, code = 0, uv = 0, popToday = 0;
  bool day = true;
  char sunrise[8] = "--:--", sunset[8] = "--:--";
  WxHour hours[8];
  WxDay days[7];
  WxHour dayHours[7][8];                        // 00:00, 03:00 ... 21:00 for each day (tap a day)
  uint32_t stamp = 0;
};
static WxData wx;
static volatile bool reqWeather = true;

// ───────────── location: set on the hub (Settings > Weather location), kept in NVS; config.h only has the default ─────────────
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
        copyText(g.name, sizeof(g.name), r["name"] | "");
        String d = String((const char *)(r["admin1"] | ""));
        const char *country = r["country"] | "";
        if (country[0]) d += d.length() ? String(", ") + country : String(country);
        copyText(g.detail, sizeof(g.detail), d.c_str());
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
  char url[900];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,"
    "wind_direction_10m,wind_gusts_10m,is_day"
    "&hourly=temperature_2m,apparent_temperature,weather_code,precipitation_probability,"
    "wind_speed_10m,wind_direction_10m,wind_gusts_10m"
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
  n.windDir = cur["wind_direction_10m"] | 0;
  n.gust  = (int)lroundf(cur["wind_gusts_10m"] | 0.0f);
  n.code  = cur["weather_code"] | 0;
  n.day   = (cur["is_day"] | 1) == 1;
  JsonObject hr = doc["hourly"];
  // hourly runs from 00:00 today (local time) for 7 days: the next hours start at the current hour
  const char *ct = cur["time"] | "";
  int start = (strlen(ct) >= 13) ? atoi(ct + 11) : 0;
  int nh = hr["time"].size();
  auto fillHour = [&](WxHour &h, int k) {               // everything but the hour, from hourly index k
    if (k < 0 || k >= nh) { h.temp = h.code = h.pop = h.feels = h.wind = h.gust = h.dir = 0; return; }
    h.temp  = (int)lroundf(hr["temperature_2m"][k] | 0.0f);
    h.feels = (int16_t)lroundf(hr["apparent_temperature"][k] | 0.0f);
    h.code  = hr["weather_code"][k] | 0;
    h.pop   = hr["precipitation_probability"][k] | 0;
    h.wind  = (uint8_t)std::min(255L, lroundf(hr["wind_speed_10m"][k] | 0.0f));
    h.gust  = (uint8_t)std::min(255L, lroundf(hr["wind_gusts_10m"][k] | 0.0f));
    h.dir   = hr["wind_direction_10m"][k] | 0;
  };
  for (int d = 0; d < 7; d++)
    for (int s = 0; s < 8; s++) {
      n.dayHours[d][s].hour = s * 3;
      fillHour(n.dayHours[d][s], d * 24 + s * 3);
    }
  for (int i = 0; i < 8; i++) {
    int k = min(start + i * 2, max(nh - 1, 0));
    const char *t = hr["time"][k] | "";                 // "2026-10-26T16:00"
    n.hours[i].hour = (strlen(t) >= 13) ? atoi(t + 11) : 0;
    fillHour(n.hours[i], k);
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

// ───────────── weather page (Tonal): conditions container on the left, next hours + the week on the right ─────────────
#define WX_LEFT_W  330
#define WX_RIGHT_X (T_PAD_X + WX_LEFT_W + T_GAP)
#define WX_RIGHT_W (SCREEN_W - T_PAD_X - WX_RIGHT_X)
#define WX_HOURS   6
#define WX_HR_H    128
static lv_obj_t *wxBigIcon, *wxLblTemp, *wxLblCond, *wxLblFeels, *wxLblPlace, *wxStat[4];
static lv_obj_t *wxHrCol[WX_HOURS], *wxHrTime[WX_HOURS], *wxHrTemp[WX_HOURS], *wxHrPop[WX_HOURS];
static lv_obj_t *wxDyName[7], *wxDyIcon[7], *wxDyLo[7], *wxDyHi[7], *wxDyTrack[7], *wxDyBar[7];
// ───────────── day detail: tap a day for its 3-hourly breakdown ─────────────
// 8 columns (00:00 ... 21:00): time, icon, temperature, feels like, rain; under them a temperature strip, wind
// (direction arrow + speed), and a smooth curve of wind speed and gusts.
#define WXD_W     720
#define WXD_H     404
#define WXD_COL   86
#define WXD_CX(i) (16 + (i) * WXD_COL + WXD_COL / 2)          // centre of column i
#define WXD_CURVE 4                                           // chart steps between two 3-hour samples: a smooth curve
#define WXD_PTS   (7 * WXD_CURVE + 1)
static lv_obj_t *wxDetail, *wxDetTitle, *wxDetSub, *wxDetTime[8], *wxDetIcon[8], *wxDetTemp[8], *wxDetFeels[8], *wxDetPop[8];
static lv_obj_t *wxDetSeg[7], *wxDetArrow[8], *wxDetWind[8], *wxDetGust[8], *wxDetChart, *wxDetUnits;
static lv_chart_series_t *wxSerWind, *wxSerGust;
static lv_point_precise_t wxArrowPts[8][5];                   // lv_line keeps a pointer to its points

// a temperature as a colour: cool blue at freezing and below, the theme's warm primary from about 25 °C (77 °F) up,
// blended in between. Used by the week's range bars and the day's temperature strip.
static lv_color_t tempColor(int t) {
  const int cold = USE_CELSIUS ? 0 : 32, warm = USE_CELSIUS ? 25 : 77;
  const uint32_t cool = themeMode == THEME_MODE_DARK ? 0x8EC5FF : 0x3D8BFD;   // light blue on dark, deeper on light
  int k = constrain((t - cold) * 255 / (warm - cold), 0, 255);
  return lv_color_mix(lv_color_hex(T->primary), lv_color_hex(cool), (uint8_t)k);
}

// arrow in a 24 x 24 box pointing where the wind blows TO (it's reported as where it comes FROM); north = up
static void wxArrowSet(int i, int fromDeg) {
  float a = (fromDeg + 180) * (float)M_PI / 180.0f, dx = sinf(a), dy = -cosf(a), px = -dy, py = dx;
  const float c = 12, L = 10, H = 6;
  float tx = c + dx * L, ty = c + dy * L;
  float pts[5][2] = {{c - dx * L, c - dy * L}, {tx, ty}, {tx - dx * H + px * H * 0.8f, ty - dy * H + py * H * 0.8f},
                     {tx, ty}, {tx - dx * H - px * H * 0.8f, ty - dy * H - py * H * 0.8f}};
  for (int k = 0; k < 5; k++) { wxArrowPts[i][k].x = (lv_value_precise_t)lroundf(pts[k][0]); wxArrowPts[i][k].y = (lv_value_precise_t)lroundf(pts[k][1]); }
  lv_line_set_points(wxDetArrow[i], wxArrowPts[i], 5);
}

// Catmull-Rom between samples i and i+1 of v[8], t in 0..1: passes through every sample, no overshoot to speak of
static int32_t curveAt(const int *v, int i, float t) {
  float p0 = v[std::max(i - 1, 0)], p1 = v[i], p2 = v[i + 1], p3 = v[std::min(i + 2, 7)];
  float r = 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
  return (int32_t)lroundf(std::max(r, 0.0f));
}

static lv_obj_t *wxDetCol(lv_obj_t *p, int i, int y, const lv_font_t *f, uint32_t col) {   // centred label in column i
  lv_obj_t *l = mkLabel(p, "", f, col);
  lv_obj_set_width(l, WXD_COL);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, WXD_CX(i) - WXD_COL / 2, y);
  return l;
}

static void onWxDetailClose(lv_event_t *) { lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_HIDDEN); }
static void wxDetailBuild() {
  wxDetail = lv_obj_create(lv_layer_top());                    // dim the whole screen, tap anywhere to close
  lv_obj_remove_style_all(wxDetail);
  lv_obj_set_size(wxDetail, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(wxDetail, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(wxDetail, LV_OPA_70, 0);
  lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wxDetail, onWxDetailClose, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *p = container(wxDetail, (SCREEN_W - WXD_W) / 2, (SCREEN_H - WXD_H) / 2, WXD_W, WXD_H, T->surface);
  lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
  wxDetTitle = mkLabel(p, "", F_T26, C_TEXT);
  lv_obj_set_pos(wxDetTitle, 28, 22);
  wxDetSub = mkLabel(p, "", F_S, C_MUTED);
  lv_obj_set_pos(wxDetSub, 28, 58);
  roundBtn(p, WXD_W - 28 - 48, 20, 48, LUCIDE_X, onWxDetailClose);

  for (int i = 0; i < 8; i++) {
    wxDetTime[i] = wxDetCol(p, i, 92, F_S, C_MUTED);
    wxDetIcon[i] = wxIcon(p, WXD_CX(i) - 14, 114, 28, WX_CLOUD, false);
    wxDetTemp[i] = wxDetCol(p, i, 148, F_T22, C_TEXT);
    wxDetFeels[i] = wxDetCol(p, i, 178, F_XS, C_MUTED);
    wxDetPop[i] = wxDetCol(p, i, 194, F_XS, C_MUTED);
  }
  // temperature strip, centre of the first column to the centre of the last: 7 gradient segments, rounded ends
  lv_obj_t *strip = panel(p, WXD_CX(0), 220, 7 * WXD_COL, 10, T->surfaceHi, 5);
  lv_obj_set_style_clip_corner(strip, true, 0);
  lv_obj_add_flag(strip, LV_OBJ_FLAG_EVENT_BUBBLE);
  for (int i = 0; i < 7; i++) {
    wxDetSeg[i] = panel(strip, i * WXD_COL, 0, WXD_COL, 10, T->primary, 0);
    lv_obj_set_style_bg_grad_dir(wxDetSeg[i], LV_GRAD_DIR_HOR, 0);
    lv_obj_add_flag(wxDetSeg[i], LV_OBJ_FLAG_EVENT_BUBBLE);
  }
  // wind: arrow + speed per column
  for (int i = 0; i < 8; i++) {
    wxDetArrow[i] = lv_line_create(p);
    lv_obj_set_size(wxDetArrow[i], 24, 24);
    lv_obj_set_pos(wxDetArrow[i], WXD_CX(i) - 28, 242);
    lv_obj_set_style_line_width(wxDetArrow[i], 2, 0);
    lv_obj_set_style_line_rounded(wxDetArrow[i], true, 0);
    lv_obj_set_style_line_color(wxDetArrow[i], lv_color_hex(T->text), 0);
    wxDetWind[i] = mkLabel(p, "", F_M, C_TEXT);
    lv_obj_set_pos(wxDetWind[i], WXD_CX(i) + 2, 244);
  }
  // wind and gust curves over the day
  wxDetChart = lv_chart_create(p);
  lv_obj_set_pos(wxDetChart, WXD_CX(0), 276);
  lv_obj_set_size(wxDetChart, 7 * WXD_COL, 70);
  lv_obj_set_style_bg_opa(wxDetChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wxDetChart, 0, 0);
  lv_obj_set_style_pad_all(wxDetChart, 2, 0);
  lv_obj_set_style_line_width(wxDetChart, 3, LV_PART_ITEMS);
  lv_obj_set_style_width(wxDetChart, 0, LV_PART_INDICATOR);   // no point markers: just the curves
  lv_obj_set_style_height(wxDetChart, 0, LV_PART_INDICATOR);
  lv_obj_add_flag(wxDetChart, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(wxDetChart, LV_OBJ_FLAG_CLICKABLE);
  lv_chart_set_type(wxDetChart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(wxDetChart, 0, 0);
  lv_chart_set_point_count(wxDetChart, WXD_PTS);
  wxSerWind = lv_chart_add_series(wxDetChart, lv_color_hex(T->muted), LV_CHART_AXIS_PRIMARY_Y);
  wxSerGust = lv_chart_add_series(wxDetChart, lv_color_hex(T->primary), LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < 8; i++) wxDetGust[i] = wxDetCol(p, i, 350, F_XS, T->primary);
  // legend
  lv_obj_t *lw = mkLabel(p, "\xE2\x80\xA2 Wind", F_XS, C_MUTED);
  lv_obj_set_pos(lw, 28, 374);
  lv_obj_t *lg = mkLabel(p, "\xE2\x80\xA2 Gusts", F_XS, T->primary);
  lv_obj_set_pos(lg, 90, 374);
  wxDetUnits = mkLabel(p, USE_MPH ? "mph \xC2\xB7 arrows point where the wind is going" : "km/h \xC2\xB7 arrows point where the wind is going", F_XS, C_MUTED);
  lv_obj_set_pos(wxDetUnits, 160, 374);
  lv_obj_add_flag(wxDetail, LV_OBJ_FLAG_HIDDEN);
}
static void onWxDay(lv_event_t *e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  static WxData w;                                             // big: not on the UI task's stack
  DATA_LOCK(); w = wx; DATA_UNLOCK();
  if (!w.valid) return;
  if (!wxDetail) wxDetailBuild();
  const WxHour *h = w.dayHours[d];
  int fLo = h[0].feels, fHi = h[0].feels, gMax = 1, wind[8], gust[8];
  for (int i = 0; i < 8; i++) {
    fLo = std::min(fLo, (int)h[i].feels); fHi = std::max(fHi, (int)h[i].feels);
    wind[i] = h[i].wind; gust[i] = std::max(h[i].gust, h[i].wind);
    gMax = std::max(gMax, gust[i]);
  }
  lv_label_set_text(wxDetTitle, d == 0 ? "Today" : d == 1 ? "Tomorrow" : DOWF[w.days[d].wday]);
  char sub[128];
  snprintf(sub, sizeof(sub), "%s \xC2\xB7 High %d\xC2\xB0 \xC2\xB7 Low %d\xC2\xB0 \xC2\xB7 Feels %d\xE2\x80\x93%d\xC2\xB0 \xC2\xB7 Rain %d%%",
           wxText(w.days[d].code), w.days[d].hi, w.days[d].lo, fLo, fHi, w.days[d].pop);
  lv_label_set_text(wxDetSub, sub);
  for (int i = 0; i < 8; i++) {
    lv_label_set_text_fmt(wxDetTime[i], "%02d:00", h[i].hour);
    wxDraw(wxDetIcon[i], wxKindOf(h[i].code), h[i].hour < 7 || h[i].hour >= 20);
    lv_label_set_text_fmt(wxDetTemp[i], "%d\xC2\xB0", h[i].temp);
    lv_label_set_text_fmt(wxDetFeels[i], "feels %d\xC2\xB0", h[i].feels);
    if (h[i].pop >= 10) lv_label_set_text_fmt(wxDetPop[i], "%d%% rain", h[i].pop); else lv_label_set_text(wxDetPop[i], "");
    if (i < 7) {
      lv_obj_set_style_bg_color(wxDetSeg[i], tempColor(h[i].temp), 0);
      lv_obj_set_style_bg_grad_color(wxDetSeg[i], tempColor(h[i + 1].temp), 0);
    }
    wxArrowSet(i, h[i].dir);
    lv_label_set_text_fmt(wxDetWind[i], "%d", h[i].wind);
    lv_label_set_text_fmt(wxDetGust[i], "G %d", gust[i]);
  }
  lv_chart_set_range(wxDetChart, LV_CHART_AXIS_PRIMARY_Y, 0, gMax + gMax / 6 + 1);
  for (int i = 0; i < 7; i++)
    for (int s = 0; s < WXD_CURVE; s++) {
      float t = (float)s / WXD_CURVE;
      lv_chart_set_value_by_id(wxDetChart, wxSerWind, i * WXD_CURVE + s, curveAt(wind, i, t));
      lv_chart_set_value_by_id(wxDetChart, wxSerGust, i * WXD_CURVE + s, curveAt(gust, i, t));
    }
  lv_chart_set_value_by_id(wxDetChart, wxSerWind, WXD_PTS - 1, wind[7]);
  lv_chart_set_value_by_id(wxDetChart, wxSerGust, WXD_PTS - 1, gust[7]);
  lv_chart_refresh(wxDetChart);
  lv_obj_remove_flag(wxDetail, LV_OBJ_FLAG_HIDDEN);
}

static void weatherBuild(lv_obj_t *pg) {
  uint32_t on = T->onSecondary;
  int H = SCREEN_H - 2 * T_PAD_TOP;
  lv_obj_t *l = container(pg, T_PAD_X, T_PAD_TOP, WX_LEFT_W, H, T->secondary);
  roundBtn(l, 22, 20, 48, LUCIDE_CHEVRON_LEFT, onPageBack, on);
  wxLblPlace = mkLabel(l, wxPlace, F_S, on);
  lv_obj_set_width(wxLblPlace, WX_LEFT_W - 100);
  lv_label_set_long_mode(wxLblPlace, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(wxLblPlace, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(wxLblPlace, 78, 34);

  wxLblTemp = mkLabel(l, "--", F_HERO, on);
  lv_obj_set_style_text_letter_space(wxLblTemp, -6, 0);
  lv_obj_set_pos(wxLblTemp, 18, 112);
  wxLblCond = mkLabel(l, "Fetching weather...", F_L, on);
  oneLine(wxLblCond, WX_LEFT_W - 44 - 72, false);
  lv_obj_set_pos(wxLblCond, 22, 214);
  wxLblFeels = mkLabel(l, "", F_S, on);
  lv_obj_set_style_text_opa(wxLblFeels, LV_OPA_80, 0);
  lv_obj_set_pos(wxLblFeels, 22, 242);
  wxBigIcon = wxIcon(l, WX_LEFT_W - 22 - 60, 196, 56, WX_PARTLY, false, on);

  // wind / rain / humidity / sunrise-sunset chips, 2 x 2 along the bottom
  static const char *icons[4] = {LUCIDE_WIND, LUCIDE_UMBRELLA, LUCIDE_DROPLETS, LUCIDE_SUNRISE};
  int cw = (WX_LEFT_W - 44 - 8) / 2;
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = panel(l, 22 + (i % 2) * (cw + 8), H - 20 - 48 - 8 - 48 + (i / 2) * 56, cw, 48, on, T_R_ROW);
    tint(c, on);
    lv_obj_t *ic = mkLabel(c, icons[i], I_20, on);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    wxStat[i] = mkLabel(c, "--", F_S, on);
    oneLine(wxStat[i], cw - 48, false);
    lv_obj_align(wxStat[i], LV_ALIGN_LEFT_MID, 40, 0);
  }

  // next hours: six columns, the first ("now") on a primary pill
  lv_obj_t *hr = panel(pg, WX_RIGHT_X, T_PAD_TOP, WX_RIGHT_W, WX_HR_H, T->surface, 32);
  int colW = (WX_RIGHT_W - 32 - 5 * 6) / WX_HOURS;
  for (int i = 0; i < WX_HOURS; i++) {
    uint32_t fg = i == 0 ? T->onPrimary : T->text;
    lv_obj_t *c = panel(hr, 16 + i * (colW + 6), 14, colW, WX_HR_H - 28, i == 0 ? T->primary : T->surface, T_R_ROW);
    wxHrCol[i] = c;
    wxHrTime[i] = mkLabel(c, "--", F_XS, fg);
    lv_obj_align(wxHrTime[i], LV_ALIGN_TOP_MID, 0, 10);
    wxHrTemp[i] = mkLabel(c, "--", F_T22, fg);
    lv_obj_align(wxHrTemp[i], LV_ALIGN_CENTER, 0, 0);
    wxHrPop[i] = mkLabel(c, "", F_XS, fg);
    lv_obj_set_style_text_opa(wxHrPop[i], LV_OPA_80, 0);
    lv_obj_align(wxHrPop[i], LV_ALIGN_BOTTOM_MID, 0, -10);
  }

  // the week: day, icon, low, range bar, high. Tap a day for its hours.
  int dy0 = T_PAD_TOP + WX_HR_H + T_GAP, dh = SCREEN_H - T_PAD_TOP - dy0;
  lv_obj_t *dy = panel(pg, WX_RIGHT_X, dy0, WX_RIGHT_W, dh, T->surface, 32);
  int rowH = (dh - 20) / 7, trackX = 18 + 52 + 34 + 40, trackW = WX_RIGHT_W - trackX - 18 - 44;
  for (int i = 0; i < 7; i++) {
    lv_obj_t *r = clearBox(dy, 0, 10 + i * rowH, WX_RIGHT_W, rowH);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, onWxDay, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    wxDyName[i] = mkLabel(r, "--", F_M, C_TEXT);
    lv_obj_align(wxDyName[i], LV_ALIGN_LEFT_MID, 18, 0);
    wxDyIcon[i] = mkLabel(r, LUCIDE_CLOUD, I_24, C_MUTED);
    lv_obj_align(wxDyIcon[i], LV_ALIGN_LEFT_MID, 18 + 52, 0);
    wxDyLo[i] = mkLabel(r, "", F_M, C_MUTED);
    lv_obj_set_width(wxDyLo[i], 34);
    lv_obj_set_style_text_align(wxDyLo[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(wxDyLo[i], LV_ALIGN_LEFT_MID, trackX - 40, 0);
    wxDyTrack[i] = panel(r, trackX, (rowH - 10) / 2, trackW, 10, T->surfaceHi, 5);
    lv_obj_add_flag(wxDyTrack[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    wxDyBar[i] = panel(wxDyTrack[i], 0, 0, 10, 10, T->primary, 5);
    lv_obj_add_flag(wxDyBar[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    wxDyHi[i] = mkLabel(r, "", F_M, C_TEXT);
    lv_obj_align(wxDyHi[i], LV_ALIGN_LEFT_MID, trackX + trackW + 10, 0);
  }
}

static void weatherRefreshUi() {
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  lv_label_set_text(wxLblPlace, wxPlace);
  if (!w.valid) return;
  wxDraw(wxBigIcon, wxKindOf(w.code), !w.day);
  lv_label_set_text_fmt(wxLblTemp, "%d\xC2\xB0", w.temp);
  lv_label_set_text(wxLblCond, wxText(w.code));
  lv_label_set_text_fmt(wxLblFeels, "Feels like %d\xC2\xB0", w.feels);
  static const char *COMPASS[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};    // where it comes from
  lv_label_set_text_fmt(wxStat[0], "%d %s %s", w.wind, USE_MPH ? "mph" : "km/h", COMPASS[((w.windDir + 22) % 360) / 45]);
  lv_label_set_text_fmt(wxStat[1], "%d%% rain", w.popToday);
  lv_label_set_text_fmt(wxStat[2], "%d%% humid", w.hum);
  lv_label_set_text_fmt(wxStat[3], "%s / %s", w.sunrise, w.sunset);
  for (int i = 0; i < WX_HOURS; i++) {
    if (i == 0) lv_label_set_text(wxHrTime[i], "Now");
    else lv_label_set_text_fmt(wxHrTime[i], "%02d:00", w.hours[i].hour);
    lv_label_set_text_fmt(wxHrTemp[i], "%d\xC2\xB0", w.hours[i].temp);
    if (w.hours[i].pop >= 10) lv_label_set_text_fmt(wxHrPop[i], "%d%%", w.hours[i].pop);
    else lv_label_set_text(wxHrPop[i], "");
  }
  int lo = w.days[0].lo, hi = w.days[0].hi;                  // the week's range, for the bars
  for (int i = 1; i < 7; i++) { lo = std::min(lo, w.days[i].lo); hi = std::max(hi, w.days[i].hi); }
  int span = std::max(hi - lo, 1), tw = lv_obj_get_width(wxDyTrack[0]);
  for (int i = 0; i < 7; i++) {
    lv_label_set_text(wxDyName[i], i == 0 ? "Today" : w.days[i].dow);
    wxDraw(wxDyIcon[i], wxKindOf(w.days[i].code), false);
    lv_label_set_text_fmt(wxDyLo[i], "%d\xC2\xB0", w.days[i].lo);
    lv_label_set_text_fmt(wxDyHi[i], "%d\xC2\xB0", w.days[i].hi);
    int x0 = (w.days[i].lo - lo) * tw / span, x1 = (w.days[i].hi - lo) * tw / span;
    lv_obj_set_pos(wxDyBar[i], x0, 0);
    lv_obj_set_width(wxDyBar[i], std::max(x1 - x0, 10));
    lv_obj_set_style_bg_color(wxDyBar[i], tempColor(w.days[i].lo), 0);
    lv_obj_set_style_bg_grad_color(wxDyBar[i], tempColor(w.days[i].hi), 0);
    lv_obj_set_style_bg_grad_dir(wxDyBar[i], LV_GRAD_DIR_HOR, 0);
  }
}
