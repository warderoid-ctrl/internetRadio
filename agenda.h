#pragma once
// Appointments: iCal feeds (Outlook/Google/iCloud "secret address") and/or Home Assistant calendars.
// Handles one-off and repeating events (DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL, BYDAY, EXDATE).
// Not handled: nth-weekday rules like "2nd Tuesday", edited single instances of a series, and non-UK timezones
// (times with a TZID are treated as the hub's local time).

#define AGENDA_WINDOW_DAYS 14
#define MAX_APPT 48

struct Appt { time_t start, end; bool allDay; char title[64]; char loc[40]; };
static Appt *appts = nullptr;                                 // MAX_APPT each, PSRAM (12 KB is too much for internal RAM)
static Appt *tmpA = nullptr;                                  // netTask builds the next list here
static int nAppts = 0;
static volatile bool reqAgenda = true;
static bool agendaConfigured = false;

// ── calendar links: pasted on the phone remote, kept in NVS (secret iCal links must never go into git) ──
// config.h ICS_URLS still works as a fallback. The list is the UI task's; netTask copies it under DATA_LOCK.
#define MAX_CAL 4
#define CAL_LEN 512
static char *calLinks = nullptr;                              // MAX_CAL x CAL_LEN, PSRAM
static int nCal = 0;
static void calKey(char *k, int i) { snprintf(k, 8, "ics%d", i); }
static void calLoad() {                                       // after prefs.begin(); also allocates the appointment lists
  appts = (Appt *)heap_caps_calloc(MAX_APPT, sizeof(Appt), MALLOC_CAP_SPIRAM);
  tmpA = (Appt *)heap_caps_calloc(MAX_APPT, sizeof(Appt), MALLOC_CAP_SPIRAM);
  calLinks = (char *)heap_caps_calloc(MAX_CAL, CAL_LEN, MALLOC_CAP_SPIRAM);
  if (!calLinks) return;
  nCal = 0;
  for (int i = 0; i < MAX_CAL; i++) {
    char k[8]; calKey(k, i);
    if (!prefs.isKey(k)) continue;
    String v = prefs.getString(k, "");
    if (v.length() && v.length() < CAL_LEN) strlcpy(calLinks + CAL_LEN * nCal++, v.c_str(), CAL_LEN);
  }
}
static void calSave() {
  for (int i = 0; i < MAX_CAL; i++) {
    char k[8]; calKey(k, i);
    if (i < nCal) prefs.putString(k, calLinks + CAL_LEN * i); else if (prefs.isKey(k)) prefs.remove(k);
  }
}
// add a link (webcal:// becomes https://); false if full / not a link
static bool calAdd(const char *u) {
  if (!calLinks) return false;
  String s = String(u);
  s.trim();
  if (s.startsWith("webcal://")) s = "https://" + s.substring(9);
  if (!s.startsWith("http") || s.length() >= CAL_LEN) return false;
  DATA_LOCK();
  bool ok = nCal < MAX_CAL;
  if (ok) strlcpy(calLinks + CAL_LEN * nCal++, s.c_str(), CAL_LEN);
  DATA_UNLOCK();
  if (ok) { calSave(); reqAgenda = true; }
  return ok;
}
static void calDel(int i) {
  if (i < 0 || i >= nCal) return;
  DATA_LOCK();
  memmove(calLinks + CAL_LEN * i, calLinks + CAL_LEN * (i + 1), CAL_LEN * (nCal - i - 1));
  nCal--;
  DATA_UNLOCK();
  calSave();
  reqAgenda = true;
}
// for the phone: "calendar.google.com ... 3f9a.ics" (enough to tell links apart without echoing the secret)
static void calLabel(int i, char *out, size_t n) {
  const char *u = calLinks + CAL_LEN * i;
  const char *h = strstr(u, "://"); h = h ? h + 3 : u;
  const char *e = strchr(h, '/');
  int hl = e ? (int)(e - h) : (int)strlen(h);
  size_t ul = strlen(u);
  snprintf(out, n, "%.*s ... %s", hl, h, ul > 8 ? u + ul - 8 : u);
}

// ── date maths (no timegm on the ESP32, so do it ourselves) ──
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int)doe - 719468;
}
static void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + (int)era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}
static time_t utcEpoch(int y, int mo, int d, int h, int mi, int s) { return (time_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s); }
static time_t localEpoch(int y, int mo, int d, int h, int mi, int s) {
  struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = -1;
  return mktime(&t);
}
static int64_t localDayIndex(time_t t) { struct tm l; localtime_r(&t, &l); return daysFromCivil(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday); }

// iCal value: 20261026T093000Z | 20261026T093000 | 20261026
static bool parseIcsDate(const char *v, time_t &out, bool &allDay) {
  int y, mo, d, h = 0, mi = 0, s = 0;
  if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) return false;
  allDay = (strlen(v) <= 8 || v[8] != 'T');
  if (!allDay) sscanf(v + 9, "%2d%2d%2d", &h, &mi, &s);
  bool utc = (!allDay && strchr(v, 'Z'));
  out = utc ? utcEpoch(y, mo, d, h, mi, s) : localEpoch(y, mo, d, h, mi, s);
  return true;
}
// ISO 8601 from Home Assistant: 2026-10-27T09:00:00+01:00 | 2026-10-27
static bool parseIso(const char *v, time_t &out, bool &allDay) {
  int y, mo, d, h = 0, mi = 0, s = 0;
  if (sscanf(v, "%4d-%2d-%2d", &y, &mo, &d) != 3) return false;
  allDay = (strlen(v) <= 10);
  if (allDay) { out = localEpoch(y, mo, d, 0, 0, 0); return true; }
  sscanf(v + 11, "%2d:%2d:%2d", &h, &mi, &s);
  int off = 0;
  const char *z = v + 19;
  if (strlen(v) > 19 && (*z == '+' || *z == '-')) { int oh = 0, om = 0; sscanf(z + 1, "%2d:%2d", &oh, &om); off = (oh * 3600 + om * 60) * (*z == '-' ? -1 : 1); }
  out = utcEpoch(y, mo, d, h, mi, s) - off;
  return true;
}

// ── collecting events (keeps the earliest MAX_APPT) ──
static int tmpN = 0;
static time_t winStart, winEnd;

static void addAppt(time_t s, time_t e, bool allDay, const char *title, const char *loc) {
  if (e <= winStart || s >= winEnd) return;
  Appt a; a.start = s; a.end = e; a.allDay = allDay;
  copyAscii(a.title, sizeof(a.title), title && title[0] ? title : "(no title)");
  copyAscii(a.loc, sizeof(a.loc), loc ? loc : "");
  if (tmpN < MAX_APPT) { tmpA[tmpN++] = a; return; }
  int latest = 0;
  for (int i = 1; i < tmpN; i++) if (tmpA[i].start > tmpA[latest].start) latest = i;
  if (a.start < tmpA[latest].start) tmpA[latest] = a;
}

// ── RRULE expansion ──
struct IcsEvent {
  bool have = false, haveEnd = false, allDay = false, cancelled = false, hasRid = false;
  time_t start = 0, end = 0;
  char title[80] = "", loc[48] = "", rrule[128] = "";
  int exd[8]; int nExd = 0;
};

static int rruleInt(const char *rr, const char *key, int def) {
  const char *p = strstr(rr, key);
  return p ? atoi(p + strlen(key)) : def;
}
static void expandAndAdd(const IcsEvent &ev) {
  if (!ev.rrule[0] || ev.hasRid) { addAppt(ev.start, ev.end, ev.allDay, ev.title, ev.loc); return; }
  char fr = '?';
  const char *f = strstr(ev.rrule, "FREQ=");
  if (f) {
    if (!strncmp(f + 5, "DAILY", 5)) fr = 'D';
    else if (!strncmp(f + 5, "WEEKLY", 6)) fr = 'W';
    else if (!strncmp(f + 5, "MONTHLY", 7)) fr = 'M';
    else if (!strncmp(f + 5, "YEARLY", 6)) fr = 'Y';
  }
  if (fr == '?') { addAppt(ev.start, ev.end, ev.allDay, ev.title, ev.loc); return; }
  int interval = std::max(1, rruleInt(ev.rrule, "INTERVAL=", 1));
  int count = rruleInt(ev.rrule, "COUNT=", 0);
  int64_t untilDay = INT64_MAX;
  const char *u = strstr(ev.rrule, "UNTIL=");
  if (u) { time_t ut; bool ad; if (parseIcsDate(u + 6, ut, ad)) untilDay = localDayIndex(ut); }
  int bymd = rruleInt(ev.rrule, "BYMONTHDAY=", 0);
  uint8_t byday = 0;                                 // Mon=bit0 .. Sun=bit6
  const char *b = strstr(ev.rrule, "BYDAY=");
  if (b) {
    b += 6;
    while (*b && *b != ';') {
      static const char *nm[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
      for (int i = 0; i < 7; i++) if (b[0] == nm[i][0] && b[1] == nm[i][1]) byday |= (1 << i);
      b++;
    }
  }
  struct tm base; localtime_r(&ev.start, &base);
  int64_t startDay = daysFromCivil(base.tm_year + 1900, base.tm_mon + 1, base.tm_mday);
  int sy = base.tm_year + 1900, sm = base.tm_mon + 1, sd = base.tm_mday;
  if (!byday) byday = 1 << ((base.tm_wday + 6) % 7);
  long dur = (long)(ev.end - ev.start);

  int64_t d0 = localDayIndex(winStart), d1 = localDayIndex(winEnd);
  int64_t from = count ? startDay : std::max<int64_t>(startDay, d0);
  int64_t to = (untilDay == INT64_MAX) ? d1 : std::min<int64_t>(d1, untilDay + 1);
  int seen = 0;
  for (int64_t day = from; day < to; day++) {
    int64_t rel = day - startDay;
    bool occ = false;
    int y, m, d; civilFromDays(day, y, m, d);
    if (fr == 'D') occ = (rel % interval == 0);
    else if (fr == 'W') {
      int wd = (int)(((day + 4) % 7 + 7) % 7);            // 0 = Sunday
      int64_t wk = (day + 3) / 7, wk0 = (startDay + 3) / 7;
      occ = ((wk - wk0) % interval == 0) && (byday & (1 << ((wd + 6) % 7)));
    } else if (fr == 'M') {
      int months = (y - sy) * 12 + (m - sm);
      occ = (months % interval == 0) && (d == (bymd ? bymd : sd));
    } else if (fr == 'Y') {
      occ = ((y - sy) % interval == 0) && (m == sm) && (d == sd);
    }
    if (!occ) continue;
    seen++;
    if (count && seen > count) break;
    if (day < d0) continue;
    int key = y * 10000 + m * 100 + d;
    bool ex = false;
    for (int i = 0; i < ev.nExd; i++) if (ev.exd[i] == key) ex = true;
    if (ex) continue;
    struct tm t = base; t.tm_mday += (int)(day - startDay); t.tm_isdst = -1;
    time_t s = mktime(&t);
    addAppt(s, s + dur, ev.allDay, ev.title, ev.loc);
  }
}

// ── streaming iCal parser (never holds the whole file in memory) ──
static void icsUnescape(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '\\' && p[1]) { p++; *o++ = (*p == 'n' || *p == 'N') ? ' ' : *p; }
    else *o++ = *p;
  }
  *o = 0;
}

class IcsSink : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { feed((char)c); return 1; }
  size_t write(const uint8_t *b, size_t n) override { for (size_t i = 0; i < n; i++) feed((char)b[i]); return n; }
  void finish() { flushLogical(); }
 private:
  char ln[420]; int lnLen = 0;
  char pend[420]; int pendLen = 0; bool skip = true;
  IcsEvent ev; bool inEvent = false;

  static bool interesting(const char *s) {
    static const char *keys[] = {"BEGIN:VEVENT", "END:VEVENT", "DTSTART", "DTEND", "SUMMARY", "LOCATION", "RRULE", "EXDATE", "STATUS", "RECURRENCE-ID"};
    for (auto k : keys) if (!strncmp(s, k, strlen(k))) return true;
    return false;
  }
  void feed(char c) {
    if (c == '\r') return;
    if (c != '\n') { if (lnLen < (int)sizeof(ln) - 1) ln[lnLen++] = c; return; }
    ln[lnLen] = 0;
    if (lnLen && (ln[0] == ' ' || ln[0] == '\t')) {                // folded continuation
      if (!skip && pendLen + lnLen < (int)sizeof(pend)) { memcpy(pend + pendLen, ln + 1, lnLen - 1); pendLen += lnLen - 1; pend[pendLen] = 0; }
    } else {
      flushLogical();
      skip = !interesting(ln);
      if (!skip) { strlcpy(pend, ln, sizeof(pend)); pendLen = strlen(pend); }
    }
    lnLen = 0;
  }
  void flushLogical() {
    if (!pendLen) return;
    pend[pendLen] = 0; pendLen = 0;
    handle(pend);
  }
  void handle(char *l) {
    if (!strcmp(l, "BEGIN:VEVENT")) { ev = IcsEvent(); inEvent = true; return; }
    if (!inEvent) return;
    if (!strcmp(l, "END:VEVENT")) {
      inEvent = false;
      if (ev.have && !ev.cancelled) {
        if (!ev.haveEnd) ev.end = ev.allDay ? ev.start + 86400 : ev.start + 1;
        expandAndAdd(ev);
      }
      return;
    }
    char *colon = strchr(l, ':');
    if (!colon) return;
    *colon = 0; char *val = colon + 1;
    // property name = up to first ';'
    if (!strncmp(l, "DTSTART", 7)) { ev.have = parseIcsDate(val, ev.start, ev.allDay); }
    else if (!strncmp(l, "DTEND", 5)) { bool ad; ev.haveEnd = parseIcsDate(val, ev.end, ad); }
    else if (!strncmp(l, "SUMMARY", 7)) { icsUnescape(val); strlcpy(ev.title, val, sizeof(ev.title)); }
    else if (!strncmp(l, "LOCATION", 8)) { icsUnescape(val); strlcpy(ev.loc, val, sizeof(ev.loc)); }
    else if (!strncmp(l, "RRULE", 5)) { strlcpy(ev.rrule, val, sizeof(ev.rrule)); }
    else if (!strncmp(l, "RECURRENCE-ID", 13)) { ev.hasRid = true; }
    else if (!strncmp(l, "STATUS", 6)) { if (!strncmp(val, "CANCELLED", 9)) ev.cancelled = true; }
    else if (!strncmp(l, "EXDATE", 6)) {
      char *tok = strtok(val, ",");
      while (tok && ev.nExd < 8) { int y, m, d; if (sscanf(tok, "%4d%2d%2d", &y, &m, &d) == 3) ev.exd[ev.nExd++] = y * 10000 + m * 100 + d; tok = strtok(nullptr, ","); }
    }
  }
};

static void haCalendarFetch(const char *entity) {
  char s[32], e[32], url[300];
  struct tm t;
  gmtime_r(&winStart, &t); strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%SZ", &t);
  gmtime_r(&winEnd, &t);   strftime(e, sizeof(e), "%Y-%m-%dT%H:%M:%SZ", &t);
  snprintf(url, sizeof(url), "%s/api/calendars/%s?start=%s&end=%s", HA_URL, entity, s, e);
  HttpOpts o; o.bearer = HA_TOKEN;
  String body;
  int code = httpGet(url, body, o);
  if (code != 200) { Serial.printf("HA calendar %s -> %d\n", entity, code); return; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  for (JsonObject ev : doc.as<JsonArray>()) {
    time_t s0, e0; bool ad0 = false, ad1 = false;
    const char *sv = ev["start"]["dateTime"] | (const char *)(ev["start"]["date"] | "");
    const char *ev1 = ev["end"]["dateTime"] | (const char *)(ev["end"]["date"] | "");
    if (!parseIso(sv, s0, ad0)) continue;
    if (!parseIso(ev1, e0, ad1)) e0 = s0 + 3600;
    addAppt(s0, e0, ad0, ev["summary"] | "", ev["location"] | "");
  }
}

static bool agendaFetch() {
  if (!appts || !tmpA) return false;
  time_t now = time(nullptr);
  struct tm l; localtime_r(&now, &l);
  l.tm_hour = 0; l.tm_min = 0; l.tm_sec = 0; l.tm_isdst = -1;
  winStart = mktime(&l);
  l.tm_mday += AGENDA_WINDOW_DAYS; l.tm_isdst = -1;
  winEnd = mktime(&l);
  tmpN = 0;
  bool any = false;
  // the links from the phone (copied: they can change while we fetch), then any from config.h
  char *links = (char *)heap_caps_malloc(MAX_CAL * CAL_LEN, MALLOC_CAP_SPIRAM);
  int nl = 0;
  if (links && calLinks) { DATA_LOCK(); nl = nCal; memcpy(links, calLinks, (size_t)nl * CAL_LEN); DATA_UNLOCK(); }
  for (int i = 0; i < nl + 8; i++) {
    const char *u = i < nl ? links + CAL_LEN * i : ICS_URLS[i - nl];
    if (i >= nl && !u) break;
    any = true;
    IcsSink sink;
    int code = httpStream(u, sink);
    sink.finish();
    Serial.printf("ICS %d -> HTTP %d (events so far %d)\n", i, code, tmpN);
  }
  free(links);
  if (HA_URL[0] && HA_TOKEN[0]) for (int i = 0; HA_CALENDARS[i]; i++) { any = true; haCalendarFetch(HA_CALENDARS[i]); }
  agendaConfigured = any;
  if (!any) return false;
  std::sort(tmpA, tmpA + tmpN, [](const Appt &a, const Appt &b) { return a.start < b.start; });
  DATA_LOCK(); memcpy(appts, tmpA, sizeof(Appt) * tmpN); nAppts = tmpN; DATA_UNLOCK();
  agVer++;
  return true;
}

// first upcoming (or in-progress) appointment. false if none
static bool agendaNext(Appt &out) {
  time_t now = time(nullptr);
  bool ok = false;
  DATA_LOCK();
  for (int i = 0; i < nAppts; i++) if (appts[i].end > now && !(appts[i].allDay && appts[i].start < now - 3600)) { out = appts[i]; ok = true; break; }
  DATA_UNLOCK();
  return ok;
}
static void fmtWhen(const Appt &a, char *buf, size_t n) {
  struct tm t; localtime_r(&a.start, &t);
  struct tm nw; time_t now = time(nullptr); localtime_r(&now, &nw);
  int64_t dd = daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) - daysFromCivil(nw.tm_year + 1900, nw.tm_mon + 1, nw.tm_mday);
  const char *day = dd <= 0 ? "today" : (dd == 1 ? "tomorrow" : DOWF[t.tm_wday]);
  if (a.allDay) snprintf(buf, n, "%s", day);
  else snprintf(buf, n, "%s at %02d:%02d", day, t.tm_hour, t.tm_min);
}
static void agendaSentence(char *out, size_t n) {
  if (!agendaConfigured) { snprintf(out, n, "No calendar is set up yet."); return; }
  Appt a;
  if (!agendaNext(a)) { snprintf(out, n, "You have nothing coming up in the next two weeks."); return; }
  char w[48]; fmtWhen(a, w, sizeof(w));
  snprintf(out, n, "Your next appointment is %s, %s.", a.title, w);
}

// ── agenda page ──
static lv_obj_t *agList, *agHint;
static void agendaBuild(lv_obj_t *pg) {
  agList = scrollCol(pg, 12, 4, 688, 408, 8);
  agHint = nullptr;
}

static void agendaRefreshUi() {
  lv_obj_clean(agList);
  if (!agendaConfigured) {
    lv_obj_t *c = card(agList, 0, 0, 688, 200);
    lv_obj_t *t = mkLabel(c, LV_SYMBOL_LIST "  No calendar connected yet", F_L, C_TEXT);
    lv_obj_set_pos(t, 24, 22);
    lv_obj_t *d = mkLabel(c, "Add one from your phone: More > Phone remote, scan the QR code,\nthen Calendars > paste your calendar's secret iCal link.\n(Google, Outlook and iCloud all have one - the phone page says where.)", F_S, C_MUTED);
    lv_obj_set_pos(d, 24, 78);
    return;
  }
  static Appt *local = (Appt *)heap_caps_malloc(sizeof(Appt) * MAX_APPT, MALLOC_CAP_SPIRAM);   // too big for the UI stack
  if (!local) return;
  int n;
  DATA_LOCK(); n = nAppts; memcpy(local, appts, sizeof(Appt) * n); DATA_UNLOCK();
  if (n == 0) {
    lv_obj_t *c = card(agList, 0, 0, 688, 120);
    lv_obj_t *t = mkLabel(c, LV_SYMBOL_OK "  Nothing in the next two weeks", F_L, C_GOOD);
    lv_obj_set_pos(t, 24, 40);
    return;
  }
  time_t now = time(nullptr);
  struct tm nw; localtime_r(&now, &nw);
  int64_t today = daysFromCivil(nw.tm_year + 1900, nw.tm_mon + 1, nw.tm_mday);
  int64_t lastDay = -1;
  bool nextMarked = false;
  for (int i = 0; i < n; i++) {
    struct tm t; localtime_r(&local[i].start, &t);
    int64_t day = daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    if (day < today) day = today;                                  // multi-day event already running
    if (day != lastDay) {
      lastDay = day;
      char h[40];
      if (day == today) snprintf(h, sizeof(h), "TODAY  -  %s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
      else if (day == today + 1) snprintf(h, sizeof(h), "TOMORROW  -  %s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
      else snprintf(h, sizeof(h), "%s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
      lv_obj_t *hl = mkLabel(agList, h, F_S, C_ACCENT);
      lv_obj_set_style_text_letter_space(hl, 2, 0);
      lv_obj_set_style_pad_top(hl, 6, 0);
    }
    bool isNext = (!nextMarked && local[i].end > now);
    if (isNext) nextMarked = true;
    lv_obj_t *r = panel(agList, 0, 0, 688, 66, isNext ? C_CARD3 : C_CARD, 18);
    if (isNext) {
      lv_obj_set_style_border_width(r, 6, 0);
      lv_obj_set_style_border_color(r, lv_color_hex(C_ACCENT), 0);
      lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
    }
    char tm[12];
    if (local[i].allDay) strlcpy(tm, "All day", sizeof(tm)); else snprintf(tm, sizeof(tm), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_obj_t *lt = mkLabel(r, tm, F_M, isNext ? C_ACCENT : C_MUTED);
    lv_obj_set_pos(lt, 22, 20);
    lv_obj_t *ln = mkLabel(r, local[i].title, F_M, C_TEXT);
    lv_obj_set_width(ln, local[i].loc[0] ? 340 : 540);
    lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ln, 128, 20);
    if (local[i].loc[0]) {
      lv_obj_t *ll = mkLabel(r, local[i].loc, F_XS, C_MUTED);
      lv_obj_set_width(ll, 190);
      lv_label_set_long_mode(ll, LV_LABEL_LONG_DOT);
      lv_obj_align(ll, LV_ALIGN_RIGHT_MID, -16, 0);
    }
  }
}

// reminders: banner + beeps N minutes before a timed appointment (called from the 5 Hz tick)
static time_t lastRemindedStart = 0;
static void agendaTick() {
#if REMIND_MINUTES_BEFORE > 0
  if (!timeIsValid()) return;
  Appt a;
  if (!agendaNext(a) || a.allDay) return;
  time_t now = time(nullptr);
  long dt = (long)(a.start - now);
  if (dt > 0 && dt <= REMIND_MINUTES_BEFORE * 60 && a.start != lastRemindedStart) {
    lastRemindedStart = a.start;
    char m[100]; snprintf(m, sizeof(m), "In %ld min: %s", (dt + 59) / 60, a.title);
    postUi(UM_TOAST, m, C_WARN);
    beep(3);
  }
#endif
}
