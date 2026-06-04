$symbols = (Get-Content font_symbols_20.txt) -join ''
lv_font_conv --font SourceHanSansSC-Bold.otf --size 16 --bpp 2 --format lvgl --range 0x20-0x7E --symbols $symbols --lv-font-name lv_font_chinese_16 --lv-include lvgl.h -o ..\components\LCD\lv_font_chinese_16.c
