$symbols = (Get-Content font_symbols_20.txt) -join ''
lv_font_conv --font AlibabaPuHuiTi-3-75-SemiBold.ttf --size 20 --bpp 2 --format lvgl --range 0x20-0x7E --symbols $symbols --lv-font-name lv_font_chinese_20 --lv-include lvgl.h -o lv_font_chinese_20.c
