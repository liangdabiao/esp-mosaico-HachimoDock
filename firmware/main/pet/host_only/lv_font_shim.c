/*
 * Host fmt_txt lookup engine implementing host_only/lvgl.h. Mirrors
 * lv_font_fmt_txt.c (get_glyph_dsc_id / get_kern_value /
 * lv_font_get_glyph_dsc_fmt_txt) at LVGL 9.5.
 */
#include "lvgl.h"

static uint32_t get_glyph_dsc_id(const lv_font_t *font, uint32_t letter) {
  const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *) font->dsc;
  for (uint16_t i = 0; i < fdsc->cmap_num; i++) {
    uint32_t rcp = letter - fdsc->cmaps[i].range_start;
    if (rcp >= fdsc->cmaps[i].range_length) continue;
    uint32_t glyph_id = 0;
    switch (fdsc->cmaps[i].type) {
    case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
      glyph_id = fdsc->cmaps[i].glyph_id_start + rcp;
      break;
    case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
      const uint8_t *gid_ofs_8 = (const uint8_t *) fdsc->cmaps[i].glyph_id_ofs_list;
      const uint16_t *gid_ofs_16 =
        (const uint16_t *) fdsc->cmaps[i].glyph_id_ofs_list;
      glyph_id = fdsc->cmaps[i].glyph_id_start
        + (fdsc->cmaps[i].list_length <= 255 ? gid_ofs_8[rcp] : gid_ofs_16[rcp]);
      break;
    }
    case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
    case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
      /* Sparse ranges: binary search unicode_list for the code point. */
      uint32_t lo = 0;
      uint32_t hi = fdsc->cmaps[i].list_length;
      while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (fdsc->cmaps[i].unicode_list[mid] < rcp) lo = mid + 1;
        else hi = mid;
      }
      if (lo >= fdsc->cmaps[i].list_length
          || fdsc->cmaps[i].unicode_list[lo] != rcp) {
        continue;
      }
      if (fdsc->cmaps[i].type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
        glyph_id = fdsc->cmaps[i].glyph_id_start + lo;
      } else {
        const uint8_t *gid_ofs_8 = (const uint8_t *) fdsc->cmaps[i].glyph_id_ofs_list;
        const uint16_t *gid_ofs_16 =
          (const uint16_t *) fdsc->cmaps[i].glyph_id_ofs_list;
        glyph_id = fdsc->cmaps[i].glyph_id_start
          + (fdsc->cmaps[i].list_length <= 255 ? gid_ofs_8[lo] : gid_ofs_16[lo]);
      }
      break;
    }
    default:
      continue;
    }
    if (glyph_id > 0) return glyph_id;
  }
  return 0;
}

static int8_t get_kern_value(const lv_font_t *font, uint32_t gid_left,
                             uint32_t gid_right) {
  const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *) font->dsc;
  const lv_font_fmt_txt_kern_classes_t *kern_classes =
    (const lv_font_fmt_txt_kern_classes_t *) fdsc->kern_dsc;
  uint8_t left_class = kern_classes->left_class_mapping[gid_left];
  uint8_t right_class = kern_classes->right_class_mapping[gid_right];
  if (left_class == 0 || right_class == 0) return 0;
  return kern_classes->class_pair_values
    [left_class * kern_classes->right_class_cnt + right_class];
}

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t *font,
                                   lv_font_glyph_dsc_t *dsc_out,
                                   uint32_t unicode_letter,
                                   uint32_t unicode_letter_next) {
  bool is_tab = unicode_letter == '\t';
  if (is_tab) unicode_letter = ' ';
  const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *) font->dsc;
  uint32_t gid = get_glyph_dsc_id(font, unicode_letter);
  if (!gid) return false;

  int8_t kvalue = 0;
  if (fdsc->kern_dsc) {
    uint32_t gid_next = get_glyph_dsc_id(font, unicode_letter_next);
    if (gid_next) kvalue = get_kern_value(font, gid, gid_next);
  }

  const lv_font_fmt_txt_glyph_dsc_t *gdsc = &fdsc->glyph_dsc[gid];
  int32_t kv = ((int32_t) ((int32_t) kvalue * fdsc->kern_scale) >> 4);
  uint32_t adv_w = gdsc->adv_w;
  if (is_tab) adv_w *= 2;
  adv_w += kv;
  adv_w = (adv_w + (1 << 3)) >> 4;

  dsc_out->adv_w = (uint16_t) adv_w;
  dsc_out->box_h = (uint16_t) (gdsc->box_h < 0 ? 0 : gdsc->box_h);
  dsc_out->box_w = (uint16_t) (gdsc->box_w < 0 ? 0 : gdsc->box_w);
  dsc_out->ofs_x = gdsc->ofs_x;
  dsc_out->ofs_y = gdsc->ofs_y;
  dsc_out->stride = fdsc->stride; /* generated font uses 0 (no line padding) */
  dsc_out->format = fdsc->bpp;
  dsc_out->is_placeholder = false;
  dsc_out->gid.index = gid;
  if (is_tab) dsc_out->box_w = (uint16_t) (dsc_out->box_w * 2);
  return true;
}

const void *lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t *g_dsc, void *draw_buf) {
  (void) draw_buf;
  /* Raw (uncompressed) fonts index straight into the shared bitmap blob. The
   * descriptor lookup pre-stashes the glyph base pointer in g_dsc->gid.src. */
  return g_dsc->gid.src;
}

bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                           uint32_t letter, uint32_t letter_next) {
  const lv_font_t *f = font;
  while (f) {
    if (f->get_glyph_dsc && f->get_glyph_dsc(f, dsc_out, letter, letter_next)) {
      dsc_out->resolved_font = f;
      /* Provide the raw bitmap up front; the renderer only reads it through
       * get_glyph_bitmap with req_raw_bitmap, which returns the same pointer. */
      const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *) f->dsc;
      const lv_font_fmt_txt_glyph_dsc_t *gdsc =
        &fdsc->glyph_dsc[dsc_out->gid.index];
      dsc_out->gid.src = &fdsc->glyph_bitmap[gdsc->bitmap_index];
      return true;
    }
    f = f->fallback;
  }
  return false;
}

void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t *g_dsc) {
  (void) g_dsc;
}
