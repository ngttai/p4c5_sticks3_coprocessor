# Giao thức link I2S giữa S3 và P4

Cài ở `cp_s3/main/audio_fe.c` (phía S3) và `phone_s3/components/esp32p4c5_bsp/src/esp32p4c5_audio.c` (phía P4). Bản thử độc lập ở `host_p4/main/link_test.c`.

## Định dạng

- **Master:** S3, I2S1, cấp BCLK (G43) và WS (G44). **Slave:** P4, I2S0.
- 16 kHz, 16 bit, Philips, stereo (hai kênh L và R trong mỗi khung).
- Khối 128 khung (8 ms), DMA 4 mô tả (khoảng 32 ms). Độ trễ vòng điện: 79 ms (đo ở chế độ echo).

| Hướng | Kênh L | Kênh R |
| --- | --- | --- |
| S3 → P4 (chân G4 → GPIO48) | Mic sau AFE (AEC, NS) | Bộ đếm 16 bit, tăng 1 mỗi khung |
| P4 → S3 (chân GPIO49 → G3) | Playback mono 16 kHz | Magic `0xA55A` khi có dữ liệu playback, `0` khi không |

## Quy tắc

1. **Bộ đếm:** P4 so bộ đếm của mỗi khung với giá trị kỳ vọng. Lệch thì tăng `ctr_err` (hiện ở trang Giới thiệu của phone). Một lỗi lúc bắt đầu là bình thường.
2. **Magic:** S3 coi chunk là hợp lệ khi quá 3/4 khung có `0xA55A` ở kênh R. Chunk không hợp lệ bị thay bằng im lặng. Quy tắc này chặn tạp âm khi chân G3 hở hoặc P4 chưa chạy.
3. **Ampli:** S3 bật ampli AW8737 (qua PMIC) khi chunk hợp lệ và đỉnh lớn hơn 30; tắt sau 2 giây không có tín hiệu.
4. **Bộ đệm mic ở P4:** `link_task` chỉ ghi vào stream buffer mic khi còn chỗ cho **nguyên một khối** 256 byte. Stream buffer FreeRTOS chấp nhận ghi dở khi gần đầy; số byte lẻ làm mọi mẫu sau lệch một byte (nhiễu toàn thang, mẫu là bội số của 256). Nếu đầy thì bỏ cả khối và tăng `mic_overrun`.
5. **Mở và đóng:** mở thiết bị (`d_enable`) reset stream buffer tương ứng. Đóng loa chờ hàng đợi rỗng tối đa 1 giây rồi thêm 80 ms, để tiếng bíp ngắn không bị cắt.

## Phía P4

- Codec ảo: `bsp_audio_codec_speaker_init()` nhận mono 16 bit ở tần số bất kỳ. Bộ hạ mẫu `esp_ae_rate_cvt` đưa về 16 kHz; bộ đệm đầu ra phải đủ cho `esp_ae_rate_cvt_get_max_out_sample_num`.
- Volume: software, qua esp_codec_dev. Đường cong đặt trong `bsp_audio_codec_speaker_init()`: −24 dB ở 1% đến −8 dB ở 100% (0 là tắt tiếng).
- `link_task` ưu tiên 18, đặt trên core 1, stack ở PSRAM, log mỗi 5 giây (`frames`, `ctr_err`, `play_underrun`, `mic_overrun`, `spk`, `mic`, `play_q`).

## Phía S3

- `link_task` (core 0, ưu tiên 10): đọc và ghi khối 128 khung; đẩy playback vào `rxq`, lấy mic sạch từ `txq`.
- `codec_task` (core 1): ghi playback ra ES8311, đọc mic và ref (kênh R loopback DAC), feed AFE (`"MR"`).
- `fetch_task` (core 1): lấy ngõ ra AFE đẩy vào `txq`.
- Mức cố định của ES8311 là 100; mic gain 24 dB.

## Lỗi hay gặp

| Triệu chứng | Nguyên nhân thường gặp |
| --- | --- |
| Loa phát tạp âm hoặc im | Chân G3 hở (không có magic), hoặc ampli chưa bật (đỉnh không quá 30) |
| Mic nhiễu toàn thang sau wake word | Bộ đệm mic ghi dở byte lẻ (đã sửa ở phiên bản này) |
| `ctr_err` tăng liên tục | P4 và S3 lệch khung (reset một bên), hoặc dây I2S dài, nhiễu |
| `play_underrun` tăng | P4 bận, không kịp đẩy playback |

## Lệnh thử link (host_p4)

- `linktest [giây] [tone]`: P4 làm slave, đếm khung, in mỗi giây RMS và đỉnh của kênh L, số lỗi bộ đếm; cuối bài in `LINKTEST RESULT`. `tone` gửi tone 1 kHz xuống S3.
- `linklat [n]`: gửi xung click và đo thời gian nhận lại. Đo điện cần `cp_s3` bật `AUDIO_FE_LINK_ECHO`; đo qua loa và mic không dùng được vì nền mic ồn (xem log phase 5).
