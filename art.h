#pragma once
// Station logos and podcast artwork as 64x64 thumbnails. netTask downloads + decodes (JPEG / PNG, cover-cropped) and
// keeps them in /art/<hash>.rgb (raw RGB565); the UI loads them into a small PSRAM cache and fills in lv_image
// widgets as thumbnails arrive. ICO / SVG / WebP / progressive JPEG logos simply stay as placeholders.

#define ART_PX     64
#define ART_BYTES  (ART_PX * ART_PX * 2)
#define ART_SLOTS  72                                        // ~590 KB of PSRAM at most
#define ART_USES   160                                       // image widgets waiting for / showing a thumbnail
#define ART_QUEUE  16
#define ART_MAX_DL (256 * 1024)

struct ArtSlot { uint32_t h; uint16_t *px; lv_image_dsc_t dsc; uint32_t used; };
struct ArtUse  { lv_obj_t *img; uint32_t h; bool shown; };
static ArtSlot *artSlots = nullptr;                           // both tables in PSRAM: internal RAM is for Wi-Fi / TLS
static ArtUse  *artUses = nullptr;
static uint32_t artClock = 0;
static volatile uint8_t artVer = 0;                          // bumped by netTask when a thumbnail lands on flash
static bool artSwap = false;                                 // sprite stores RGB565 byte-swapped (checked once)
static uint16_t artBg = 0;                                   // placeholder / transparent-PNG background

struct ArtReq { uint32_t h; char url[256]; };
static ArtReq *artQ = nullptr;                               // PSRAM ring, filled by the UI, drained by netTask
static volatile int artQHead = 0, artQTail = 0;
static uint32_t artFailed[64];                               // don't retry these this boot
static int nArtFailed = 0;

static uint32_t artHash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h ? h : 1; }
static void artPath(char *p, size_t n, uint32_t h) { snprintf(p, n, "/art/%08lx.rgb", (unsigned long)h); }
static bool artIsFailed(uint32_t h) { for (int i = 0; i < nArtFailed; i++) if (artFailed[i] == h) return true; return false; }

static void artInit() {                                      // after LittleFS.begin()
  if (!LittleFS.exists("/art")) LittleFS.mkdir("/art");
  artQ = (ArtReq *)heap_caps_calloc(ART_QUEUE, sizeof(ArtReq), MALLOC_CAP_SPIRAM);
  artSlots = (ArtSlot *)heap_caps_calloc(ART_SLOTS, sizeof(ArtSlot), MALLOC_CAP_SPIRAM);
  artUses = (ArtUse *)heap_caps_calloc(ART_USES, sizeof(ArtUse), MALLOC_CAP_SPIRAM);
  LGFX_Sprite probe;                                         // same check as the photo frame: native or swapped 565?
  static uint16_t px[4];
  probe.setColorDepth(16);
  probe.setBuffer(px, 2, 2, 16);
  probe.fillScreen(probe.color565(255, 0, 0));
  artSwap = (px[0] != 0xF800);
  artBg = (uint16_t)(((C_CARD3 >> 8) & 0xF800) | ((C_CARD3 >> 5) & 0x07E0) | ((C_CARD3 >> 3) & 0x001F));
}

// ── UI thread ──
static ArtSlot *artFindSlot(uint32_t h) {
  if (!artSlots) return nullptr;
  for (int i = 0; i < ART_SLOTS; i++) { ArtSlot &s = artSlots[i]; if (s.px && s.h == h) { s.used = ++artClock; return &s; } }
  return nullptr;
}
static bool artInUse(uint32_t h) { for (int i = 0; i < ART_USES; i++) if (artUses[i].img && artUses[i].h == h && artUses[i].shown) return true; return false; }
static ArtSlot *artLoad(uint32_t h) {                        // thumbnail file -> cache slot (evicts the oldest unused one)
  char p[32]; artPath(p, sizeof(p), h);
  File f = LittleFS.open(p, "r");
  if (!f) return nullptr;
  ArtSlot *slot = nullptr;
  for (int i = 0; i < ART_SLOTS && !slot; i++) if (!artSlots[i].px) slot = &artSlots[i];
  if (!slot) for (int i = 0; i < ART_SLOTS; i++) { ArtSlot &s = artSlots[i]; if (!artInUse(s.h) && (!slot || s.used < slot->used)) slot = &s; }
  if (!slot) { f.close(); return nullptr; }
  if (!slot->px) slot->px = (uint16_t *)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
  if (!slot->px) { f.close(); return nullptr; }
  bool ok = f.read((uint8_t *)slot->px, ART_BYTES) == ART_BYTES;
  f.close();
  if (!ok) { LittleFS.remove(p); return nullptr; }
  slot->h = h; slot->used = ++artClock;
  memset(&slot->dsc, 0, sizeof(slot->dsc));
  slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  slot->dsc.header.w = ART_PX; slot->dsc.header.h = ART_PX;
  slot->dsc.header.stride = ART_PX * 2;
  slot->dsc.data_size = ART_BYTES;
  slot->dsc.data = (const uint8_t *)slot->px;
  lv_image_cache_drop(&slot->dsc);
  return slot;
}
static void artEnqueue(uint32_t h, const char *url) {
  if (!artQ || artIsFailed(h)) return;
  for (int i = artQTail; i != artQHead; i = (i + 1) % ART_QUEUE) if (artQ[i].h == h) return;
  int next = (artQHead + 1) % ART_QUEUE;
  if (next == artQTail) return;                              // queue full: asked again next time it's shown
  artQ[artQHead].h = h;
  strlcpy(artQ[artQHead].url, url, sizeof(artQ[0].url));
  artQHead = next;
}
static void onArtImgDelete(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  for (int i = 0; i < ART_USES; i++) if (artUses[i].img == o) artUses[i].img = nullptr;
}
// a 64x64 placeholder that shows url's thumbnail now, or as soon as it has been fetched
static lv_obj_t *artImage(lv_obj_t *parent, int x, int y, const char *url) {
  lv_obj_t *box = panel(parent, x, y, ART_PX, ART_PX, C_CARD3, 14);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_clip_corner(box, true, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_t *img = lv_image_create(box);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
  if (!url || !url[0] || !artUses) return box;
  uint32_t h = artHash(url);
  ArtUse *use = nullptr;
  for (int i = 0; i < ART_USES; i++) if (!artUses[i].img) { use = &artUses[i]; break; }
  if (!use) return box;
  use->img = img; use->h = h; use->shown = false;
  lv_obj_add_event_cb(img, onArtImgDelete, LV_EVENT_DELETE, nullptr);
  ArtSlot *s = artFindSlot(h);
  if (!s) s = artLoad(h);
  if (s) { lv_image_set_src(img, &s->dsc); use->shown = true; }
  else artEnqueue(h, url);
  return box;
}
static void artUiTick() {                                    // fill in thumbnails that have arrived
  static uint8_t seen = 0;
  if (artVer == seen) return;
  seen = artVer;
  if (!artUses) return;
  for (int i = 0; i < ART_USES; i++) {
    ArtUse &u = artUses[i];
    if (!u.img || u.shown) continue;
    ArtSlot *s = artFindSlot(u.h);
    if (!s) s = artLoad(u.h);
    if (s) { lv_image_set_src(u.img, &s->dsc); u.shown = true; }
  }
}

// ── netTask ──
class BufSink : public Stream {                              // collects a download in PSRAM, refuses past the cap
public:
  uint8_t *buf; size_t n = 0, cap;
  BufSink(uint8_t *b, size_t c) : buf(b), cap(c) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t len) override {
    if (n + len > cap) return 0;
    memcpy(buf + n, b, len); n += len; return len;
  }
};
static bool imgDims(const uint8_t *d, size_t n, int &w, int &h, bool &png) {
  if (n > 24 && !memcmp(d, "\x89PNG", 4)) { png = true; w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]; h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]; return w > 0 && h > 0; }
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  png = false;
  size_t i = 2;
  while (i + 9 < n) {                                        // walk JPEG segments to the frame header
    if (d[i] != 0xFF) { i++; continue; }
    uint8_t m = d[i + 1];
    if (m == 0xC0 || m == 0xC1) { h = (d[i + 5] << 8) | d[i + 6]; w = (d[i + 7] << 8) | d[i + 8]; return w > 0 && h > 0; }
    if (m == 0xC2) return false;                             // progressive: the decoder can't do it
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    i += 2 + ((d[i + 2] << 8) | d[i + 3]);
  }
  return false;
}
static void artNetTick() {                                   // one download per call
  if (!artQ || artQTail == artQHead) return;
  ArtReq r = artQ[artQTail];
  artQTail = (artQTail + 1) % ART_QUEUE;
  char p[32]; artPath(p, sizeof(p), r.h);
  if (LittleFS.exists(p)) { artVer++; return; }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(ART_MAX_DL, MALLOC_CAP_SPIRAM);
  uint16_t *px = (uint16_t *)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
  bool ok = false;
  if (buf && px) {
    BufSink sink(buf, ART_MAX_DL);
    int w = 0, h = 0; bool png = false;
    if (httpStream(r.url, sink) == 200 && imgDims(buf, sink.n, w, h, png)) {
      static LGFX_Sprite spr;                                // netTask only
      spr.setColorDepth(16);
      spr.setBuffer(px, ART_PX, ART_PX, 16);
      for (int i = 0; i < ART_PX * ART_PX; i++) px[i] = artSwap ? (uint16_t)((artBg >> 8) | (artBg << 8)) : artBg;
      float sc = std::max((float)ART_PX / w, (float)ART_PX / h);
      ok = png ? spr.drawPng(buf, sink.n, 0, 0, ART_PX, ART_PX, 0, 0, sc, sc, lgfx::datum_t::middle_center)
               : spr.drawJpg(buf, sink.n, 0, 0, ART_PX, ART_PX, 0, 0, sc, sc, lgfx::datum_t::middle_center);
      if (ok && artSwap) for (int i = 0; i < ART_PX * ART_PX; i++) px[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
      if (ok) { File f = LittleFS.open(p, "w"); ok = f && f.write((uint8_t *)px, ART_BYTES) == ART_BYTES; if (f) f.close(); }
    }
  }
  free(buf); free(px);
  if (ok) artVer++;
  else if (nArtFailed < (int)(sizeof(artFailed) / sizeof(artFailed[0]))) artFailed[nArtFailed++] = r.h;
}
