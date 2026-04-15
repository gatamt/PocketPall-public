# Reference hardware

PocketPall targets the Waveshare ESP32-S3 AMOLED development module with a 4-inch touchscreen, 16 MB flash, and 8 MB octal PSRAM.

Reference product listing (for convenience — substitute an equivalent board if this SKU is unavailable):

- [Waveshare ESP32-S3 4-inch AMOLED development module](https://www.amazon.de/dp/B0DSVK5576)

The firmware drives the display via QSPI (Sitronix SH8601 panel), the touch panel via I²C (FocalTech FT3168), and expects the standard peripheral set listed in the main `README.md` hardware table (ES8311 audio codec, QMI8658 IMU, AXP2101 PMIC, TCA9554 I/O expander). If you use a different carrier board, expect to adjust the I²C addresses in `Personlig_Ai/main/app_config.h` and the QSPI pin map in `display_hal.c`.
