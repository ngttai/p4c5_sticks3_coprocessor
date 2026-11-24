# Giai đoạn 4: AFE độc lập trên StickS3 (project `afe_s3`)

Ngày 2026-11-23. IDF 6.1, esp-sr 2.6.2, S3 240 MHz, 16 kHz, input "MR" (mic + ref số từ ES8311, `no_dac_ref=false`),
mic gain 24 dB, loa volume 45, tone 1 kHz -12 dBFS, mỗi cấu hình 14 s (tone từ giây 3 đến 11). NS/VAD dùng WebRTC
(không nạp model nsnet2, nên "VC" ở đây chưa phải VC có NS mạng nơ-ron). Ampli loa chỉ bật trong lúc phát tone.

Kiểm chứng đường ref: kênh R của bản ghi có RMS 5657 = đúng biên độ tone / căn 2, vậy ES8311 loopback DAC lên kênh phải hoạt động.

| Cấu hình | CPU core 0 (audio + fetch + AFE) | RAM nội tối thiểu | Tone 1 kHz ở ngõ ra AFE | RMS mic -> ngõ ra |
| --- | --- | --- | --- | --- |
| SR LOW | 17,2% | 261 KB | giảm 72 dB, dưới mức nền | 309 -> 70 (12,9 dB) |
| SR HIGH | 23,8% | 260 KB | 76 dB | 308 -> 60 (14,3 dB) |
| FD LOW | 29,1% | 260 KB | 76 dB | 309 -> 62 (14,0 dB) |
| FD HIGH | 36,7% | 260 KB | 78 dB | 309 -> 63 (13,8 dB) |
| VC LOW | 60,3% | 248 KB | 97 dB | 309 -> 7 (32,8 dB) |
| VC HIGH | 62,8% | 206 KB | 93 dB | 309 -> 11 (28,6 dB) |
| SR LOW, tắt AEC (đối chứng) | 1,6% | 206 KB | 0 dB | 309 -> 309 (0 dB) |

Cách đọc:
- Tone ở mic thô là 1275 RMS (volume 70) hoặc 309 (volume 45), còn ngõ ra AFE chỉ bằng mức nền lúc im lặng (SR/FD khoảng 62-70 RMS, VC khoảng 8 do có NS). AEC xóa hết tone, tới mức nền của mic.
- Số "ERLE" 72 đến 97 dB lấy từ biên độ tại bin 1 kHz so với nền nhiễu, nên chỉ là cận dưới, không phải ERLE thật. Chỉ số RMS bị mic noise floor (khoảng 62 RMS ở gain 24 dB) giới hạn: SR/FD chỉ đạt 13-14 dB theo RMS toàn băng dù tone đã mất hoàn toàn.
- Ngưỡng plan là 20 dB theo RMS: chỉ VC đạt, vì VC còn giảm cả nền. Cần đo lại bằng tiếng nói thật (không phải tone) và với mic gain thấp hơn để có số có ý nghĩa.

Vấn đề cần xử lý:
- (Đã xử lý) Core 1 từng idle 100%: esp-sr 2.6.2 giao diện V1 không tạo task AFE riêng, AEC/VAD chạy ngay trong `feed()`/`fetch()` của task gọi, nên `afe_perferred_core` không có tác dụng (task list giữa lúc chạy không có task AFE nào). Cách ghim: tạo task feed (đọc ES8311 + feed) và task fetch bằng `xTaskCreatePinnedToCore(..., 1)`.
- Chưa đo: tiếng nói, VAD với giọng, nsnet2, độ trễ mic -> P4.
- Tiếng rè cũ của loa do ampli bị bật từ firmware trước (PMIC reg 0x11 bit 3); đã tắt ở cp_s3 và afe_s3.

## Sau khi ghim feed/fetch vào core 1 (cùng ngày)

| Cấu hình | CPU core 0 | CPU core 1 |
| --- | --- | --- |
| SR LOW | 0,0% | 16,6% |
| SR HIGH | 0,0% | 23,7% |
| FD LOW | 0,0% | 29,1% |
| FD HIGH | 0,0% | 36,5% |
| VC LOW | 0,1% | 60,7% |
| VC HIGH | 0,1% | 63,2% |

Core 0 trống hoàn toàn cho WiFi, hosted SPI và I2S link ở giai đoạn 5. Ngưỡng plan cho core AFE là ở mức 60% hoặc thấp hơn: SR và FD đạt, VC chạm ngưỡng.
Chất lượng AEC không đổi so với trước khi ghim. Con số CPU này gồm cả đọc/ghi codec và tạo tone trong task audio, nên cao hơn một chút so với AFE thuần.

## Giọng nói tổng hợp (far-end only, chưa có người nói)

Nguồn: 12 giây TTS (flite, ba giọng: slt, kal16, awb), chuẩn hóa -14 LUFS (RMS 5761, peak 29202), phát lặp từ flash,
loa volume 45, mic gain 24 dB, AFE feed/fetch ghim core 1. Giây 3 đến 14 phát giọng, người không nói. Tiếng nói ở mic
khoảng 200 đến 800 RMS, nền mic 65 RMS. Độ giảm tính trên tổng năng lượng mic và ngõ ra AFE trong cửa sổ phát.

| Cấu hình | Giảm toàn cửa sổ | Giảm sau hội tụ (giây 7 đến 14) | Nền ngõ ra khi im | CPU core 1 | Nhận xét |
| --- | --- | --- | --- | --- | --- |
| SR LOW | -6,9 dB | 3,4 dB | 65,6 | 17,7% | AEC gần như không khử được giọng; VAD báo "nói" 100% do dội âm |
| FD LOW | 5,8 dB | 7,4 dB | 66,0 | 30,7% | Yếu |
| FD HIGH | 18,8 dB | 26,7 dB | 65,1 | 38,2% | Tốt: ngõ ra 1 đến 19 RMS, VAD 0% trong lúc phát |
| VC LOW | 22,8 dB | 22,1 dB | 16,7 | 62,8% | Tốt, kèm NS, nhưng CPU sát ngưỡng |
| SR LOW không AEC | 0,0 dB | 0,0 dB | 66,1 | 1,8% | Đối chứng |

Kết luận tạm thời:
- Tone 1 kHz là phép thử quá dễ: mọi cấu hình đều xóa sạch. Với giọng nói, chỉ FD HIGH (và VC) khử được.
- SR_LOW (`NLP_OFF`) không dùng được cho full-duplex: dội âm làm VAD luôn báo có tiếng nói.
- Ứng viên cho giai đoạn 5: FD HIGH (khoảng 38% core 1, RAM nội tối thiểu 259 KB).
- Chưa đo double-talk (người nói chen lúc loa phát) nên chưa biết FD HIGH có giữ được giọng người hay không.
- Giọng flite không giống giọng thật về phổ và nhịp, nên cần kiểm lại với giọng thật qua loa ngoài.

## Double-talk (người nói thật chen vào lúc loa phát giọng)

Quy trình mỗi vòng (20 s): bip ba lần (giây 1 đến 2), loa phát giọng TTS từ giây 3 đến 16, bip đơn giây 8 = bắt đầu nói,
bip đơn giây 15 = dừng nói. Cửa sổ far-end-only = giây 6 đến 7, double-talk = giây 10 đến 14, nền = giây 17 đến 19.
Cùng một người nói, miệng cách mic khoảng 15 đến 20 cm. Giọng người ước lượng bằng năng lượng mic(DT) trừ năng lượng
dội âm đo ở far-end-only, nên chỉ là xấp xỉ (nội dung TTS khác nhau theo đoạn).

| Cấu hình | Far-end-only: mic -> out (RMS) | Double-talk: mic -> out (RMS) | Giọng người ước lượng | out / giọng người | VAD khi nói | Nền khi im |
| --- | --- | --- | --- | --- | --- | --- |
| FD HIGH | 337 -> 0,2 đến 0,4 | 566 -> 249 | 455 | -5,2 dB | 100% | 23 |
| VC LOW | 406 -> 16 | 575 -> 237 | 407 | -4,7 dB | 100% | 13 |
| SR LOW, không AEC (đối chứng) | 291 -> 291 | 492 -> 492 | 397 | +1,9 dB | 100% | 40 |

Nhận xét:
- Cả FD HIGH và VC LOW đều giữ được giọng người khi loa đang phát: ngõ ra còn khoảng 237 đến 249 RMS, chỉ yếu hơn giọng gốc khoảng 5 dB, và VAD báo nói 100%. Dội âm far-end vẫn bị xóa sạch ở FD HIGH (0,2 đến 0,4 RMS khi chỉ có loa phát).
- VAD ở far-end-only: FD HIGH 0% trong giây 5 đến 8, còn VC LOW có lúc báo nói (93% ở giây 6, 22% ở giây 8): VC dễ kích VAD nhầm bằng tiếng loa.
- Một lần chạy FD HIGH đầu tiên bị loại: ngõ ra chỉ 23 RMS lúc double-talk, nhưng cửa sổ đó người nói chưa đúng lúc nên không dùng (log chỉ xuất hiện trong phiên làm việc).
- Số "BROADBAND ... out floor" trong chế độ này sai (tính cả 3 tiếng bip ở giây 1 đến 2), đừng dùng; nền đúng là cột cuối bảng trên.
- Mới một người nói, một lần mỗi cấu hình, một tư thế. Chưa đo ở khoảng cách xa, tiếng nói to nhỏ khác nhau, hay loa phát to hơn.

Kết luận giai đoạn 4: chọn FD HIGH cho giai đoạn 5 (khoảng 38% core 1, RAM nội tối thiểu 259 KB, khử dội âm giọng nói
đến dưới nền, giữ được giọng người, VAD không bị kích nhầm bởi loa). VC LOW tốn khoảng 63% core 1 và dễ kích VAD nhầm.
