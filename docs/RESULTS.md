# Kết quả nghiên cứu: StickS3 (ESP32-S3) thay C5 cho WiFi và AEC trên P4C5

Tiếng Việt | [English](en/RESULTS.md)

Cập nhật 2026-11-24. IDF 6.1, esp_hosted 3.1.0, esp-sr 2.6.2. Chi tiết số đo từng giai đoạn: [logs/](logs/).

## Kết luận ngắn

S3 làm được vai trò co-processor cho P4C5 với hai việc cùng lúc: **WiFi 2,4 GHz qua SPI** và **AEC + khử nhiễu cho mic qua I2S**. Một bản phone chạy thật trên cấu hình này (WiFi, nhạc, radio, trợ lý có wake word, test mạng).
Đổi lại mất BLE, WiFi 5 GHz và chất lượng nhạc (link 16 kHz mono). Một số tiêu chí của plan chưa đạt hoặc chưa đo, xem các mục bên dưới.

## Được gì

| Hạng mục | Kết quả | Nguồn |
| --- | --- | --- |
| WiFi qua hosted SPI 26 MHz | TCP khoảng 10 đến 13 Mbps (soak 10 phút: RX 12,8, TX 14,3). Trên phone: tải xuống 11,7, tải lên 8,4 Mbps | logs phase 3, phone |
| UDP | Bão hòa khoảng 20 Mbps (TX) và 17 Mbps (RX) | logs phase 3 |
| AEC trên S3 (FD HIGH) | Giọng loa (far-end only): giảm 26,7 dB sau hội tụ, ngõ ra gần 0. Khi người nói chen (double-talk) giữ được giọng người, yếu hơn khoảng 5 dB, VAD đúng | logs phase 4 |
| Link I2S S3 sang P4 | 16 kHz, soak 1 giờ: 29 triệu khung mỗi chiều, 1 lỗi bộ đếm (lúc bắt đầu), 0 timeout, không trôi clock | logs phase 5 soak |
| CPU S3 khi chạy tất cả | Core 0 (WiFi, hosted, link): trung bình 38%, đỉnh 58,6%. Core 1 (AFE): trung bình 45,5%, đỉnh 60,2% | logs phase 5 soak |
| Trễ AFE | Trung bình 13 ms, tối đa 26 ms | logs phase 5 soak |
| Trễ link điện (P4 vào S3 ra P4) | 79 ms hai chiều, không dao động | logs phase 5 |
| Phone chạy trên S3 | WiFi, nhạc, radio, trợ lý (wake word "Hi ESP", hội thoại), test mạng, trang Giới thiệu hiện thông tin S3 | phone_s3 |
| Gỡ khỏi P4 | Không còn chạy AFE trên P4 khi dùng S3 (mic đã sạch từ S3) | phone_s3 |

## Mất gì

- **BLE:** không có (không dùng hosted HCI).
- **WiFi 5 GHz và WiFi 6:** S3 chỉ có WiFi 4, 2,4 GHz.
- **Chất lượng nhạc:** link 16 kHz mono, nên mất dải trên 8 kHz và mất stereo. Nhạc 44,1 hoặc 48 kHz được hạ mẫu xuống 16 kHz.
- **Barge-in qua AEC của P4:** tắt. AEC chỉ chạy trên S3, nó xử lý mic nhưng P4 không dùng nó để ngắt lời trợ lý.
- **Âm lượng:** chỉ có software volume trên P4 (không có điều khiển ES8311 từ phone).
- **Tính năng KSDIY bỏ khỏi bản phone_s3:** 4G, camera, USB camera, video, thư viện ảnh, quản lý file, thời tiết, màn hình thứ hai, máy tính, game, level.

## Hạn chế đã biết

- **Throughput giảm 18 đến 26%** so với mốc giai đoạn 3 (không audio) khi AFE và link chạy cùng. Tiêu chí plan là không quá 10%: **không đạt**. Vẫn cao hơn ngưỡng tuyệt đối 8 Mbps (gấp 2,6 lần bitrate video 4 Mbps).
- **Số đo throughput nhiễu:** chênh dưới khoảng 2 Mbps giữa các lần không có ý nghĩa, vì PC và AP cũng dùng WiFi.
- **RAM nội S3 sát:** tối thiểu khoảng 40 KB (ngưỡng plan 40 KB). Đạt nhờ đưa stream buffer và stack task vào PSRAM, giảm stack hosted xuống 6144 và giảm buffer WiFi. Thêm tính năng trên S3 sẽ dễ vượt.
- **Core 1 chạm ngưỡng 60%** ở đỉnh (AFE FD HIGH).
- **Trễ mic đến P4 chưa đo trực tiếp.** Ước tính trung bình 67 đến 72 ms, xấu nhất khoảng 95 ms (ngưỡng plan 80 ms). Có thể giảm khoảng 16 ms bằng cách giảm DMA của link từ 4 xuống 2 mô tả.
- **Chân G3 của StickS3 là chân strapping:** link playback dùng nó làm ngõ vào, nên S3 phải bỏ qua khung không có magic (0xA55A), nếu không chân hở gây phát tạp âm.
- **Reset qua cổng serial:** một số cách mở cổng serial (`idf.py flash` hoặc `monitor`, công cụ kéo DTR/RTS) reset board; reset P4 cũng reset S3 qua GPIO50. Mở cổng mà không kéo DTR/RTS (script trong `tools/`, `cat` sau `stty`) không reset trong các lần thử ở đây.
- **Mic khá ồn ở gain 24 dB:** nền khoảng 62 RMS, giới hạn các số đo RMS; cần đo lại với giọng thật ở nhiều khoảng cách.
- **Loa StickS3 nhỏ:** cần đường cong âm lượng riêng (−24 đến −8 dB), mức cố định 100 trên S3 và biên độ bíp 24000 để nghe rõ ở mức thấp.

## Chưa làm

- **Mốc chuẩn P4 + C5 qua SDIO** (giai đoạn 1 và 2): chưa đo, nên chưa có so sánh trực tiếp với C5.
- **BLE** cùng WiFi (giai đoạn 7).
- **FPS và CPU của P4** khi giải mã video qua S3 (giai đoạn 6): chưa đo; ứng dụng video đã gỡ khỏi bản phone_s3. Mức CPU P4 lấy lại được khi chuyển AFE sang S3 chưa có số.
- **Soak 8 giờ** (chỉ mới 1 giờ).
- **PC nối dây** để loại điểm nghẽn WiFi phía PC khi đo throughput.
- **Link I2S dưới tải thật của phone** (LVGL, camera, ghi flash): chưa đo. Rủi ro lý thuyết: task link của P4 đặt stack ở PSRAM, có thể đứng khi flash ghi, trong khi đệm DMA chỉ khoảng 32 ms.
- **AEC khi trợ lý nói qua loa** (barge-in): chưa đo bằng giọng thật.
- **NS mạng nơ-ron (nsnet2):** chưa nạp, nên cấu hình VC chưa phải VC đầy đủ.
- **S31 (Mosaico)** và Bluetooth audio (A2DP): chưa thử, S3 không có Bluetooth Classic.
- **H.265** trên P4: chưa tìm thấy decoder chính thức.
- **Hiển thị RAM và CPU của S3** trên trang Giới thiệu: cần kênh phụ (AUX) của hosted.

## Lỗi đã gặp và cách xử lý

| Lỗi | Nguyên nhân | Xử lý |
| --- | --- | --- |
| `pserial_task` tràn stack trên S3 | Stack hosted 4096 | Tăng lên 8192, sau đó giảm còn 6144 để tiết kiệm RAM |
| Task watchdog trên S3 khi chạy AFE | Phép tính trig kiểu double trên S3 (chỉ có FPU đơn) | Dùng float và bảng 16 phần tử |
| AFE không chạy ở core 1 | esp-sr V1 không có task riêng, chạy trong `feed()` và `fetch()` | Ghim task feed và fetch vào core 1 |
| S3 chạy 160 MHz | Tên Kconfig cũ bị bỏ qua trong IDF 6.1 | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` |
| P4 reboot liên tục (`ESP_ERR_NVS_NOT_INITIALIZED`) | `audio_fe` chạy trước khi NVS khởi tạo trên S3 | Khởi tạo NVS và event loop trước `audio_fe_start()` |
| Loa rè và hú | Ampli bật từ firmware cũ; chân G3 hở thành tạp âm | Tắt ampli qua PMIC; thêm magic 0xA55A |
| RAM nội S3 cạn (824 B) | Stream buffer 32 KB và stack lớn ở RAM nội | Stream buffer và stack task sang PSRAM |
| Bíp không nghe | Đóng codec cắt hàng đợi PCM | Chờ hàng đợi rỗng trước khi đóng |
| Nhạc và radio im, bộ đếm giây chạy nhanh | Bộ đệm đầu ra của resampler quá nhỏ | Dùng `get_max_out_sample_num` |
| Wake word nghe nhưng trợ lý không nhận giọng | `xStreamBufferSend` ghi dở số byte lẻ khi bộ đệm mic đầy, mọi mẫu sau lệch một byte | Chỉ ghi nguyên khối 256 byte vào bộ đệm mic |
| Âm lượng thấp không nghe | Đường cong mặc định −50 đến 0 dB quá dốc cho loa nhỏ | Đường cong −24 đến −8 dB, tăng mức S3 và biên độ bíp |
