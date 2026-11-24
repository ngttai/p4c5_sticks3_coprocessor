# Giai đoạn 3: UDP và soak 10 phút (SPI 26 MHz)

Ngày 2026-11-23. IDF 6.1, esp_hosted 3.1.0 (host và cp khớp), S3 240 MHz, checksum bật.
Phía PC: Python source/sink (iperf2 cài sau), PC nối WiFi 5 GHz, AP 2,4 GHz cho S3. Đo bằng iperf của IDF trên P4.

## UDP, 10 giây mỗi mức

| Tải gửi vào | TX P4->PC (PC nhận) | RX PC->P4 (P4 báo) |
| --- | --- | --- |
| 10 Mbps | 10,0 Mbps | 9,5 Mbps |
| 20 Mbps | 19,0 Mbps | 16,5 đến 17,2 Mbps |
| 30 Mbps | 20,1 Mbps (bão hòa) | 17,3 Mbps (bão hòa) |

## Soak TCP 10 phút (báo cáo mỗi 60 giây)

| Chiều | Trung bình | Min / Max theo phút | Reboot S3 | Lỗi S3 | Lỗi transport P4 |
| --- | --- | --- | --- | --- | --- |
| RX PC->P4 | 12,8 Mbps | 11,6 / 13,3 | 0 | 0 | 0 |
| TX P4->PC | 14,3 Mbps | 10,5 / 16,0 | 0 | 0 | 0 |

Phút cuối của TX giảm còn 10,5 Mbps (có thể do WiFi hoặc PC, chưa tách nguyên nhân).

## Chưa đo
- Soak 8 giờ, CPU và RAM nội của S3, so với iperf chạy thẳng trên S3.
- PC nối dây (để loại điểm nghẽn WiFi phía PC).
- Ghi nhận: StickS3 phát tiếng rè (chưa xác định nguyên nhân: ampli loa không được điều khiển hay nhiễu WiFi).
