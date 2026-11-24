# tools

[Tiếng Việt](README.md) | English

PC-side measurement scripts that produced the numbers in [../docs/logs/](../docs/logs/). They need Python 3 with `pyserial` (`pip install pyserial`).

## Shared environment variables

| Variable | Meaning |
| --- | --- |
| `P4_PORT` | Serial port of the P4C5, for example `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<id>-if00` |
| `S3_PORT` | Serial port of the StickS3 |
| `PC_IP` | IP address of the PC on the same WiFi network as the S3 (for the P4-to-PC TX direction) |
| `WS`, `WP` | 2.4 GHz WiFi name and password. Read from the environment only, never stored in a file; the logs mask both values |
| `OUT_DIR` | (optional) where `soak1h.py` writes its result, default the temp directory |

The scripts open the port with `dtr` and `rts` off so they **do not reset the board**. Tools that toggle DTR/RTS when opening the port (such as `idf.py monitor` or `flash`) can reset the P4, and through GPIO50 the S3 as well.

## Scripts

| Script | Purpose | Firmware needed |
| --- | --- | --- |
| `soak1h.py [seconds]` | TCP RX then TX (1800 s each by default) together with the I2S link and AFE; summarizes throughput, RAM, CPU, AFE lag and errors | `cp_s3` (AFE on) and `host_p4` |
| `combo.py [seconds] [rx\|tx\|both]` | iperf TCP together with `linktest ... tone`; prints throughput and the S3 log | same |
| `lt.py "<command>" ...` | Runs P4 console commands (for example `linktest 20 tone`) and prints the result with the S3 `audio_fe` lines | same |
| `lat.py linklat [n]` | Measures link latency with a click pulse. The electrical measurement needs `cp_s3` built with `AUDIO_FE_LINK_ECHO` | same |
| `cap_dt.py <file>` | Logs the S3 serial output until `ALL DONE` (the `afe_s3` double-talk test) | `afe_s3` |
| `listen.py <port> <seconds>` | Reads a serial port without resetting the board | any |

Example:

```bash
export P4_PORT=/dev/serial/by-id/...P4...  S3_PORT=/dev/serial/by-id/...S3...
export PC_IP=192.168.1.10  WS='wifi-name'  WP='wifi-password'
python3 tools/combo.py 60 both
```

Network test server for the phone's Settings screen: [../phone_s3/tools/speed_server.py](../phone_s3/tools/speed_server.py).

## Notes

- `combo.py` and `soak1h.py` join WiFi with the `sta_connect` command on the P4 console, then measure with the IDF `iperf` command. They assume the `host_p4` build with the `linktest` and `linklat` commands.
- Throughput numbers are noisy by about 2 Mbps because the PC also uses WiFi; comparing configurations needs repeated runs or a wired PC.
