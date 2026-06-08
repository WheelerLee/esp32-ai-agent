set(srcs
            lcd.c
            lazy_font.c
            lv_font_chinese_16.c)

set(include_dirs
            include)

set(requires
            AUDIO
            driver
            esp_lcd
            esp_lcd_ili9341
            esp_lcd_touch
            esp_lcd_touch_xpt2046
            esp_timer
            esp_system
            lvgl
            spiffs
            WIFI)

idf_component_register(SRCS ${srcs}
                      INCLUDE_DIRS ${include_dirs}
                      REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
