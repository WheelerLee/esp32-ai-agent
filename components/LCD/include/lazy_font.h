#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

const lv_font_t *lazy_font_load(const char *path, const lv_font_t *fallback);
void lazy_font_unload(void);

#ifdef __cplusplus
}
#endif
