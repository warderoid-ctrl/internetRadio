#pragma once
// Rename the playing station (and its favourite / recent card): full-screen field + keyboard on the top layer.
// Part of the sketch: included from internetRadio.ino.

static lv_obj_t *renOv, *renTa;
static void renClose() { lv_obj_add_flag(renOv, LV_OBJ_FLAG_HIDDEN); }
static void onRenOk(lv_event_t *) { stationRename(lv_textarea_get_text(renTa)); renClose(); }
static void onRenCancel(lv_event_t *) { renClose(); }
static void onRenKb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) onRenOk(e);
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) renClose();
}
static void onRenameBtn(lv_event_t *) {
  if (!renOv) {
    renOv = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(renOv);
    lv_obj_set_size(renOv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(renOv, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(renOv, LV_OPA_COVER, 0);
    lv_obj_add_flag(renOv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = mkLabel(renOv, "Rename station", F_T26, C_TEXT);
    lv_obj_set_pos(t, T_PAD_X, 22);
    renTa = textField(renOv, 20, 84, 520, 64, nullptr, nullptr);
    lv_textarea_set_max_length(renTa, sizeof(curSt.name) - 1);
    textBtn(renOv, 552, 84, 112, 64, "Cancel", F_M, T->surfaceHi, onRenCancel);
    textBtn(renOv, 674, 84, 106, 64, "Save", F_M, T->primary, onRenOk);
    keyboardFor(renOv, SCREEN_W, 270, renTa, onRenKb);
  }
  lv_textarea_set_text(renTa, curSt.name);
  lv_obj_remove_flag(renOv, LV_OBJ_FLAG_HIDDEN);
}
