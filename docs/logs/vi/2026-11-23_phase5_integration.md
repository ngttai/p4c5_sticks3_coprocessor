# Giai đoạn 5: gộp hosted SPI + AFE FD HIGH + I2S link trên S3

[English](../2026-11-23_phase5_integration.md) | Tiếng Việt

Ngày 2026-11-23. IDF 6.1, esp_hosted 3.1.0, esp-sr 2.6.2, SPI 26 MHz, S3 240 MHz, AFE_TYPE_FD + HIGH_PERF, input "MR".
Firmware: `cp_s3/main/audio_fe.c` (ES8311 + AFE + I2S1 master tới P4), `host_p4/main/link_test.c` (lệnh `linktest [giây] [tone]`, I2S slave).
Dây I2S: P4 GPIO46 BCLK, 47 WS, 48 mic sạch, 49 playback; S3 G43, G44, G4, G3. Khung S3 -> P4: L = mic (sau AFE), R = bộ đếm 16 bit.
Khung P4 -> S3: L = playback, R = magic 0xA55A (S3 bỏ qua khung không có magic, nên G3 hở không làm loa phát tạp âm).
Core: task feed + fetch (AFE) ghim core 1, link I2S ghim core 0, hosted/WiFi ở core 0.

## Kết quả

Link I2S (S3 master 16 kHz): đúng 16 kHz (1 152 000 khung trong 72 s), lỗi bộ đếm = 1 mỗi lần chạy (lúc P4 bắt đầu nhận),
0 timeout, rx_overrun = 0; không thấy trôi lệch giữa hai miền clock I2S0 (codec) và I2S1 (link) trong các lần đo 52 đến 72 s.

AEC qua link (P4 phát tone 1 kHz xuống S3, loa phát, mic thu lại): mic sạch ở P4 có L RMS khoảng 85 (xấp xỉ nền khi im 80 đến 100),
so với 505 đến 517 khi bypass AFE. Tone bị xóa đến mức nền.

Throughput TCP cùng lúc với link tone (26 MHz), so với mốc giai đoạn 3 (không audio): RX 12,8 và TX 14,3 Mbps.

| Biến thể cp_s3 | RX PC->P4 | TX P4->PC | RAM nội tối thiểu S3 | Nhận xét |
| --- | --- | --- | --- | --- |
| Bộ đệm WiFi mặc định (RX động 32, TX động 32, RX tĩnh 10), WiFi/LWIP ở RAM nội | 11,7 Mbps (-9%) | 12,1 Mbps (-16%) | 824 B | Gần cạn RAM nội, không dùng được |
| Như trên + WiFi/LWIP ưu tiên PSRAM | 11,9 Mbps | 6,3 Mbps, giảm dần 9,8 xuống 3,6 | 39,9 KB | TX bị nghẽn, loại |
| Bộ đệm WiFi nhỏ (RX động 16, TX động 16, RX tĩnh 8) | 10,5 Mbps (-18%) | 10,6 Mbps (-26%) | 39,8 KB | Cấu hình đang dùng |

Đánh giá theo plan:
- Throughput khi AFE chạy cùng giảm không quá 10%: KHÔNG ĐẠT ở cấu hình đang dùng (giảm 18 đến 26%), nhưng vẫn cao hơn ngưỡng tuyệt đối 8 Mbps (gấp 2,6 lần bitrate video 4 Mbps). Mốc giai đoạn 3 cũng dao động mạnh (TX từ 10,5 đến 16 Mbps theo phút), mỗi biến thể mới đo một lần.
- RAM nội tối thiểu >= 40 KB: gần đạt, 39,8 KB.
- I2S link không underrun/magic sai: đạt trong 52 đến 72 s (chưa đo 1 giờ).
- CPU S3 core 1 (AFE): chưa đo trong firmware tích hợp; ở giai đoạn 4 là khoảng 38%.
- Chưa đo: CPU core 0 khi có traffic, độ trễ mic -> P4, 1 giờ, BLE, FPS giải mã trên P4.

Thay đổi cấu hình đã làm trong cp_s3 (đều nằm trong `sdkconfig.defaults*`):
- Console chuyển sang USB-Serial/JTAG (UART0 G43/G44 dành cho I2S link).
- PSRAM octal 8 MB; task `codec`, `afe_fetch`, `fe_stats` đặt stack ở PSRAM; `SPIRAM_MALLOC_ALWAYSINTERNAL` = 2048.
- Partition riêng `partitions_cp_s3.csv` (2 phân vùng OTA 3,5 MB); app 1,64 MB.
- Khởi tạo NVS trước `audio_fe_start()`: nếu không, `esp_wifi_init` do P4 gọi qua RPC có thể thua NVS và P4 rơi vào vòng reboot (đã gặp một lần).

## Độ trễ và CPU (cùng ngày, sau khi đổi link sang chunk 128 khung = 8 ms, DMA 4 x 8 ms)

Độ trễ link điện (cp_s3 chế độ `AUDIO_FE_LINK_ECHO`: S3 trả playback của P4 ngược lại, P4 gửi click 24000 và đo): 10 lần đều
**79 ms** (không dao động), đi và về. Một chiều S3 -> P4 khoảng 40 ms (DMA 32 ms + 8 ms chunk). Đo bằng âm thanh thật
(P4 -> loa -> mic -> P4) không dùng được: nền mic có đỉnh 1500 đến 2800, click chỉ nổi lên khoảng 4400, phát hiện sai.

Độ trễ AFE (S3 tự đo: từ lúc chunk 512 mẫu đọc xong khỏi codec đến lúc `fetch()` trả mẫu tương ứng):
trung bình 10,5 ms khi rảnh, 15 đến 16 ms khi có iperf RX (tối đa 22 ms).

Ước tính mic -> P4 (không đo trực tiếp, cộng các thành phần): thu đủ chunk 32 ms (trung bình 16 ms, tối đa 32 ms) + AFE 11 đến 16 ms
(tối đa 22 ms) + link S3 -> P4 khoảng 40 ms = trung bình khoảng 67 đến 72 ms, xấu nhất khoảng 95 ms. Ngưỡng plan 80 ms:
đạt về trung bình, sát ở trường hợp xấu nhất. Có thể giảm khoảng 16 ms nếu bớt DMA link từ 4 xuống 2 mô tả.

CPU S3 (FD HIGH + link tone + iperf TCP RX 40 s):

| Điều kiện | Core 0 (hosted, WiFi, link I2S) | Core 1 (AFE feed + fetch) |
| --- | --- | --- |
| Rảnh, chỉ audio_fe | 15% | 33% |
| iperf RX 10 Mbps | 37 đến 47% | 52 đến 55% |

Cả hai nằm trong ngưỡng plan (core 1 <= 60%, core 0 <= 70%).

RAM nội: trong lần chạy này `min free` tụt xuống **12 KB** khi có traffic (rảnh khoảng 55 KB), không còn 39,8 KB như lần trước.
Không đạt ngưỡng 40 KB; chưa rõ vì sao khác lần trước (có thể do thay đổi chunk/DMA, biến động theo traffic TCP). Cần điều tra trước khi soak.

## Sửa RAM nội trên S3 (cùng ngày)

Nguyên nhân chính: hai stream buffer của audio_fe (txq và rxq, mỗi cái 8192 mẫu = 16 KB) nằm trong RAM nội, cộng thêm stack
8 KB cho mỗi task hosted (tôi đã tăng từ 4096 để sửa tràn stack của `pserial_task`). RAM nội tối thiểu khi có traffic tụt xuống 10 đến 15 KB.

Các thử nghiệm (mỗi mục đo 40 s TCP + link tone, RAM tối thiểu trong lúc chạy):

| Thay đổi | RAM nội tối thiểu | Kết quả |
| --- | --- | --- |
| Hàng đợi SPI hosted 20 xuống 10 | khoảng 15 KB | Không cải thiện RAM, throughput kém đi; hoàn nguyên |
| Tắt WiFi IRAM_OPT và RX_IRAM_OPT | không đo | Throughput RX giảm còn 4 đến 6 Mbps; hoàn nguyên |
| Stack task hosted 8192 xuống 6144 | RX 31 KB, TX 9,8 KB | RAM rảnh tăng 12 KB, kết nối WiFi vẫn ổn (4096 thì `pserial_task` tràn) |
| Stream buffer sang PSRAM (`xStreamBufferCreateWithCaps`) | **TX 41,8 KB, RX 40,4 KB** | Đạt ngưỡng 40 KB; RAM rảnh khi nghỉ khoảng 103 KB |

Cấu hình cuối cùng: stack hosted 6144, hai stream buffer ở PSRAM, WiFi/hosted giữ nguyên (WiFi IRAM_OPT bật, hàng đợi SPI 20).
Throughput TCP cùng lúc với AFE và link tone (40 s, một lần mỗi chiều): RX trung bình 11,0 Mbps (9,5 đến 12,3), TX trung bình 8,7 Mbps (6,2 đến 11,7).

Lưu ý về độ nhiễu của throughput: chạy lặp cùng một firmware RX cho 7,7, 11 và 9,9 Mbps. Chênh lệch giữa các cấu hình dưới khoảng 2 Mbps
không có ý nghĩa thống kê (PC và AP cùng dùng WiFi). Cần đo dài hơn và PC nối dây để so sánh chính xác.
