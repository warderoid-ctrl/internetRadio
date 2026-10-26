// LVGL heap in PSRAM (lv_conf.h: LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM).
// The UI is thousands of small objects; with plain malloc they all land in internal RAM and Wi-Fi runs out.
#include "lvgl.h"
#include <esp_heap_caps.h>

#define LV_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, LV_HEAP_CAPS);
  return p ? p : malloc(size);                     // fall back to internal RAM if PSRAM is missing / full
}
void *lv_realloc_core(void *p, size_t new_size) {
  void *q = heap_caps_realloc(p, new_size, LV_HEAP_CAPS);
  return q ? q : realloc(p, new_size);
}
void lv_free_core(void *p) { free(p); }            // free() handles both heaps

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
