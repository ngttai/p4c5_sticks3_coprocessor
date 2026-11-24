# host_p4

[Tiếng Việt](README.md) | English

Test firmware for the KSDIY P4C5 (ESP32-P4): esp_hosted 3.1.0 host over SPI to the StickS3, an `iperf` console and two commands to check the I2S link. Use it to check the wiring and measure before running `../phone_s3`.
Built from the esp_hosted `wifi/iperf` host example; its original README is [README_upstream.md](README_upstream.md). Differences from the example: `esp-extconn` is removed (it drives the C5 over SDIO) and GPIO54 (the C5's EN) is pulled low.

## Console commands (USB-Serial/JTAG)
| Command | Purpose |
| --- | --- |
| `sta_connect "<ssid>" <password>` | Join a 2.4 GHz WiFi network |
| `iperf -s -i 10 -t 60` / `iperf -c <PC ip> -i 10 -t 60` | Measure TCP RX / TX (the PC runs the source or sink, see `../tools/`) |
| `linktest [seconds] [tone]` | Count I2S frames and report counter errors and timeouts; `tone` sends a 1 kHz tone to the S3 |
| `linklat [n]` | Measure link latency with a click pulse |

SPI clock: `CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ` in `sdkconfig.defaults` (10, 20, 26 or 40; 26 is the most stable).

## Build
```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32p4 && idf.py build && idf.py -p <P4 port> flash
```
