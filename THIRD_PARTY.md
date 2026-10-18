# Third-party components

This project is MIT licensed. Dependencies retain their original licenses.

| Component | Version / commit | License | Location / origin |
|---|---|---|---|
| ESP-IDF | 5.5.4 / 735507283d5b2f9fb363a1901172dbd9e847945d | Apache-2.0 and component licenses | https://github.com/espressif/esp-idf |
| M5GFX | 0.2.19 / 53a7184601f3667b030ba141c58b87ce2acfaa2a | MIT and included font licenses | firmware/components/M5GFX |
| M5PM1 | 1.0.6 / 8f1f1a60b3040088cd0ca2b9eb9d024ccc4d907f | MIT | firmware/components/M5PM1 |
| M5IOE1 | 1.0.8 / 37db04861687858b6748f5dbd1a84439a6635c46 | MIT | firmware/components/M5IOE1 |
| LVGL | 9.5.0 / 85aa60d18b3d5e5588d7b247abf90198f07c8a63 | MIT | firmware/components/lvgl |
| jsQR | 1.4.0 | Apache-2.0 | web/vendor/jsQR.js; web/vendor/jsQR.LICENSE |
| Noto Sans | subset generated at 20 px, 4 bpp | SIL OFL 1.1 | firmware/main/font_ru.c; firmware/assets/NotoSans.LICENSE |

The AMOLED CO5300 initialization, pin assignments, CST820 touch packet decoding and power sequencing in `firmware/main/board.cpp` are adapted from [M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo), Copyright (c) 2026 M5Stack Technology CO LTD, MIT. The original MIT notice is reproduced in `firmware/assets/M5Stack.LICENSE`. No networking, audio, demo apps, or original RTC auto-correction code is carried over.

M5PM1/M5IOE1 CMake integration receives one small patch: M5GFX is declared as an optional public dependency, so native I2C class layout is identical in libraries and consumers. The original I2C conflict guard is preserved; no `i2c_bus` initialization or legacy I2C driver is used by the application.

`font_ru.c` was generated with `lv_font_conv@1.5.3` (MIT) from Google Fonts' `ofl/notosans/NotoSans[wdth,wght].ttf`, SHA-256 `bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`. It contains ASCII, U+0400–U+045F and U+2013/U+2014/U+2026 glyphs. Regeneration command:

```text
lv_font_conv --font NotoSans.ttf --size 20 --bpp 4 --format lvgl --lv-font-name font_ru --range 0x20-0x7e,0x400-0x45f,0x2013-0x2014,0x2026 --no-compress --no-prefilter -o firmware/main/font_ru.c
```

jsQR bundled artifact SHA-256: `bc40c8a15196236b2314db0856f72ca0b49980cd5413b8c852a7349f5fee0859`.
