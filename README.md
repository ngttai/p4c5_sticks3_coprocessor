# ksdiy_p4_s3

M5StickS3 (ESP32-S3) làm co-processor cho board KSDIY P4C5: **WiFi 2,4 GHz qua SPI (ESP-Hosted 3.1.0)** và **mic sạch (AEC, NS, VAD) qua I2S**. Kèm bản phone chạy trên cấu hình đó.

## Cấu trúc

| Thư mục | Nội dung |
| --- | --- |
| `cp_s3/` | Firmware S3: esp_hosted co-processor + `audio_fe` (ES8311, AFE FD HIGH, link I2S) |
| `host_p4/` | Firmware thử trên P4: iperf qua hosted, lệnh `linktest` và `linklat` cho link I2S |
| `afe_s3/` | AFE độc lập trên StickS3 (tone, giọng TTS, double-talk) dùng để chọn cấu hình AFE |
| `phone_s3/` | Phone (Brookesia, LVGL) bản tinh giản: Cài đặt (WiFi, test mạng, âm lượng), Nhạc, Radio, Trợ lý |
| `hw_specs/` | `hw_matrix.md` của P4C5, schematic và pin map StickS3 |
| `tools/` | Script đo trên PC (soak, throughput, link, độ trễ) |
| `docs/` | Kết quả, kiến trúc, sơ đồ đấu dây, giao thức link, hướng dẫn port, log đo |

## Đọc theo thứ tự

1. [docs/RESULTS.md](docs/RESULTS.md): được gì, mất gì, hạn chế, chưa làm.
2. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): sơ đồ khối và luồng dữ liệu.
3. [docs/WIRING.md](docs/WIRING.md): sơ đồ và bảng đấu dây.
4. [docs/I2S_LINK.md](docs/I2S_LINK.md): giao thức link I2S.
5. [docs/PORTING.md](docs/PORTING.md): port sang board P4 khác.
6. [docs/logs/](docs/logs/): số đo từng giai đoạn; script tạo ra chúng ở [tools/](tools/).

## Build

Dùng ESP-IDF 6.1. Mỗi project là một ESP-IDF project độc lập:

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
cd cp_s3    && idf.py set-target esp32s3 && idf.py build   # S3
cd host_p4  && idf.py set-target esp32p4 && idf.py build   # P4, bản thử
cd phone_s3 && idf.py set-target esp32p4 && idf.py build   # P4, bản phone
```

`sdkconfig` được sinh từ `sdkconfig.defaults*` và không nằm trong git. Nạp: `idf.py -p <cổng> flash` (S3 và P4 là hai cổng USB-Serial/JTAG khác nhau; mở cổng serial reset board).

## Ghi chú

- Cần esp_hosted 3.1.0 cho cả hai phía; không dùng chung với slave C5 stock (2.12.13).
- `phone_s3` có BSP cục bộ `components/esp32p4c5_bsp`, khác bản GitHub gốc ở WiFi (SPI tới S3) và âm thanh (link I2S).
- Bản đầy đủ của phone (trước khi tinh giản và gỡ 4G) không nằm trong repo này.
