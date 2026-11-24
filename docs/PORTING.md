# Port `phone_s3` sang board P4 khác

Tiếng Việt | [English](en/PORTING.md)

`phone_s3` tách phần phụ thuộc phần cứng vào một BSP cục bộ, `phone_s3/components/esp32p4c5_bsp`. Board mới cần BSP riêng của nó; phần giao diện và ứng dụng không đổi nếu BSP cung cấp cùng API.

## Điều kiện phần cứng

- Board P4 có sẵn **ít nhất 11 GPIO** rảnh ở mức 3,3 V: 7 cho SPI hosted (CLK, MOSI, MISO, CS, handshake, data ready, reset) và 4 cho I2S (BCLK, WS, mic vào, playback ra), cộng GND.
- Một StickS3 chạy `cp_s3`, nối theo [WIRING.md](WIRING.md). Nếu board mới có C5, C6 hay co-processor khác trên SDIO thì phải giữ nó im hoặc bỏ nó.
- Màn hình, cảm ứng, thẻ SD, PMIC, nút theo board mới. Tham chiếu board gốc: [../hw_specs/hw_matrix.md](../hw_specs/hw_matrix.md).

## Việc phải làm

1. **Chân SPI hosted:** sửa khối `CONFIG_ESP_HOSTED_HOST_*` trong `phone_s3/sdkconfig.defaults` (CLK, MOSI, MISO, CS, HANDSHAKE, DATA_READY, RESET, `SPI_CLK_MHZ`). Bắt đầu ở 10 MHz, tăng dần; 26 MHz là mức ổn định đã đo trên board gốc.
2. **Chân link I2S:** sửa `LINK_BCLK`, `LINK_WS`, `LINK_DIN`, `LINK_DOUT` ở đầu `components/esp32p4c5_bsp/src/esp32p4c5_audio.c`. Chọn cổng I2S ở `BSP_I2S_NUM`. Chân phía S3 giữ như trong [WIRING.md](WIRING.md); nếu đổi phải sửa `cp_s3` (xem bước 6).
3. **Giữ C5 im:** `components/esp32p4c5_bsp/src/esp32p4c5_wifi.c` kéo GPIO54 xuống thấp. Bỏ dòng này hoặc đổi chân nếu board không có C5.
4. **Viết lại các phần BSP khác theo board mới:** `esp32p4c5_bsp.c` (nguồn, GPIO), `esp32p4c5_pmic.c` (AXP2101), `esp32p4c5_sdcard.c`, `esp32p4c5_button.c`, `esp32p4c5_led.c`, `esp32p4c5_imu.c`, `esp32p4c5_spiffs.c`, `esp32p4c5_camera.c`, cùng `include/bsp/` (màn hình, cảm ứng, định nghĩa chân). Các hàm công khai trong `include/bsp/esp32p4c5_bsp.h` và `esp-bsp.h` là hợp đồng với phần ứng dụng.
5. **Phân vùng và bộ nhớ:** kiểm tra `partitions.csv` với dung lượng flash của board; kiểm tra PSRAM (các stack và stream buffer của link dùng PSRAM).
6. **Firmware S3:** nếu đổi chân phía S3, sửa `sdkconfig.defaults` và `sdkconfig.defaults.esp32s3` của `cp_s3` (SPI) và các `#define` chân I2S trong `cp_s3/main/audio_fe.c`. Nạp `cp_s3` trước, rồi `host_p4` để thử link, rồi mới tới `phone_s3`.

## Thứ tự kiểm tra gợi ý

1. `host_p4`: hosted SPI lên được, `iperf` chạy (xem README của `host_p4`).
2. `host_p4`: lệnh `linktest 10 tone` đếm khung, `ctr_err` không tăng.
3. `phone_s3`: WiFi, rồi âm thanh (bíp, nhạc), rồi trợ lý và wake word.

## Điểm dễ vấp

- **Chân strapping của S3 (G3):** không đổi vai trò; chỉ dùng làm ngõ vào và để P4 đẩy magic `0xA55A` (xem [I2S_LINK.md](I2S_LINK.md)).
- **Reset qua cổng serial:** một số công cụ mở cổng làm reset board; dùng script trong [../tools/](../tools/) để ghi log mà không reset.
- **Phiên bản:** dùng IDF 6.1 và `esp_hosted` 3.1.0 cho cả hai phía; slave stock C5 trên board gốc dùng 2.12.13 nên không dùng chung được.
- **RAM nội S3 sát 40 KB:** thêm tính năng ở S3 cần kiểm lại.
