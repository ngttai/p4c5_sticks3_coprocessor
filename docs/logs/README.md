# Log đo

Tiếng Việt | [English](README.en.md)

Đọc theo thứ tự thời gian; mỗi file giữ số liệu ở thời điểm đo, kể cả các cấu hình sau đó đã bị thay.

| File | Nội dung | Ghi chú |
| --- | --- | --- |
| `2026-11-23_phase3_spi_sweep.md` | Quét xung SPI 10, 20, 26, 40 MHz | Chọn 26 MHz |
| `2026-11-23_phase3_udp_soak.md` | UDP và soak 10 phút | Tiếng rè của loa ở đây sau đó đã tìm ra nguyên nhân (ampli) |
| `2026-11-23_phase4_afe_standalone.md` | AFE độc lập: CPU, AEC với tone, giọng, double-talk | Chọn FD HIGH |
| `2026-11-23_phase5_integration.md` | Gộp hosted, AFE và link; độ trễ, CPU, sửa RAM nội | Các biến thể RAM và throughput ở giữa file là các thử nghiệm đã bị thay; cấu hình cuối ở mục "Sửa RAM nội" |
| `2026-11-23_phase5_soak_1h.md` | Soak 1 giờ cấu hình cuối | Số liệu cuối cùng của firmware S3 |

Số liệu tổng hợp và kết luận: [../RESULTS.md](../RESULTS.md). Script tạo ra chúng: [../../tools/](../../tools/).
