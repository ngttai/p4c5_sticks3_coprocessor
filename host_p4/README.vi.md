# host_p4

[English](README.md) | Tiếng Việt

Firmware thử trên KSDIY P4C5 (ESP32-P4): host esp_hosted 3.1.0 qua SPI tới StickS3, console `iperf` và hai lệnh kiểm tra link I2S. Dùng để kiểm dây và đo trước khi chạy `../phone_s3`.
Dựng từ example `wifi/iperf` host; README gốc: [README_upstream.md](README_upstream.md). Khác example: bỏ `esp-extconn` (nó điều khiển C5 qua SDIO) và kéo GPIO54 (EN của C5) xuống thấp.

## Lệnh console (USB-Serial/JTAG)
| Lệnh | Việc |
| --- | --- |
| `sta_connect "<ssid>" <mật khẩu>` | Nối WiFi 2,4 GHz |
| `iperf -s -i 10 -t 60` / `iperf -c <ip PC> -i 10 -t 60` | Đo TCP RX / TX (PC chạy nguồn hoặc sink, xem `../tools/`) |
| `linktest [giây] [tone]` | Đếm khung I2S, báo lỗi bộ đếm và timeout; `tone` gửi tone 1 kHz xuống S3 |
| `linklat [n]` | Đo độ trễ link bằng xung click |

Xung SPI: `CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ` trong `sdkconfig.defaults` (10, 20, 26 hoặc 40; 26 ổn định nhất).

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32p4 && idf.py build && idf.py -p <cổng P4> flash
```
