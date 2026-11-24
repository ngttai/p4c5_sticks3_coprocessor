# cp_s3

English | [Tiếng Việt](README.vi.md)

Firmware for the M5StickS3 (ESP32-S3): an esp_hosted 3.1.0 co-processor (2.4 GHz WiFi over SPI) plus `audio_fe` (ES8311, AFE FD HIGH, I2S link to the P4).
Built from the esp_hosted `wifi/iperf` cp example; the example's original README is [README_upstream.md](README_upstream.md).

## Pins
SPI to the P4: CLK 5, MOSI 6, MISO 7, CS 8, handshake 1, data ready 2, reset 9 (`CONFIG_EH_TRANSPORT_CP_SPI_GPIO_RESET=9`). I2S link: BCLK 43, WS 44, mic out 4, playback in 3. Full list: [../docs/WIRING.md](../docs/WIRING.md).

## Configuration (menuconfig, section "StickS3 audio_fe")
| Option | Meaning | Default |
| --- | --- | --- |
| `AUDIO_FE_ENABLE` | Run audio_fe (ES8311 + AFE + link) | y |
| `AUDIO_FE_BYPASS` | Send the raw microphone to the link, no AFE (check the wiring first) | n |
| `AUDIO_FE_AMP_ALWAYS_ON` | Amplifier always on (latency measurements only) | n |
| `AUDIO_FE_LINK_ECHO` | Return the P4 playback to the P4 (electrical link latency) | n |
| `AUDIO_FE_FD_HIGH` | AFE FD HIGH_PERF (otherwise FD LOW_COST) | y |

The console uses USB-Serial/JTAG (UART0 pins G43 and G44 are taken by the I2S link). Link protocol: [../docs/I2S_LINK.md](../docs/I2S_LINK.md).

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3 && idf.py build && idf.py -p <S3 port> flash
```
