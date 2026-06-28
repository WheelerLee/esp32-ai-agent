#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 从二进制字库表加载按需读取的 LVGL 字体。
const lv_font_t *lazy_font_load(const char *path, const lv_font_t *fallback);

// 释放当前懒加载字体文件和缓存，恢复备用字体链路。
void lazy_font_unload(void);

#ifdef __cplusplus
}
#endif
