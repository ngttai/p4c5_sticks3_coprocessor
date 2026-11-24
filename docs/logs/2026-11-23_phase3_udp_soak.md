# Phase 3: UDP and 10-minute soak (SPI 26 MHz)

English | [Tiếng Việt](vi/2026-11-23_phase3_udp_soak.md)

Date 2026-11-23. IDF 6.1, esp_hosted 3.1.0 (host and cp matched), S3 at 240 MHz, checksum on.
PC side: Python source/sink (iperf2 installed later), PC on 5 GHz WiFi, 2.4 GHz AP for the S3. Measured with the IDF iperf on the P4.

## UDP, 10 seconds per level

| Offered load | TX P4->PC (PC receives) | RX PC->P4 (P4 reports) |
| --- | --- | --- |
| 10 Mbps | 10.0 Mbps | 9.5 Mbps |
| 20 Mbps | 19.0 Mbps | 16.5 to 17.2 Mbps |
| 30 Mbps | 20.1 Mbps (saturated) | 17.3 Mbps (saturated) |

## 10-minute TCP soak (reported every 60 seconds)

| Direction | Average | Min / Max per minute | S3 reboots | S3 errors | P4 transport errors |
| --- | --- | --- | --- | --- | --- |
| RX PC->P4 | 12.8 Mbps | 11.6 / 13.3 | 0 | 0 | 0 |
| TX P4->PC | 14.3 Mbps | 10.5 / 16.0 | 0 | 0 | 0 |

The last minute of TX dropped to 10.5 Mbps (possibly WiFi or the PC; the cause was not separated).

## Not measured
- 8-hour soak, S3 CPU and internal RAM, comparison with iperf running directly on the S3.
- Wired PC (to rule out the WiFi bottleneck on the PC side).
- Noted: the StickS3 made a hissing sound (cause not identified at the time: speaker amplifier not controlled, or WiFi noise). It was later traced to the amplifier left on by earlier firmware (see the phase 4 log).
