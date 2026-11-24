# phone_s3

Tiếng Việt | [English](README.en.md)

Phone (Brookesia, LVGL) cho KSDIY P4C5 dùng M5StickS3 làm co-processor: WiFi 2,4 GHz qua SPI (esp_hosted 3.1.0) và âm thanh qua link I2S 16 kHz mono. Bản tinh giản từ `esp32p4c5_phone`, chỉ giữ các app để thử WiFi, mic và âm thanh.

Tổng quan, kết quả và đấu dây: [../README.md](../README.md), [../docs/](../docs/).

## Ứng dụng
- **Cài đặt:** WiFi, tốc độ mạng (cần `tools/speed_server.py` trên PC cùng mạng), tốc độ bộ nhớ, âm lượng, ngôn ngữ, giờ, khóa màn hình, nguồn, trang Giới thiệu (thông tin S3 và bộ đếm link I2S).
- **Trợ lý:** hội thoại giọng nói (esp_xiaozhi), wake word "Hi ESP" trên P4, mic đã qua AEC và NS trên S3.
- **Nhạc, Radio:** phát qua loa của StickS3 (16 kHz mono).

## Khác với bản gốc
- WiFi qua esp_hosted 3.1.0 SPI 26 MHz tới S3 (chân JP1: CLK 26, MOSI 27, MISO 28, CS 29, handshake 30, data ready 45, reset 50). C5 giữ ở reset (GPIO54 thấp). Không BLE, không 5 GHz.
- BSP cục bộ `components/esp32p4c5_bsp`: `esp32p4c5_wifi.c` (kết nối S3) và `esp32p4c5_audio.c` (codec ảo trên link I2S, software volume, hạ mẫu về 16 kHz).
- Assistant không chạy AFE trên P4 (`CONFIG_BSP_AUDIO_S3_LINK=y`).
- Đã gỡ: 4G, camera, USB camera, video, thư viện ảnh, quản lý file, thời tiết, màn hình thứ hai, máy tính, game 2048, level, demo Squareline.

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32p4 && idf.py build
```
Cần nạp `../cp_s3` lên StickS3 trước. Nạp hoặc monitor P4 cũng reset S3 (qua GPIO50).
