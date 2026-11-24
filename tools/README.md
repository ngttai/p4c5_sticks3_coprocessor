# tools

Script đo trên PC dùng để tạo các số trong [../docs/logs/](../docs/logs/). Cần Python 3 với `pyserial` (`pip install pyserial`).

## Biến môi trường chung

| Biến | Ý nghĩa |
| --- | --- |
| `P4_PORT` | Cổng serial của P4C5, ví dụ `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mã>-if00` |
| `S3_PORT` | Cổng serial của StickS3 |
| `PC_IP` | IP của PC cùng mạng WiFi với S3 (cho chiều TX P4 sang PC) |
| `WS`, `WP` | Tên và mật khẩu WiFi 2,4 GHz. Chỉ đọc từ môi trường, không lưu vào file; log tự che hai giá trị này |
| `OUT_DIR` | (tuỳ chọn) nơi ghi kết quả của `soak1h.py`, mặc định thư mục tạm |

Các script mở cổng với `dtr` và `rts` tắt để **không reset board**. Mở cổng bằng `cat`, `idf.py monitor` hay công cụ khác thường reset P4, và qua GPIO50 reset luôn S3.

## Script

| Script | Việc | Cần nạp |
| --- | --- | --- |
| `soak1h.py [giây]` | TCP RX rồi TX (mỗi nửa 1800 s mặc định) cùng link I2S và AFE; tóm tắt throughput, RAM, CPU, trễ AFE, lỗi | `cp_s3` (AFE bật) và `host_p4` |
| `combo.py [giây] [rx\|tx\|both]` | iperf TCP cùng lúc `linktest ... tone`; in throughput và log S3 | như trên |
| `lt.py "<lệnh>" ...` | Chạy lệnh console của P4 (ví dụ `linktest 20 tone`), in kết quả và dòng `audio_fe` của S3 | như trên |
| `lat.py linklat [n]` | Đo độ trễ link bằng xung click. Đo điện cần `cp_s3` bật `AUDIO_FE_LINK_ECHO` | như trên |
| `cap_dt.py <file>` | Ghi log serial S3 đến khi gặp `ALL DONE` (bài double-talk của `afe_s3`) | `afe_s3` |
| `listen.py <cổng> <giây>` | Đọc serial không reset board | bất kỳ |

Ví dụ:

```bash
export P4_PORT=/dev/serial/by-id/...P4...  S3_PORT=/dev/serial/by-id/...S3...
export PC_IP=192.168.1.10  WS='ten-wifi'  WP='mat-khau'
python3 tools/combo.py 60 both
```

Server test mạng cho màn hình Cài đặt của phone: [../phone_s3/tools/speed_server.py](../phone_s3/tools/speed_server.py).

## Lưu ý

- `combo.py` và `soak1h.py` đăng nhập WiFi qua lệnh `sta_connect` trên console P4, rồi đo bằng lệnh `iperf` của IDF. Hai script giả định bản `host_p4` với các lệnh `linktest` và `linklat`.
- Số throughput nhiễu khoảng 2 Mbps vì PC cũng dùng WiFi; so sánh các cấu hình cần đo nhiều lần hoặc nối PC bằng dây.
