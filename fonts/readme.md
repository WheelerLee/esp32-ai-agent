当前工程保留两类中文字体：

1. `components/LCD/lv_font_chinese_16.c`

   这是编译进 app 的小型兜底字体，只在外部字体加载失败或缺字时使用。

   ```
   lv_font_conv --font fonts\SourceHanSansSC-Bold.otf --size 16 --bpp 2 --format lvgl --range '0x20-0x7E' --symbols '已连接网络网关正在未请扫描并选择发现隐藏横屏显示智能助手音量播放返回输入密码刷新重新扫描' --no-kerning --lv-font-name lv_font_chinese_16 --lv-include lvgl.h -o components\LCD\lv_font_chinese_16.c
   ```

2. `fonts/llm_text_14_lazy.bin`

   这是大模型回复使用的 14px、bpp1 常用字字体。它不是 LVGL 原生 bin，而是工程
   自定义的 lazy 字体格式：启动时只读索引，字形 bitmap 在绘制时按字读取。

   ```
   sh tools/generate_lazy_font.sh --fontsize 14 --bbp 1
   ```

   Windows PowerShell 下也可以运行：

   ```
   .\tools\generate_lazy_font.ps1 -FontSize 14 -bbp 1
   ```

   脚本会生成 `fonts/llm_text_14_lazy.bin`，并复制到
   `font_active/llm_text_14_lazy.bin`。顶层 `CMakeLists.txt` 会把 `font_active/`
   打包为独立的 `font` SPIFFS 分区镜像，LCD 初始化时挂载到 `/font` 并打开
   `/font/llm_text_14_lazy.bin`。
