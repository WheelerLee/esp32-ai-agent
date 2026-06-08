当前工程使用 LVGL 8.3 的 C 字体，直接编译进 app：

```
lv_font_conv --font fonts\SourceHanSansSC-Bold.otf --size 16 --bpp 2 --format lvgl --range '0x20-0x7E' --symbols '已连接网络网关正在未请扫描并选择发现隐藏横屏显示智能助手音量播放返回输入密码刷新重新扫描' --no-kerning --lv-font-name lv_font_chinese_16 --lv-include lvgl.h -o components\LCD\lv_font_chinese_16.c
```

这份 C 字体只作为字体分区加载失败时的兜底。大模型回复使用 14px、bpp1 的二进制字体文件：

```
lv_font_conv --font fonts\puhui.ttf --size 14 --bpp 1 --format bin --range '0x20-0x7E' --symbols <常用字.txt 中的常用汉字和标点> --no-kerning -o fonts\llm_text_14.bin
```

生成后把它复制到 `font_active/llm_text_14.bin`。构建时顶层
`CMakeLists.txt` 会把 `font_active/` 打包为 `font` SPIFFS 分区镜像。

LVGL 的二进制字体示例常用 `.fnt` 后缀，本质上就是 `lv_font_conv --format bin`
的产物。当前工程会把 `font` SPIFFS 分区挂载到 `/font`，注册为 LVGL 的 `F:`
驱动，再调用 `lv_font_load("F:/llm_text_14.bin")`。
