#pragma once
// Ambient mode: photo frame (photos uploaded to the hub's flash, optionally the SD card) with a clock overlay,
// or a giant night clock. Starts after AMBIENT_AFTER_MIN idle minutes; tap to wake.

#define MAX_PHOTOS 150
#define BL_AMBIENT 70
#define BL_NIGHT   215
// Photos that overhang the screen after "cover" scaling (portraits, panoramas) are kept up to 1.5x the screen in
// that direction and panned slowly across while shown. Buffers are sized for the largest case.
#define PAN_MAX_W   (SCREEN_W * 3 / 2)
#define PAN_MAX_H   (SCREEN_H * 3 / 2)
#define PAN_MIN_PX  40                                          // less overhang than this: just crop, don't pan
#define PHOTO_BUF_PX ((uint32_t)SCREEN_W * PAN_MAX_H > (uint32_t)PAN_MAX_W * SCREEN_H ? (uint32_t)SCREEN_W * PAN_MAX_H : (uint32_t)PAN_MAX_W * SCREEN_H)

static lv_obj_t *scrAmbient, *imgFront, *imgBack;
static lv_obj_t *ambPhotoUi, *ambClockUi;                      // overlay groups for the two looks
static lv_obj_t *ambTime, *ambDate, *ambWx, *ambBigTime, *ambBigDate, *ambWake;
static lv_obj_t *ambSleepCard, *ambSleepIcon, *ambSleepVal, *ambSleepCap, *ambWxCard, *ambWxIcon, *ambWxVal, *ambWxCap;
static lv_image_dsc_t dscA, dscB;
static uint16_t *bufA = nullptr, *bufB = nullptr;
static bool frontIsA = true;
static bool ambientActive = false;
static bool ambNight = false;
static bool ambPhotoLook = false;                               // showing photos (else the clock)
// screensaver: 0 auto (photos by day, clock at night), 1 always photos, 2 always the clock. More page, kept in NVS.
static uint8_t ambMode = 0;
static const char *AMB_MODE_NAMES[3] = {"Auto", "Photos", "Clock"};
static uint32_t ambNextSlide = 0;
static LGFX_Sprite photoSpr;   // decoder only, renders into our own buffers
static bool sprSwap = false;

// src 0 = LittleFS, 1 = SD. taken: when the photo was taken (from its name, d202308141822_..., set by the phone
// page; 0 = unknown). Place, weather etc. are in a .json next to it, read when the photo is shown.
struct PhotoRef { char path[56]; uint8_t src; time_t taken; };
static PhotoRef *photos = nullptr;
static int nPhotos = 0, photoIdx = -1;
static uint8_t photoOrder[MAX_PHOTOS];                          // shuffled play order, reshuffled each round
static int orderPos = 0, orderN = 0;
static time_t nameTaken(const char *name) {                     // "d202308141822_..." -> local time, else 0
  if (name[0] != 'd' || strlen(name) < 13) return 0;
  for (int i = 1; i <= 12; i++) if (!isdigit((unsigned char)name[i])) return 0;
  auto num = [&](int at, int len) { int v = 0; for (int i = 0; i < len; i++) v = v * 10 + (name[at + i] - '0'); return v; };
  struct tm t = {};
  t.tm_year = num(1, 4) - 1900; t.tm_mon = num(5, 2) - 1; t.tm_mday = num(7, 2);
  t.tm_hour = num(9, 2); t.tm_min = num(11, 2); t.tm_isdst = -1;
  return mktime(&t);
}
// taken on today's date in an earlier year
static bool onThisDay(time_t taken) {
  struct tm now, t;
  if (!taken || !nowTm(&now)) return false;
  localtime_r(&taken, &t);
  return t.tm_mon == now.tm_mon && t.tm_mday == now.tm_mday && t.tm_year < now.tm_year;
}
static int nextShuffled() {
  if (orderPos >= orderN || orderN != nPhotos) {
    orderN = nPhotos; orderPos = 0;
    for (int i = 0; i < orderN; i++) photoOrder[i] = i;
    for (int i = orderN - 1; i > 0; i--) { int j = esp_random() % (i + 1); uint8_t t = photoOrder[i]; photoOrder[i] = photoOrder[j]; photoOrder[j] = t; }
    // "on this day" photos lead each round
    std::stable_partition(photoOrder, photoOrder + orderN, [](uint8_t k) { return onThisDay(photos[k].taken); });
    if (orderN > 1 && photoOrder[0] == photoIdx) { uint8_t t = photoOrder[0]; photoOrder[0] = photoOrder[1]; photoOrder[1] = t; }  // no repeat across rounds
  }
  return photoOrder[orderPos++];
}
static bool ambWantPhotos(bool night) { return nPhotos > 0 && (ambMode == 1 || (ambMode == 0 && !night)); }
static bool sdOk = false;
static bool lfsOk = false;

#if ENABLE_SD
static SPIClass SD_SPI(FSPI);
static void sdMount() {
  SD_SPI.begin(5, 4, 6, 0);                                   // SCK, MISO, MOSI, CS (Elecrow lesson-04)
  sdOk = SD.begin(0, SD_SPI, 40000000);
  Serial.println(sdOk ? "SD mounted" : "SD not found (switches S1 S0 = 11?)");
}
#endif

static bool isImageName(const char *n) {
  const char *dot = strrchr(n, '.');
  if (!dot) return false;
  return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".png");
}
static void scanDir(fs::FS &fsys, const char *dir, uint8_t src) {
  File root = fsys.open(dir);
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f && nPhotos < MAX_PHOTOS) {
    if (!f.isDirectory() && isImageName(f.name()) && f.name()[0] != '.') {
      String p = f.path();
      if (p.length() < sizeof(photos[0].path)) {
        strlcpy(photos[nPhotos].path, p.c_str(), sizeof(photos[0].path));
        photos[nPhotos].src = src;
        photos[nPhotos].taken = nameTaken(f.name());
        nPhotos++;
      }
    }
    f = root.openNextFile();
  }
}
static void photosRescan() {
  if (!photos) photos = (PhotoRef *)heap_caps_malloc(sizeof(PhotoRef) * MAX_PHOTOS, MALLOC_CAP_SPIRAM);
  nPhotos = 0;
  if (!photos) return;
  if (lfsOk) scanDir(LittleFS, "/photos", 0);
#if ENABLE_SD
  if (sdOk) { scanDir(SD, "/photos", 1); scanDir(SD, "/", 1); }
#endif
  // shuffle so it isn't the same order every time
  for (int i = nPhotos - 1; i > 0; i--) { int j = random(i + 1); PhotoRef t = photos[i]; photos[i] = photos[j]; photos[j] = t; }
  photoIdx = -1;
}

static bool jpegDims(fs::FS &fsys, const char *path, int &w, int &h) {
  File f = fsys.open(path, "r");
  if (!f) return false;
  uint8_t b[4];
  if (f.read(b, 2) != 2 || b[0] != 0xFF || b[1] != 0xD8) { f.close(); return false; }
  while (f.read(b, 1) == 1) {
    if (b[0] != 0xFF) continue;
    uint8_t m;
    do { if (f.read(&m, 1) != 1) { f.close(); return false; } } while (m == 0xFF);
    if (m == 0xD8 || m == 0x01 || m == 0x00 || (m >= 0xD0 && m <= 0xD7)) continue;
    uint8_t L[2];
    if (f.read(L, 2) != 2) break;
    int len = (L[0] << 8) | L[1];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      uint8_t d[5];
      if (f.read(d, 5) != 5) break;
      h = (d[1] << 8) | d[2]; w = (d[3] << 8) | d[4];
      f.close(); return true;
    }
    f.seek(f.position() + len - 2);
  }
  f.close();
  return false;
}

// LovyanGFX sprites may store RGB565 byte-swapped; LVGL wants native. Find out once, by experiment.
static void detectSwap() {
  static uint16_t probe[4];
  photoSpr.setColorDepth(16);
  photoSpr.setBuffer(probe, 2, 2, 16);
  photoSpr.fillScreen(photoSpr.color565(255, 0, 0));
  sprSwap = (probe[0] != 0xF800);
}

// PNG size from the IHDR chunk: 8-byte signature, then length + "IHDR", then big-endian width and height
static bool pngDims(fs::FS &fsys, const char *path, int &w, int &h) {
  File f = fsys.open(path, "r");
  if (!f) return false;
  uint8_t b[24];
  bool ok = f.read(b, 24) == 24 && !memcmp(b + 12, "IHDR", 4);
  f.close();
  if (!ok) return false;
  w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
  h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
  return true;
}
// Blurred backdrop for photos shown whole ("fit"): decode the photo tiny (cover-cropped), box-blur it, then stretch it
// bilinearly over the screen and darken it. Pixels are in the sprite's byte order (sprSwap), like the rest of dst.
#define BG_W (SCREEN_W / 8)
#define BG_H (SCREEN_H / 8)
static inline uint16_t sw16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static void blurBackdrop(uint16_t *dst, uint16_t *small) {
  static uint8_t *work = nullptr;                              // r5 g6 b5 planes + scratch, in PSRAM (internal RAM is tight)
  if (!work) work = (uint8_t *)heap_caps_malloc(4 * BG_W * BG_H, MALLOC_CAP_SPIRAM);
  if (!work) { memset(dst, 0, SCREEN_W * SCREEN_H * 2); return; }
  uint8_t *ch[3] = {work, work + BG_W * BG_H, work + 2 * BG_W * BG_H}, *tmp = work + 3 * BG_W * BG_H;
  for (int i = 0; i < BG_W * BG_H; i++) {
    uint16_t c = sprSwap ? sw16(small[i]) : small[i];
    ch[0][i] = c >> 11; ch[1][i] = (c >> 5) & 63; ch[2][i] = c & 31;
  }
  for (int k = 0; k < 3; k++) for (int pass = 0; pass < 2; pass++) {   // separable 5-tap box blur, twice
    uint8_t *p = ch[k];
    for (int y = 0; y < BG_H; y++) for (int x = 0; x < BG_W; x++) {
      int s = 0; for (int d = -2; d <= 2; d++) s += p[y * BG_W + std::min(std::max(x + d, 0), BG_W - 1)];
      tmp[y * BG_W + x] = s / 5;
    }
    for (int y = 0; y < BG_H; y++) for (int x = 0; x < BG_W; x++) {
      int s = 0; for (int d = -2; d <= 2; d++) s += tmp[std::min(std::max(y + d, 0), BG_H - 1) * BG_W + x];
      p[y * BG_W + x] = s / 5;
    }
  }
  for (int y = 0; y < SCREEN_H; y++) {                         // bilinear stretch x8, darkened to ~40 %
    int fy = (y * (BG_H - 1) * 256) / (SCREEN_H - 1), y0 = fy >> 8, y1 = std::min(y0 + 1, BG_H - 1), wy = fy & 255;
    for (int x = 0; x < SCREEN_W; x++) {
      int fx = (x * (BG_W - 1) * 256) / (SCREEN_W - 1), x0 = fx >> 8, x1 = std::min(x0 + 1, BG_W - 1), wx = fx & 255;
      int v[3];
      for (int k = 0; k < 3; k++) {
        const uint8_t *p = ch[k];
        int top = p[y0 * BG_W + x0] * (256 - wx) + p[y0 * BG_W + x1] * wx;
        int bot = p[y1 * BG_W + x0] * (256 - wx) + p[y1 * BG_W + x1] * wx;
        v[k] = ((top * (256 - wy) + bot * wy) >> 16) * 2 / 5;
      }
      uint16_t c = (uint16_t)((v[0] << 11) | (v[1] << 5) | v[2]);
      dst[y * SCREEN_W + x] = sprSwap ? sw16(c) : c;
    }
  }
}

// "cover" scale: fill the whole screen, trimming whichever edges overhang
static float coverScale(int w, int h) {
  return (w > 0 && h > 0) ? std::max((float)SCREEN_W / w, (float)SCREEN_H / h) : 1.0f;
}

static bool decodeInto(uint16_t *dst, const PhotoRef &p, int &bw, int &bh) {
  fs::FS &fsys = (p.src == 1)
#if ENABLE_SD
    ? (fs::FS &)SD
#else
    ? (fs::FS &)LittleFS
#endif
    : (fs::FS &)LittleFS;
  const char *dot = strrchr(p.path, '.');
  bool isBmp = !strcasecmp(dot, ".bmp"), isPng = !strcasecmp(dot, ".png");
  int w = 0, h = 0;
  bool dims = isBmp ? false : isPng ? pngDims(fsys, p.path, w, h) : jpegDims(fsys, p.path, w, h);
  float sc = dims ? coverScale(w, h) : 1.0f;
  bw = SCREEN_W; bh = SCREEN_H;
  bool fit = false;
  if (dims) {
    int sw = (int)(w * sc), sh = (int)(h * sc);
    if (sw > PAN_MAX_W || sh > PAN_MAX_H) fit = true;           // too far off the screen's shape (portrait, panorama):
    else {                                                     // show it whole on a blurred copy of itself
      // close to the screen's shape: fill the screen and pan across the overhang, so all of it gets seen
      if (sw - SCREEN_W >= PAN_MIN_PX) bw = sw;
      if (sh - SCREEN_H >= PAN_MIN_PX) bh = sh;
    }
  }
  photoSpr.setColorDepth(16);
  if (fit) {
    static uint16_t *small = nullptr;
    if (!small) small = (uint16_t *)heap_caps_malloc(BG_W * BG_H * 2, MALLOC_CAP_SPIRAM);
    if (!small) fit = false;
    else {
      float bs = std::max((float)BG_W / w, (float)BG_H / h);
      photoSpr.setBuffer(small, BG_W, BG_H, 16);
      photoSpr.fillScreen(TFT_BLACK);
      if (isPng) photoSpr.drawPngFile(fsys, p.path, 0, 0, BG_W, BG_H, 0, 0, bs, bs, lgfx::datum_t::middle_center);
      else photoSpr.drawJpgFile(fsys, p.path, 0, 0, BG_W, BG_H, 0, 0, bs, bs, lgfx::datum_t::middle_center);
      blurBackdrop(dst, small);
      sc = std::min((float)SCREEN_W / w, (float)SCREEN_H / h);   // whole photo on top
    }
  }
  photoSpr.setBuffer(dst, bw, bh, 16);
  if (!fit) photoSpr.fillScreen(TFT_BLACK);
  // x/y is the top-left of a bw x bh box; middle_center centres the scaled image in it and the box clips the rest
  bool ok;
  if (isBmp) ok = photoSpr.drawBmpFile(fsys, p.path, 0, 0, bw, bh, 0, 0, 1.0f, 0.0f, lgfx::datum_t::middle_center);
  else if (isPng) ok = photoSpr.drawPngFile(fsys, p.path, 0, 0, bw, bh, 0, 0, sc, sc, lgfx::datum_t::middle_center);
  else ok = photoSpr.drawJpgFile(fsys, p.path, 0, 0, bw, bh, 0, 0, sc, sc, lgfx::datum_t::middle_center);
  if (ok && sprSwap) {
    uint32_t n = (uint32_t)bw * bh;
    for (uint32_t i = 0; i < n; i++) dst[i] = (uint16_t)((dst[i] >> 8) | (dst[i] << 8));
  }
  return ok;
}

// ───────────── photo caption: where, when, how long ago, the weather that day, how far from home ─────────────
static lv_obj_t *ambCap, *ambCapTitle, *ambCapLine;
static void agoText(time_t then, char *b, size_t n) {
  struct tm now, t;
  if (!nowTm(&now)) { b[0] = 0; return; }
  localtime_r(&then, &t);
  int years = now.tm_year - t.tm_year - ((now.tm_mon < t.tm_mon || (now.tm_mon == t.tm_mon && now.tm_mday < t.tm_mday)) ? 1 : 0);
  long days = (long)((time(nullptr) - then) / 86400);
  if (onThisDay(then)) snprintf(b, n, "%d year%s ago today", now.tm_year - t.tm_year, now.tm_year - t.tm_year == 1 ? "" : "s");
  else if (days < 1) snprintf(b, n, "Today");
  else if (days < 2) snprintf(b, n, "Yesterday");
  else if (days < 14) snprintf(b, n, "%ld days ago", days);
  else if (days < 60) snprintf(b, n, "%ld weeks ago", days / 7);
  else if (years < 1) snprintf(b, n, "%ld months ago", days / 30);
  else snprintf(b, n, years == 1 ? "A year ago" : "%d years ago", years);
}
static float distKm(float lat1, float lon1, float lat2, float lon2) {   // haversine
  const float R = 6371.0f, r = (float)M_PI / 180.0f;
  float dl = (lat2 - lat1) * r, dn = (lon2 - lon1) * r;
  float a = sinf(dl / 2) * sinf(dl / 2) + cosf(lat1 * r) * cosf(lat2 * r) * sinf(dn / 2) * sinf(dn / 2);
  return 2 * R * asinf(std::min(1.0f, sqrtf(a)));
}
static void photoCaption(const PhotoRef &p) {
  char place[64] = "", when[24] = "", parts[200] = "";
  time_t taken = p.taken;
  int wc = -1, tx = 0; bool pos = false; float lat = 0, lon = 0;
  if (p.src == 0) {                                             // the phone page's metadata, next to the photo
    String j = String(p.path);
    j = j.substring(0, j.lastIndexOf('.')) + ".json";
    File f = LittleFS.exists(j) ? LittleFS.open(j, "r") : File();
    if (f) {
      JsonDocument d;
      if (!deserializeJson(d, f)) {
        copyText(place, sizeof(place), d["p"] | "");
        if (d["wc"].is<int>()) { wc = d["wc"]; tx = d["tx"] | 0; }
        if (d["lat"].is<float>() && d["lon"].is<float>()) { pos = true; lat = d["lat"]; lon = d["lon"]; }
      }
      f.close();
    }
  }
  auto add = [&](const char *s) { if (!s[0]) return; if (parts[0]) strlcat(parts, " \xC2\xB7 ", sizeof(parts)); strlcat(parts, s, sizeof(parts)); };
  if (taken) {
    struct tm t; localtime_r(&taken, &t);
    snprintf(when, sizeof(when), "%d %s %d", t.tm_mday, MON3[t.tm_mon], t.tm_year + 1900);
    if (place[0]) add(when);
    char ago[32]; agoText(taken, ago, sizeof(ago)); add(ago);
  }
  if (wc >= 0) {
    char w[48]; snprintf(w, sizeof(w), "%d\xC2\xB0 %s", USE_CELSIUS ? tx : tx * 9 / 5 + 32, wxText(wc)); add(w);
  }
  if (pos) {
    float km = distKm(wxLat, wxLon, lat, lon);
    char d[40];
    if (km < 2) strlcpy(d, "At home", sizeof(d));
    else {
      long v = lroundf(km * (USE_MPH ? 0.621f : 1.0f));
      const char *u = USE_MPH ? "miles" : "km";
      if (v >= 1000) snprintf(d, sizeof(d), "%ld,%03ld %s from home", v / 1000, v % 1000, u);   // 1,640 km
      else snprintf(d, sizeof(d), "%ld %s from home", v, u);
    }
    add(d);
  }
  const char *title = place[0] ? place : when;
  if (!title[0]) { lv_obj_add_flag(ambCap, LV_OBJ_FLAG_HIDDEN); return; }   // nothing known: no caption
  lv_label_set_text(ambCapTitle, title);
  lv_label_set_text(ambCapLine, parts);
  if (parts[0]) lv_obj_remove_flag(ambCapLine, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ambCapLine, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ambCap, LV_OBJ_FLAG_HIDDEN);
}

static void fadeCb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void panXCb(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); }
static void panYCb(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }

static void setPhotoSize(lv_image_dsc_t &d, int w, int h) {
  d.header.w = w; d.header.h = h;
  d.header.stride = w * 2;
  d.data_size = (uint32_t)w * h * 2;
}
// glide from one end of the overhang to the other while the photo is up (fade in + slide time), alternating ends
static void startPan(lv_obj_t *img, int w, int h) {
  static bool fromEnd = false;
  lv_anim_delete(img, panXCb); lv_anim_delete(img, panYCb);
  int ox = SCREEN_W - w, oy = SCREEN_H - h;                      // <= 0: how far the image can slide
  lv_obj_set_pos(img, ox / 2, oy / 2);
  if (ox == 0 && oy == 0) return;
  fromEnd = !fromEnd;
  bool horiz = ox != 0;
  int32_t span = horiz ? ox : oy;
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, img);
  lv_anim_set_exec_cb(&a, horiz ? panXCb : panYCb);
  lv_anim_set_values(&a, fromEnd ? span : 0, fromEnd ? 0 : span);
  lv_anim_set_duration(&a, (uint32_t)AMBIENT_SLIDE_SEC * 1000UL + 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

static bool nextPhoto() {
  if (nPhotos == 0 || !bufA || !bufB) return false;
  for (int tries = 0; tries < nPhotos && tries < 4; tries++) {
    photoIdx = nextShuffled();
    uint16_t *dst = frontIsA ? bufB : bufA;                    // decode into the hidden buffer
    int w, h;
    if (!decodeInto(dst, photos[photoIdx], w, h)) continue;
    photoCaption(photos[photoIdx]);
    lv_obj_t *back = frontIsA ? imgBack : imgFront;
    lv_obj_t *front = frontIsA ? imgFront : imgBack;
    lv_image_dsc_t &d = frontIsA ? dscB : dscA;
    setPhotoSize(d, w, h);
    lv_image_cache_drop(&d);
    lv_image_set_src(back, &d);
    startPan(back, w, h);
    lv_obj_set_style_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_move_foreground(back);
    lv_obj_move_foreground(ambPhotoUi);                        // overlay stays on top
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, back); lv_anim_set_exec_cb(&a, fadeCb);
    lv_anim_set_values(&a, 0, 255); lv_anim_set_duration(&a, 900);
    lv_anim_start(&a);
    (void)front;
    frontIsA = !frontIsA;
    return true;
  }
  return false;
}

static void ambientExit();
static void onAmbientTap(lv_event_t *) { ambientExit(); }

static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *p = panel(parent, x, y, w, h, 0x000000, 28);
  lv_obj_set_style_bg_opa(p, LV_OPA_60, 0);
  return p;
}

static void ambientBuild() {
  scrAmbient = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrAmbient, lv_color_hex(0x000000), 0);
  lv_obj_remove_flag(scrAmbient, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scrAmbient, onAmbientTap, LV_EVENT_CLICKED, nullptr);

  bufA = (uint16_t *)heap_caps_malloc(PHOTO_BUF_PX * 2, MALLOC_CAP_SPIRAM);
  bufB = (uint16_t *)heap_caps_malloc(PHOTO_BUF_PX * 2, MALLOC_CAP_SPIRAM);
  lv_image_dsc_t *ds[2] = {&dscA, &dscB};
  uint16_t *bs[2] = {bufA, bufB};
  for (int i = 0; i < 2; i++) {
    memset(ds[i], 0, sizeof(lv_image_dsc_t));
    ds[i]->header.magic = LV_IMAGE_HEADER_MAGIC;
    ds[i]->header.cf = LV_COLOR_FORMAT_RGB565;
    setPhotoSize(*ds[i], SCREEN_W, SCREEN_H);
    ds[i]->data = (const uint8_t *)bs[i];
  }
  imgFront = lv_image_create(scrAmbient); lv_obj_set_pos(imgFront, 0, 0);
  imgBack  = lv_image_create(scrAmbient); lv_obj_set_pos(imgBack, 0, 0);
  if (bufA) lv_image_set_src(imgFront, &dscA);
  lv_obj_add_flag(imgFront, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(imgBack, LV_OBJ_FLAG_EVENT_BUBBLE);

  // photo look: small clock + weather pills at the bottom
  ambPhotoUi = clearBox(scrAmbient, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(ambPhotoUi, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(ambPhotoUi, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *p1 = pill(ambPhotoUi, 24, 372, 250, 88);
  ambTime = mkLabel(p1, "--:--", F_T34, 0xEDE6DD);
  lv_obj_set_pos(ambTime, 22, 10);
  ambDate = mkLabel(p1, "", F_XS, 0xB3A99D);
  lv_obj_set_pos(ambDate, 24, 58);
  lv_obj_t *p2 = pill(ambPhotoUi, 560, 394, 216, 66);
  ambWx = mkLabel(p2, "", F_M, 0xEDE6DD);
  lv_obj_center(ambWx);
  // caption, top left, sized to its text: place (or date) over "14 Aug 2023 · 3 years ago today · 27° Clear sky..."
  ambCap = pill(ambPhotoUi, 24, 24, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(ambCap, 22, 0);
  lv_obj_set_style_pad_hor(ambCap, 18, 0);
  lv_obj_set_style_pad_ver(ambCap, 12, 0);
  lv_obj_set_flex_flow(ambCap, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ambCap, 2, 0);
  ambCapTitle = mkLabel(ambCap, "", F_L, 0xEDE6DD);
  ambCapLine = mkLabel(ambCap, "", F_S, 0xB3A99D);
  lv_obj_set_style_max_width(ambCapTitle, 640, 0);
  lv_obj_set_style_max_width(ambCapLine, 640, 0);
  lv_obj_add_flag(ambCap, LV_OBJ_FLAG_HIDDEN);

  // clock look (StandBy style, Daylight-Night mockup): date + huge time on the left, sleep timer and overnight
  // weather cards on the right. At night everything is one dim red on black; by day the dark theme's text colours.
  ambClockUi = clearBox(scrAmbient, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(ambClockUi, LV_OBJ_FLAG_CLICKABLE);
  ambBigDate = mkLabel(ambClockUi, "", F_S, T_NIGHT);
  lv_obj_set_style_text_letter_space(ambBigDate, 1, 0);
  lv_obj_set_pos(ambBigDate, 28, 128);
  ambBigTime = mkLabel(ambClockUi, "--:--", F_NIGHT, T_NIGHT);
  lv_obj_set_style_text_letter_space(ambBigTime, -18, 0);
  lv_obj_set_pos(ambBigTime, 12, 152);

  lv_obj_t *col = lv_obj_create(ambClockUi);                     // the cards + hint, centred vertically on the right
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 168, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 12, 0);
  lv_obj_align(col, LV_ALIGN_RIGHT_MID, -24, 0);
  lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  auto nightCard = [&](lv_obj_t *&cardObj, lv_obj_t *&icon, lv_obj_t *&val, lv_obj_t *&cap, const char *glyph) {
    cardObj = panel(col, 0, 0, 168, LV_SIZE_CONTENT, T_NIGHT_BG, 26);
    lv_obj_set_style_border_width(cardObj, 1, 0);
    lv_obj_set_style_pad_all(cardObj, 16, 0);
    lv_obj_set_flex_flow(cardObj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cardObj, 6, 0);
    lv_obj_add_flag(cardObj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(cardObj, LV_OBJ_FLAG_CLICKABLE);
    icon = mkLabel(cardObj, glyph, I_24, T_NIGHT);
    val = mkLabel(cardObj, "", F_T26, T_NIGHT);
    cap = mkLabel(cardObj, "", F_XS, T_NIGHT);
    lv_obj_set_width(cap, 168 - 32);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  };
  nightCard(ambSleepCard, ambSleepIcon, ambSleepVal, ambSleepCap, LUCIDE_MOON);
  nightCard(ambWxCard, ambWxIcon, ambWxVal, ambWxCap, LUCIDE_CLOUD);
  ambWake = mkLabel(col, "TAP TO WAKE", F_XS, T_NIGHT);
  lv_obj_set_width(ambWake, 168);
  lv_obj_set_style_text_align(ambWake, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_letter_space(ambWake, 1, 0);
  lv_obj_add_flag(ambSleepCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN);
}

static void ambientLook(bool clockLook, bool night) {
  ambNight = night;
  if (clockLook) { lv_obj_add_flag(imgFront, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(imgBack, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(ambPhotoUi, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_remove_flag(imgFront, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(imgBack, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(ambPhotoUi, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN); }
  // one colour for everything at night (red chosen to stay readable with the backlight down); by day dark-theme text
  uint32_t fg = night ? T_NIGHT : THEME_DARK.text, sub = night ? T_NIGHT : THEME_DARK.muted;
  uint32_t line = night ? T_NIGHT_LINE : THEME_DARK.outline;
  lv_obj_t *bigs[] = {ambBigTime, ambSleepVal, ambWxVal, ambSleepIcon, ambWxIcon};
  lv_obj_t *smalls[] = {ambBigDate, ambSleepCap, ambWxCap, ambWake};
  for (lv_obj_t *o : bigs) lv_obj_set_style_text_color(o, lv_color_hex(fg), 0);
  for (lv_obj_t *o : smalls) lv_obj_set_style_text_color(o, lv_color_hex(sub), 0);
  lv_obj_set_style_border_color(ambSleepCard, lv_color_hex(line), 0);
  lv_obj_set_style_border_color(ambWxCard, lv_color_hex(line), 0);
}

static void ambientEnter() {
  if (ambientActive) return;
  photosRescan();
  bool night = isNight();
  bool havePhoto = ambWantPhotos(night) && nextPhoto();
  ambPhotoLook = havePhoto;
  ambientLook(!havePhoto, night);
  ambientActive = true;
  ambNextSlide = millis() + (uint32_t)AMBIENT_SLIDE_SEC * 1000UL;
  stc(night ? BL_NIGHT : BL_AMBIENT);
  lv_screen_load_anim(scrAmbient, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
}

static lv_obj_t *scrMainRef;                                    // set by the .ino
static void ambientExit() {
  if (!ambientActive) return;
  ambientActive = false;
  stc(STC_BL_MAX);
  lv_screen_load_anim(scrMainRef, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

static void ambientTick() {
  if (!ambientActive) return;
  static uint32_t lastSec = 0;
  uint32_t now = millis();
  bool night = isNight();
  if (night != ambNight) {                                      // crossed into/out of night
    bool photoLook = ambWantPhotos(night);
    ambPhotoLook = photoLook;
    ambientLook(!photoLook, night);
    stc(night ? BL_NIGHT : BL_AMBIENT);
    if (photoLook) nextPhoto();
  }
  if (ambPhotoLook && nPhotos > 1 && (int32_t)(now - ambNextSlide) >= 0) { ambNextSlide = now + (uint32_t)AMBIENT_SLIDE_SEC * 1000UL; nextPhoto(); }
  if (now - lastSec < 1000) return;
  lastSec = now;
  // burn-in protection: once a minute nudge the clock / overlays along a slow wandering path (+-16 x, +-12 y px)
  static uint32_t lastShift = 0, step = 0;
  if (now - lastShift >= 60000) {
    lastShift = now; step++;
    int dx = (int)lroundf(16.0f * sinf(step * 0.9f)), dy = (int)lroundf(12.0f * sinf(step * 0.53f + 1.0f));
    lv_obj_set_pos(ambClockUi, dx, dy);
    lv_obj_set_pos(ambPhotoUi, dx, dy);
  }
  struct tm t;
  if (!nowTm(&t)) return;
  // labels only change when their text does (once a minute at most): no redraws, no flicker
  auto put = [](lv_obj_t *l, const char *s) { if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s); };
  char b[12], d[40], up[24];
  snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(d, sizeof(d), "%s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
  snprintf(up, sizeof(up), "%s %d %s", DOW3[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
  for (char *p = up; *p; p++) *p = toupper(*p);
  put(ambTime, b); put(ambDate, d);
  put(ambBigTime, b); put(ambBigDate, up);
  // radio sleep timer card: only while one runs
  if (sleepAt) {
    char m[16], cap[80];
    int32_t left = (int32_t)(sleepAt - now);
    snprintf(m, sizeof(m), "%d min", left > 0 ? (int)(left / 60000 + 1) : 1);
    snprintf(cap, sizeof(cap), "Radio sleep \xC2\xB7 %s", curSt.name);
    put(ambSleepVal, m); put(ambSleepCap, cap);
    lv_obj_remove_flag(ambSleepCard, LV_OBJ_FLAG_HIDDEN);
  } else lv_obj_add_flag(ambSleepCard, LV_OBJ_FLAG_HIDDEN);
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  if (w.valid) {
    char s[48]; snprintf(s, sizeof(s), "%d\xC2\xB0 %s", w.temp, wxText(w.code));
    put(ambWx, s);
    // overnight card: now, and what it'll be like in about six hours (hours[] steps by two)
    const WxHour &later = w.hours[3];
    char v[12], o[64];
    snprintf(v, sizeof(v), "%d\xC2\xB0", w.temp);
    snprintf(o, sizeof(o), "%s by %02d:00", wxText(later.code), later.hour);
    put(ambWxVal, v); put(ambWxCap, o);
    static int lastCode = -1; static bool lastDay = true;
    if (lastCode != w.code || lastDay != w.day) { lastCode = w.code; lastDay = w.day; wxDraw(ambWxIcon, wxKindOf(w.code), !w.day); }
  }
}
