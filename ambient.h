#pragma once
// Ambient mode: photo frame (photos uploaded to the hub's flash, optionally the SD card) with a clock overlay,
// or a giant night clock. Starts after AMBIENT_AFTER_MIN idle minutes; tap to wake.

#define MAX_PHOTOS 150
#define BL_AMBIENT 70
#define BL_NIGHT   215

static lv_obj_t *scrAmbient, *imgFront, *imgBack;
static lv_obj_t *ambPhotoUi, *ambClockUi;                      // overlay groups for the two looks
static lv_obj_t *ambTime, *ambDate, *ambWx, *ambBigTime, *ambBigDate, *ambBigWx, *ambBigNext, *ambWxIcon;
static lv_image_dsc_t dscA, dscB;
static uint16_t *bufA = nullptr, *bufB = nullptr;
static bool frontIsA = true;
static bool ambientActive = false;
static bool ambNight = false;
static uint32_t ambNextSlide = 0;
static LGFX_Sprite photoSpr;   // decoder only, renders into our own buffers
static bool sprSwap = false;

struct PhotoRef { char path[56]; uint8_t src; };               // src 0 = LittleFS, 1 = SD
static PhotoRef *photos = nullptr;
static int nPhotos = 0, photoIdx = -1;
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
      if (p.length() < sizeof(photos[0].path)) { strlcpy(photos[nPhotos].path, p.c_str(), sizeof(photos[0].path)); photos[nPhotos].src = src; nPhotos++; }
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
// "cover" scale: fill the whole screen, trimming whichever edges overhang
static float coverScale(int w, int h) {
  return (w > 0 && h > 0) ? std::max((float)SCREEN_W / w, (float)SCREEN_H / h) : 1.0f;
}

static bool decodeInto(uint16_t *dst, const PhotoRef &p) {
  fs::FS &fsys = (p.src == 1)
#if ENABLE_SD
    ? (fs::FS &)SD
#else
    ? (fs::FS &)LittleFS
#endif
    : (fs::FS &)LittleFS;
  photoSpr.setColorDepth(16);
  photoSpr.setBuffer(dst, SCREEN_W, SCREEN_H, 16);
  photoSpr.fillScreen(TFT_BLACK);
  const char *dot = strrchr(p.path, '.');
  bool ok = false;
  // x/y is the top-left of a SCREEN_W x SCREEN_H box; middle_center centres the scaled image in it and the box clips
  // the overhang (passing the screen centre as x/y put the image's top-left there: only a quarter was visible)
  int w = 0, h = 0;
  if (!strcasecmp(dot, ".bmp")) ok = photoSpr.drawBmpFile(fsys, p.path, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1.0f, 0.0f, lgfx::datum_t::middle_center);
  else if (!strcasecmp(dot, ".png")) {
    float sc = pngDims(fsys, p.path, w, h) ? coverScale(w, h) : 1.0f;
    ok = photoSpr.drawPngFile(fsys, p.path, 0, 0, SCREEN_W, SCREEN_H, 0, 0, sc, sc, lgfx::datum_t::middle_center);
  } else {
    float sc = jpegDims(fsys, p.path, w, h) ? coverScale(w, h) : 1.0f;
    ok = photoSpr.drawJpgFile(fsys, p.path, 0, 0, SCREEN_W, SCREEN_H, 0, 0, sc, sc, lgfx::datum_t::middle_center);
  }
  if (ok && sprSwap) {
    uint32_t n = SCREEN_W * SCREEN_H;
    for (uint32_t i = 0; i < n; i++) dst[i] = (uint16_t)((dst[i] >> 8) | (dst[i] << 8));
  }
  return ok;
}

static void fadeCb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static bool nextPhoto() {
  if (nPhotos == 0 || !bufA || !bufB) return false;
  for (int tries = 0; tries < nPhotos && tries < 4; tries++) {
    photoIdx = (photoIdx + 1) % nPhotos;
    uint16_t *dst = frontIsA ? bufB : bufA;                    // decode into the hidden buffer
    if (!decodeInto(dst, photos[photoIdx])) continue;
    lv_obj_t *back = frontIsA ? imgBack : imgFront;
    lv_obj_t *front = frontIsA ? imgFront : imgBack;
    lv_image_set_src(back, frontIsA ? &dscB : &dscA);
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

  bufA = (uint16_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
  bufB = (uint16_t *)heap_caps_malloc(SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
  lv_image_dsc_t *ds[2] = {&dscA, &dscB};
  uint16_t *bs[2] = {bufA, bufB};
  for (int i = 0; i < 2; i++) {
    memset(ds[i], 0, sizeof(lv_image_dsc_t));
    ds[i]->header.magic = LV_IMAGE_HEADER_MAGIC;
    ds[i]->header.cf = LV_COLOR_FORMAT_RGB565;
    ds[i]->header.w = SCREEN_W; ds[i]->header.h = SCREEN_H;
    ds[i]->header.stride = SCREEN_W * 2;
    ds[i]->data_size = SCREEN_W * SCREEN_H * 2;
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
  ambTime = mkLabel(p1, "--:--", F_XL, C_TEXT);
  lv_obj_set_pos(ambTime, 22, 6);
  ambDate = mkLabel(p1, "", F_XS, C_MUTED);
  lv_obj_set_pos(ambDate, 24, 64);
  lv_obj_t *p2 = pill(ambPhotoUi, 560, 394, 216, 66);
  ambWx = mkLabel(p2, "", F_M, C_TEXT);
  lv_obj_center(ambWx);

  // clock look
  ambClockUi = clearBox(scrAmbient, 0, 0, SCREEN_W, SCREEN_H);
  lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(ambClockUi, LV_OBJ_FLAG_CLICKABLE);
  ambBigTime = mkLabel(ambClockUi, "--:--", F_CLK, C_TEXT);
  lv_obj_align(ambBigTime, LV_ALIGN_TOP_MID, 0, 36);
  ambBigDate = mkLabel(ambClockUi, "", F_L, C_MUTED);
  lv_obj_align(ambBigDate, LV_ALIGN_TOP_MID, 0, 214);
  ambWxIcon = wxIcon(ambClockUi, 210, 296, 64, WX_CLOUD, false);
  ambBigWx = mkLabel(ambClockUi, "", F_L, C_TEXT);
  lv_obj_set_pos(ambBigWx, 290, 306);
  ambBigNext = mkLabel(ambClockUi, "", F_M, C_MUTED);
  lv_obj_set_width(ambBigNext, 700);
  lv_obj_set_style_text_align(ambBigNext, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(ambBigNext, LV_LABEL_LONG_DOT);
  lv_obj_align(ambBigNext, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN);
}

static void ambientLook(bool clockLook, bool night) {
  ambNight = night;
  if (clockLook) { lv_obj_add_flag(imgFront, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(imgBack, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(ambPhotoUi, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN); }
  else { lv_obj_remove_flag(imgFront, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(imgBack, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(ambPhotoUi, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(ambClockUi, LV_OBJ_FLAG_HIDDEN); }
  uint32_t tc = night ? 0xB5471F : C_TEXT;                       // warm dim orange at night: easy on the eyes
  lv_obj_set_style_text_color(ambBigTime, lv_color_hex(tc), 0);
  lv_obj_set_style_text_color(ambBigDate, lv_color_hex(night ? 0x6E3115 : C_MUTED), 0);
  lv_obj_set_style_text_color(ambBigWx, lv_color_hex(night ? 0x6E3115 : C_TEXT), 0);
  lv_obj_set_style_text_color(ambBigNext, lv_color_hex(night ? 0x5A2811 : C_MUTED), 0);
  lv_obj_set_style_opa(ambWxIcon, night ? LV_OPA_30 : LV_OPA_COVER, 0);
}

static void ambientEnter() {
  if (ambientActive) return;
  photosRescan();
  bool night = isNight();
  bool havePhoto = !night && nPhotos > 0 && nextPhoto();
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
    bool photoLook = !night && nPhotos > 0;
    ambientLook(!photoLook, night);
    stc(night ? BL_NIGHT : BL_AMBIENT);
    if (photoLook) nextPhoto();
  }
  if (!ambNight && nPhotos > 1 && (int32_t)(now - ambNextSlide) >= 0) { ambNextSlide = now + (uint32_t)AMBIENT_SLIDE_SEC * 1000UL; nextPhoto(); }
  if (now - lastSec < 1000) return;
  lastSec = now;
  struct tm t;
  if (!nowTm(&t)) return;
  char b[12], d[40];
  snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(d, sizeof(d), "%s %d %s", DOWF[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
  lv_label_set_text(ambTime, b);  lv_label_set_text(ambDate, d);
  lv_label_set_text(ambBigTime, b); lv_label_set_text(ambBigDate, d);
  DATA_LOCK(); WxData w = wx; DATA_UNLOCK();
  if (w.valid) {
    char s[48]; snprintf(s, sizeof(s), "%d\xC2\xB0 %s", w.temp, wxText(w.code));
    lv_label_set_text(ambWx, s); lv_label_set_text(ambBigWx, s);
    static int lastCode = -1; static bool lastDay = true;
    if (lastCode != w.code || lastDay != w.day) { lastCode = w.code; lastDay = w.day; wxDraw(ambWxIcon, wxKindOf(w.code), !w.day); }
  }
  Appt a;
  if (agendaNext(a)) {
    char wh[48], s[110]; fmtWhen(a, wh, sizeof(wh));
    snprintf(s, sizeof(s), LV_SYMBOL_BELL "  %s  -  %s", a.title, wh);
    lv_label_set_text(ambBigNext, s);
  } else lv_label_set_text(ambBigNext, "");
}
