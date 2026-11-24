# afe_s3

[English](README.md) | Tiếng Việt

Bộ thử AFE độc lập trên StickS3 (không có WiFi và link I2S), dùng để chọn cấu hình AFE ở giai đoạn 4. Kết quả: [../docs/logs/vi/2026-11-23_phase4_afe_standalone.md](../docs/logs/vi/2026-11-23_phase4_afe_standalone.md).

Chọn bài thử bằng `TEST_MODE` trong `main/main.c`:
- `1`: tone 1 kHz, lần lượt SR LOW, FD LOW, FD HIGH, VC LOW và đối chứng không AEC.
- `2`: giọng tổng hợp (flite) lặp lại, cùng năm cấu hình đó.
- `3`: double-talk. Mỗi vòng chờ nhấn KEY1, vòng 20 s: bip ba lần, loa phát giọng từ giây 3, bip đơn giây 8 là bắt đầu nói, bip đơn giây 15 là dừng; in `ALL DONE` khi xong. Ghi log bằng `../tools/cap_dt.py`.

Build: `idf.py set-target esp32s3 && idf.py build && idf.py -p <cổng S3> flash`.
