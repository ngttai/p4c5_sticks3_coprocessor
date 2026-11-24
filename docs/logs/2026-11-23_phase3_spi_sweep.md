# Phase 3: SPI clock sweep (hosted SPI full-duplex, StickS3 as co-processor)

English | [Tiếng Việt](vi/2026-11-23_phase3_spi_sweep.md)

Date 2026-11-23. IDF 6.1, esp_hosted 3.1.0 (host and cp matched), S3 at 240 MHz, checksum on, short jumper wires.
PC side: Python TCP source/sink (iperf was not installed), PC on 5 GHz WiFi in the same 192.168.1.0/24 network, 2.4 GHz AP for the S3.
The P4 ran `iperf -c` / `iperf -s` (the IDF iperf), 10 seconds, TCP, one run per level. The result is the average reported by the P4.

| SPI (MHz) | TCP TX P4->PC (Mbps) | TCP RX PC->P4 (Mbps) | Note |
| --- | --- | --- | --- |
| 10 | 7.4 | 7.2 (stable) | |
| 20 | 12.6 | 8.5 to 10.8 | |
| 26 | 12.9 | 11.6 (stable at 14) | best and stable |
| 40 | 8.2 | 8.3 | swings widely (2 to 14), not stable |

Not measured: UDP, 60 seconds, 10 minutes, comparison with iperf running directly on the S3, S3 CPU and internal RAM.
The PC side is also on WiFi, so it may be the bottleneck; this needs confirming with a wired PC.
Problem found and fixed: `pserial_task` overflowed its stack on the S3 during `sta_connect` (stack 4096 -> 8192).
