# 中文字体

固件字体 `homehub_font_22` 为 Noto Sans SC 的 22 像素、4bpp、未压缩 LVGL 位图子集，包含产品短标签和 ASCII。

- 官方源字体：https://github.com/google/fonts/tree/main/ofl/notosanssc
- 子集服务：https://fonts.googleapis.com/css2?family=Noto+Sans+SC:wght@400 （带 symbols.txt 的 text 参数）
- 下载的子集：homehub-subset.woff2；字体声明：subset.css。
- 转换器：官方 lvgl/lv_font_conv，npm 版本1.5.3。
- 许可证：同目录 OFL.txt。保留原字体版权声明，固件不附带 Windows 商业字体。
- 生成结果：firmware/src/homehub_font_22.c。

所有字体文件用于本项目界面，不承诺任意自定义姓名/长AI文本的所有汉字都能显示。增加显示文字时需同步扩充 symbols.txt 并重新生成子集。
