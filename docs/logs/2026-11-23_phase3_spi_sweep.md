# Giai đoạn 3: quét xung SPI (hosted SPI full-duplex, StickS3 làm co-processor)

Ngày 2026-11-23. IDF 6.1, esp_hosted 3.1.0 (host và cp khớp), S3 240 MHz, checksum bật, dây jumper ngắn.
Phía PC: Python TCP source/sink (không cài iperf), PC nối WiFi 5 GHz cùng mạng 192.168.1.0/24, AP 2,4 GHz cho S3.
P4 chạy `iperf -c` / `iperf -s` (iperf của IDF), 10 giây, TCP, 1 lần đo mỗi mức. Kết quả trung bình P4 báo cáo.

| SPI (MHz) | TCP TX P4->PC (Mbps) | TCP RX PC->P4 (Mbps) | Ghi chú |
| --- | --- | --- | --- |
| 10 | 7,4 | 7,2 (ổn định) | |
| 20 | 12,6 | 8,5 đến 10,8 | |
| 26 | 12,9 | 11,6 (ổn định 14) | tốt nhất, ổn định |
| 40 | 8,2 | 8,3 | dao động mạnh (2 đến 14), không ổn định |

Chưa đo: UDP, 60 giây, 10 phút, so với iperf chạy thẳng trên S3, CPU S3 và RAM nội.
Phía PC cũng là WiFi nên có thể là điểm nghẽn, cần xác nhận bằng PC cắm dây.
Lỗi đã gặp và sửa: `pserial_task` tràn stack trên S3 khi `sta_connect` (stack 4096 -> 8192).
