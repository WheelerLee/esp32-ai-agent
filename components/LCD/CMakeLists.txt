set(srcs
            lcd.c
            lv_font_chinese_16.c)

set(include_dirs
            include)

set(requires
            driver
            esp_lcd
            esp_lcd_ili9341
            esp_timer
            lvgl)

idf_component_register(SRCS ${srcs}
                      INCLUDE_DIRS ${include_dirs}
                      REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
