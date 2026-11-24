# cp_s3

Firmware cho M5StickS3 (ESP32-S3): co-processor esp_hosted 3.1.0 (WiFi 2,4 GHz qua SPI) cộng `audio_fe` (ES8311, AFE FD HIGH, link I2S tới P4).
Dựng từ example `wifi/iperf` cp của esp_hosted; README gốc của example: [README_upstream.md](README_upstream.md).

## Chân
SPI tới P4: CLK 5, MOSI 6, MISO 7, CS 8, handshake 1, data ready 2, reset 9 (`CONFIG_EH_TRANSPORT_CP_SPI_GPIO_RESET=9`). I2S link: BCLK 43, WS 44, mic ra 4, playback vào 3. Đầy đủ: [../docs/WIRING.md](../docs/WIRING.md).

## Cấu hình (menuconfig, mục "StickS3 audio_fe")
| Tùy chọn | Ý nghĩa | Mặc định |
| --- | --- | --- |
| `AUDIO_FE_ENABLE` | Chạy audio_fe (ES8311 + AFE + link) | y |
| `AUDIO_FE_BYPASS` | Gửi mic thô lên link, không qua AFE (kiểm dây trước) | n |
| `AUDIO_FE_AMP_ALWAYS_ON` | Ampli luôn bật (chỉ để đo trễ) | n |
| `AUDIO_FE_LINK_ECHO` | Trả playback của P4 ngược lại (đo trễ điện của link) | n |
| `AUDIO_FE_FD_HIGH` | AFE FD HIGH_PERF (ngược lại FD LOW_COST) | y |

Console dùng USB-Serial/JTAG (UART0 G43 và G44 dành cho link I2S). Giao thức link: [../docs/I2S_LINK.md](../docs/I2S_LINK.md).

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3 && idf.py build && idf.py -p <cổng S3> flash
```
