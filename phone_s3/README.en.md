# phone_s3

[Tiếng Việt](README.md) | English

A phone UI (Brookesia, LVGL) for the KSDIY P4C5 that uses an M5StickS3 as its co-processor: 2.4 GHz WiFi over SPI (esp_hosted 3.1.0) and audio over a 16 kHz mono I2S link. It is a slimmed-down copy of `esp32p4c5_phone`, keeping only the apps that exercise WiFi, microphone and audio.

Overview, results and wiring: [../README.en.md](../README.en.md), [../docs/en/](../docs/en/).

## Apps
- **Settings:** WiFi, network speed (needs `tools/speed_server.py` on a PC on the same network), memory speed, volume, language, time, screen lock, power, About (S3 info and I2S link counters).
- **Assistant:** voice conversation (esp_xiaozhi), "Hi ESP" wake word on the P4, microphone already cleaned by AEC and NS on the S3.
- **Music, Radio:** playback through the StickS3 speaker (16 kHz mono).

## Differences from the original
- WiFi through esp_hosted 3.1.0 SPI at 26 MHz to the S3 (JP1 pins: CLK 26, MOSI 27, MISO 28, CS 29, handshake 30, data ready 45, reset 50). The C5 is held in reset (GPIO54 low). No BLE, no 5 GHz.
- Local BSP `components/esp32p4c5_bsp`: `esp32p4c5_wifi.c` (connects to the S3) and `esp32p4c5_audio.c` (virtual codec on the I2S link, software volume, resampling to 16 kHz).
- The assistant does not run the AFE on the P4 (`CONFIG_BSP_AUDIO_S3_LINK=y`).
- Removed: 4G, camera, USB camera, video, gallery, file manager, weather, second screen, calculator, 2048 game, level, Squareline demo.

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32p4 && idf.py build
```
Flash `../cp_s3` to the StickS3 first. Flashing or monitoring the P4 also resets the S3 (through GPIO50).
