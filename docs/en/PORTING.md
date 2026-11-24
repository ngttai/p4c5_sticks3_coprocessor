# Porting `phone_s3` to another P4 board

[Tiếng Việt](../PORTING.md) | English

`phone_s3` keeps its hardware dependencies in a local BSP, `phone_s3/components/esp32p4c5_bsp`. A new board needs its own BSP; the UI and apps stay unchanged if the BSP provides the same API.

## Hardware requirements

- A P4 board with **at least 11 free GPIOs** at 3.3 V: 7 for the hosted SPI (CLK, MOSI, MISO, CS, handshake, data ready, reset) and 4 for I2S (BCLK, WS, microphone in, playback out), plus GND.
- A StickS3 running `cp_s3`, wired as in [WIRING.md](WIRING.md). If the new board has a C5, C6 or another SDIO co-processor, keep it idle or remove it.
- Display, touch, SD card, PMIC and buttons as the new board has them. Reference for the original board: [../../hw_specs/hw_matrix.md](../../hw_specs/hw_matrix.md).

## What to do

1. **Hosted SPI pins:** edit the `CONFIG_ESP_HOSTED_HOST_*` block in `phone_s3/sdkconfig.defaults` (CLK, MOSI, MISO, CS, HANDSHAKE, DATA_READY, RESET, `SPI_CLK_MHZ`). Start at 10 MHz and raise it; 26 MHz is the stable value measured on the original board.
2. **I2S link pins:** edit `LINK_BCLK`, `LINK_WS`, `LINK_DIN`, `LINK_DOUT` at the top of `components/esp32p4c5_bsp/src/esp32p4c5_audio.c`. Choose the I2S port with `BSP_I2S_NUM`. Keep the S3-side pins as in [WIRING.md](WIRING.md); if you change them, `cp_s3` must change too (step 6).
3. **Keep the C5 idle:** `components/esp32p4c5_bsp/src/esp32p4c5_wifi.c` pulls GPIO54 low. Remove that line or change the pin if the board has no C5.
4. **Rewrite the other BSP parts for the new board:** `esp32p4c5_bsp.c` (power, GPIO), `esp32p4c5_pmic.c` (AXP2101), `esp32p4c5_sdcard.c`, `esp32p4c5_button.c`, `esp32p4c5_led.c`, `esp32p4c5_imu.c`, `esp32p4c5_spiffs.c`, `esp32p4c5_camera.c`, and `include/bsp/` (display, touch, pin definitions). The public functions in `include/bsp/esp32p4c5_bsp.h` and `esp-bsp.h` are the contract with the application code.
5. **Partitions and memory:** check `partitions.csv` against the board's flash size; check PSRAM (the link's stacks and stream buffers use it).
6. **S3 firmware:** if you change the S3-side pins, edit `sdkconfig.defaults` and `sdkconfig.defaults.esp32s3` of `cp_s3` (SPI) and the I2S pin `#define`s in `cp_s3/main/audio_fe.c`. Flash `cp_s3` first, then `host_p4` to test the link, and only then `phone_s3`.

## Suggested test order

1. `host_p4`: hosted SPI comes up and `iperf` runs (see [I2S_LINK.md](I2S_LINK.md) for the link commands).
2. `host_p4`: `linktest 10 tone` counts frames and `ctr_err` does not grow.
3. `phone_s3`: WiFi, then audio (beep, music), then the assistant and wake word.

## Pitfalls

- **S3 strapping pin (G3):** do not change its role; it is only an input and carries the magic `0xA55A` from the P4 (see [I2S_LINK.md](I2S_LINK.md)).
- **Reset through the serial port:** some tools that open the port reset the board; use the scripts in [../../tools/](../../tools/) to log without resetting.
- **Versions:** use IDF 6.1 and `esp_hosted` 3.1.0 on both sides; the stock C5 slave on the original board uses 2.12.13 and cannot be mixed.
- **S3 internal RAM is close to 40 KB:** re-check it after adding anything on the S3.
