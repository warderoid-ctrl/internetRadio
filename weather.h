#pragma once
// Weather: Open-Meteo (free, no API key). Today + next hours + 7 days. Fetched by the network task.

struct WxHour { int hour; int temp; uint8_t code; int pop; };
struct WxDay  { char dow[4]; uint8_t code; int hi, lo; int pop; };
struct WxData {
  bool valid = false;
  int temp = 0, feels = 0, hum = 0, wind = 0, code = 0, uv = 0, popToday = 0;
  bool day = true;
  char sunrise[8] = "--:--", sunset[8] = "--:--";
  WxHour hours[8];
  WxDay days[7];
  uint32_t stamp = 0;
};
static WxData wx;
static volatile bool reqWeather = true;

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
    "&hourly=temperature_2m,weather_code,precipitation_probability&forecast_hours=16"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
    "&forecast_days=7&timezone=auto&wind_speed_unit=%s&temperature_unit=%s",
    WEATHER_LAT, WEATHER_LON, USE_MPH ? "mph" : "kmh", USE_CELSIUS ? "celsius" : "fahrenheit");
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
  for (int i = 0; i < 8; i++) {
    int k = i * 2;
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
    strlcpy(n.days[i].dow, i == 0 ? "Today" : DOW3[tm.tm_wday & 7], sizeof(n.days[i].dow));
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
           w.temp, wxText(w.code), WEATHER_PLACE, w.days[0].hi, w.days[0].lo, w.popToday);
}

// ───────────── weather page ─────────────
static lv_obj_t *wxBigIcon, *wxLblTemp, *wxLblCond, *wxLblPlace, *wxStat[6];
static lv_obj_t *wxHrTime[8], *wxHrIcon[8], *wxHrTemp[8];
static lv_obj_t *wxDyName[7], *wxDyIcon[7], *wxDyTemp[7], *wxDyPop[7];
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
  wxLblPlace = mkLabel(top, WEATHER_PLACE, F_S, C_MUTED);
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
