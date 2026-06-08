# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lesson1)

spiffs_create_partition_image(font font_active FLASH_IN_PROJECT
                              DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/font_active/llm_text_14_lazy.bin)

# Work around a Windows ESP32-S3 toolchain ICE seen while compiling
# esp_lcd/rgb/esp_lcd_panel_rgb.c in rgb_panel_draw_bitmap. This project uses
# SPI ILI9341, but esp_lcd still compiles the RGB source as part of the
# component, so lower only esp_lcd's optimization level.
idf_component_get_property(esp_lcd_lib esp_lcd COMPONENT_LIB)
target_compile_options(${esp_lcd_lib} PRIVATE -O0)

# micro-mp3's bundled OpenCore decoder triggers GCC's maybe-uninitialized
# analysis under ESP-IDF's project-wide -Werror settings.
idf_component_get_property(micro_mp3_lib esphome__micro-mp3 COMPONENT_LIB)
target_compile_options(${micro_mp3_lib} PRIVATE -Wno-error=maybe-uninitialized)
