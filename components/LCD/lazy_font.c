#include "lazy_font.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lazy_font";

#define LAZY_FONT_MAGIC "LZYFNT1"
#define LAZY_FONT_CACHE_COUNT 64

typedef struct {
  uint8_t magic[8];
  uint16_t version;
  uint16_t font_px;
  uint16_t line_height;
  uint16_t base_line;
  uint16_t bpp;
  uint16_t reserved;
  uint32_t glyph_count;
  uint32_t index_offset;
  uint32_t bitmap_offset;
} lazy_font_header_t;

typedef struct {
  uint32_t code;
  uint32_t bitmap_offset;
  uint16_t bitmap_size;
  uint16_t adv_w;
  uint16_t box_w;
  uint16_t box_h;
  int16_t ofs_x;
  int16_t ofs_y;
} lazy_font_entry_t;

typedef struct {
  uint32_t code;
  bool valid;
  uint8_t *bitmap;
} lazy_font_cache_entry_t;

typedef struct {
  lv_font_t font;
  FILE *file;
  lazy_font_entry_t *entries;
  uint32_t glyph_count;
  uint32_t bitmap_offset;
  uint16_t max_bitmap_size;
  uint8_t next_cache_slot;
  lazy_font_cache_entry_t cache[LAZY_FONT_CACHE_COUNT];
} lazy_font_state_t;

static lazy_font_state_t s_lazy_font;

static const lazy_font_entry_t *find_entry(const lazy_font_state_t *state, uint32_t code)
{
  uint32_t left = 0;
  uint32_t right = state->glyph_count;

  while (left < right) {
    uint32_t mid = left + (right - left) / 2;
    const lazy_font_entry_t *entry = &state->entries[mid];
    if (entry->code == code) {
      return entry;
    }
    if (entry->code < code) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  return NULL;
}

static bool lazy_get_glyph_dsc(const lv_font_t *font,
                               lv_font_glyph_dsc_t *dsc_out,
                               uint32_t unicode_letter,
                               uint32_t unicode_letter_next)
{
  (void)unicode_letter_next;

  if (font == NULL || dsc_out == NULL || font->dsc == NULL) {
    return false;
  }

  const lazy_font_state_t *state = (const lazy_font_state_t *)font->dsc;
  const lazy_font_entry_t *entry = find_entry(state, unicode_letter);
  if (entry == NULL) {
    return false;
  }

  memset(dsc_out, 0, sizeof(*dsc_out));
  dsc_out->resolved_font = font;
  dsc_out->adv_w = entry->adv_w;
  dsc_out->box_w = entry->box_w;
  dsc_out->box_h = entry->box_h;
  dsc_out->ofs_x = entry->ofs_x;
  dsc_out->ofs_y = entry->ofs_y;
  dsc_out->bpp = 1;
  return true;
}

static const uint8_t *lazy_get_glyph_bitmap(const lv_font_t *font, uint32_t unicode_letter)
{
  if (font == NULL || font->dsc == NULL) {
    return NULL;
  }

  lazy_font_state_t *state = (lazy_font_state_t *)font->dsc;
  const lazy_font_entry_t *entry = find_entry(state, unicode_letter);
  if (entry == NULL || entry->bitmap_size == 0 || state->file == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < LAZY_FONT_CACHE_COUNT; ++i) {
    if (state->cache[i].valid && state->cache[i].code == unicode_letter) {
      return state->cache[i].bitmap;
    }
  }

  lazy_font_cache_entry_t *slot = &state->cache[state->next_cache_slot];
  state->next_cache_slot = (state->next_cache_slot + 1) % LAZY_FONT_CACHE_COUNT;

  if (fseek(state->file, state->bitmap_offset + entry->bitmap_offset, SEEK_SET) != 0) {
    slot->valid = false;
    return NULL;
  }

  size_t read_count = fread(slot->bitmap, 1, entry->bitmap_size, state->file);
  if (read_count != entry->bitmap_size) {
    slot->valid = false;
    return NULL;
  }

  slot->code = unicode_letter;
  slot->valid = true;
  return slot->bitmap;
}

void lazy_font_unload(void)
{
  if (s_lazy_font.file != NULL) {
    fclose(s_lazy_font.file);
  }

  free(s_lazy_font.entries);
  for (size_t i = 0; i < LAZY_FONT_CACHE_COUNT; ++i) {
    free(s_lazy_font.cache[i].bitmap);
  }

  memset(&s_lazy_font, 0, sizeof(s_lazy_font));
}

const lv_font_t *lazy_font_load(const char *path, const lv_font_t *fallback)
{
  if (path == NULL) {
    return NULL;
  }

  lazy_font_unload();

  int64_t start_us = esp_timer_get_time();
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    ESP_LOGW(TAG, "open lazy font failed: %s", path);
    return NULL;
  }

  lazy_font_header_t header;
  if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
    ESP_LOGW(TAG, "read lazy font header failed: %s", path);
    fclose(file);
    return NULL;
  }

  if (memcmp(header.magic, LAZY_FONT_MAGIC, strlen(LAZY_FONT_MAGIC)) != 0 ||
      header.version != 1 ||
      header.bpp != 1 ||
      header.glyph_count == 0 ||
      header.index_offset < sizeof(header) ||
      header.bitmap_offset <= header.index_offset) {
    ESP_LOGW(TAG, "unsupported lazy font format: %s", path);
    fclose(file);
    return NULL;
  }

  lazy_font_entry_t *entries = calloc(header.glyph_count, sizeof(lazy_font_entry_t));
  if (entries == NULL) {
    ESP_LOGW(TAG, "allocate lazy font index failed: %lu glyphs", (unsigned long)header.glyph_count);
    fclose(file);
    return NULL;
  }

  if (fseek(file, header.index_offset, SEEK_SET) != 0 ||
      fread(entries, sizeof(lazy_font_entry_t), header.glyph_count, file) != header.glyph_count) {
    ESP_LOGW(TAG, "read lazy font index failed: %s", path);
    free(entries);
    fclose(file);
    return NULL;
  }

  uint16_t max_bitmap_size = 0;
  for (uint32_t i = 0; i < header.glyph_count; ++i) {
    if (entries[i].bitmap_size > max_bitmap_size) {
      max_bitmap_size = entries[i].bitmap_size;
    }
  }

  if (max_bitmap_size == 0) {
    ESP_LOGW(TAG, "lazy font contains no drawable glyphs: %s", path);
    free(entries);
    fclose(file);
    return NULL;
  }

  for (size_t i = 0; i < LAZY_FONT_CACHE_COUNT; ++i) {
    s_lazy_font.cache[i].bitmap = malloc(max_bitmap_size);
    if (s_lazy_font.cache[i].bitmap == NULL) {
      ESP_LOGW(TAG, "allocate lazy font bitmap cache failed: %u bytes", max_bitmap_size);
      free(entries);
      fclose(file);
      lazy_font_unload();
      return NULL;
    }
  }

  s_lazy_font.file = file;
  s_lazy_font.entries = entries;
  s_lazy_font.glyph_count = header.glyph_count;
  s_lazy_font.bitmap_offset = header.bitmap_offset;
  s_lazy_font.max_bitmap_size = max_bitmap_size;
  s_lazy_font.font.get_glyph_dsc = lazy_get_glyph_dsc;
  s_lazy_font.font.get_glyph_bitmap = lazy_get_glyph_bitmap;
  s_lazy_font.font.line_height = header.line_height;
  s_lazy_font.font.base_line = header.base_line;
  s_lazy_font.font.subpx = LV_FONT_SUBPX_NONE;
  s_lazy_font.font.underline_position = -1;
  s_lazy_font.font.underline_thickness = 1;
  s_lazy_font.font.dsc = &s_lazy_font;
  s_lazy_font.font.fallback = fallback;

  ESP_LOGI(TAG,
           "loaded lazy font index: %s, %lu glyphs, cache %u x %u bytes, in %lld ms",
           path,
           (unsigned long)header.glyph_count,
           LAZY_FONT_CACHE_COUNT,
           max_bitmap_size,
           (long long)((esp_timer_get_time() - start_us) / 1000));
  return &s_lazy_font.font;
}
