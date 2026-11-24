# Phase 5: hosted SPI + AFE FD HIGH + I2S link together on the S3

English | [Tiếng Việt](vi/2026-11-23_phase5_integration.md)

Date 2026-11-23. IDF 6.1, esp_hosted 3.1.0, esp-sr 2.6.2, SPI 26 MHz, S3 at 240 MHz, AFE_TYPE_FD + HIGH_PERF, input "MR".
Firmware: `cp_s3/main/audio_fe.c` (ES8311 + AFE + I2S1 master to the P4), `host_p4/main/link_test.c` (command `linktest [seconds] [tone]`, I2S slave).
I2S wires: P4 GPIO46 BCLK, 47 WS, 48 clean microphone, 49 playback; S3 G43, G44, G4, G3. S3 -> P4 frame: L = microphone (after the AFE), R = 16-bit counter.
P4 -> S3 frame: L = playback, R = magic 0xA55A (the S3 ignores frames without the magic, so a floating G3 does not make the speaker play noise).
Cores: feed + fetch tasks (AFE) pinned to core 1, I2S link pinned to core 0, hosted/WiFi on core 0.

## Results

I2S link (S3 master, 16 kHz): exactly 16 kHz (1,152,000 frames in 72 s), counter errors = 1 per run (when the P4 starts receiving),
0 timeouts, rx_overrun = 0; no drift between the two I2S clock domains, I2S0 (codec) and I2S1 (link), in runs of 52 to 72 s.

AEC through the link (the P4 plays a 1 kHz tone to the S3, the speaker plays it, the microphone records it back): the clean microphone at the P4 has an L RMS of about 85 (close to the 80 to 100 floor when silent),
against 505 to 517 with the AFE bypassed. The tone is removed down to the floor.

TCP throughput at the same time as the tone over the link (26 MHz), against the phase 3 baseline (no audio): RX 12.8 and TX 14.3 Mbps.

| cp_s3 variant | RX PC->P4 | TX P4->PC | S3 minimum internal RAM | Comment |
| --- | --- | --- | --- | --- |
| Default WiFi buffers (dynamic RX 32, dynamic TX 32, static RX 10), WiFi/LWIP in internal RAM | 11.7 Mbps (-9%) | 12.1 Mbps (-16%) | 824 B | Internal RAM nearly exhausted, unusable |
| As above + WiFi/LWIP preferring PSRAM | 11.9 Mbps | 6.3 Mbps, falling from 9.8 to 3.6 | 39.9 KB | TX congested, rejected |
| Small WiFi buffers (dynamic RX 16, dynamic TX 16, static RX 8) | 10.5 Mbps (-18%) | 10.6 Mbps (-26%) | 39.8 KB | Configuration in use |

Assessment against the plan:
- Throughput drop with the AFE running at most 10%: NOT MET with the configuration in use (18 to 26% drop), but still above the absolute 8 Mbps threshold (2.6 times the 4 Mbps video bitrate). The phase 3 baseline also swings widely (TX from 10.5 to 16 Mbps per minute), and each new variant was measured once.
- Minimum internal RAM at least 40 KB: nearly met, 39.8 KB.
- I2S link without underrun or wrong magic: met over 52 to 72 s (1 hour not yet measured).
- S3 core 1 CPU (AFE): not measured in the integrated firmware; about 38% in phase 4.
- Not measured: core 0 CPU with traffic, microphone -> P4 latency, 1 hour, BLE, decoding FPS on the P4.

Configuration changes made in cp_s3 (all in `sdkconfig.defaults*`):
- Console moved to USB-Serial/JTAG (UART0 G43/G44 are taken by the I2S link).
- Octal 8 MB PSRAM; the `codec`, `afe_fetch` and `fe_stats` tasks have their stacks in PSRAM; `SPIRAM_MALLOC_ALWAYSINTERNAL` = 2048.
- Dedicated partition table `partitions_cp_s3.csv` (two 3.5 MB OTA partitions); the app is 1.64 MB.
- NVS is initialized before `audio_fe_start()`: otherwise `esp_wifi_init`, called by the P4 through RPC, can lose a race with NVS and the P4 falls into a reboot loop (seen once).

## Latency and CPU (same day, after changing the link to 128-frame chunks = 8 ms, DMA 4 x 8 ms)

Electrical link latency (cp_s3 in `AUDIO_FE_LINK_ECHO` mode: the S3 returns the P4's playback, the P4 sends a click of 24000 and measures): 10 runs all gave
**79 ms** (no variation), there and back. One direction S3 -> P4 is about 40 ms (DMA 32 ms + 8 ms chunk). Measuring with real sound
(P4 -> speaker -> microphone -> P4) does not work: the microphone floor peaks at 1500 to 2800 while the click rises to only about 4400, so detection is wrong.

AFE latency (measured by the S3 itself: from the moment the 512-sample chunk has been read from the codec to the moment `fetch()` returns the matching samples):
average 10.5 ms when idle, 15 to 16 ms with iperf RX (maximum 22 ms).

Microphone -> P4 estimate (not measured directly, sum of components): collecting a full 32 ms chunk (average 16 ms, maximum 32 ms) + AFE 11 to 16 ms
(maximum 22 ms) + S3 -> P4 link about 40 ms = about 67 to 72 ms on average, about 95 ms worst case. Plan threshold 80 ms:
met on average, tight in the worst case. About 16 ms could be saved by reducing the link DMA from 4 to 2 descriptors.

S3 CPU (FD HIGH + link tone + iperf TCP RX for 40 s):

| Condition | Core 0 (hosted, WiFi, I2S link) | Core 1 (AFE feed + fetch) |
| --- | --- | --- |
| Idle, audio_fe only | 15% | 33% |
| iperf RX 10 Mbps | 37 to 47% | 52 to 55% |

Both are within the plan thresholds (core 1 at most 60%, core 0 at most 70%).

Internal RAM: in this run `min free` fell to **12 KB** under traffic (about 55 KB when idle), no longer the 39.8 KB of the earlier run.
The 40 KB threshold is not met; it is unclear why it differs from before (possibly the chunk/DMA change, or variation with TCP traffic). It needs investigating before the soak.

## Internal RAM fix on the S3 (same day)

Main cause: the two audio_fe stream buffers (txq and rxq, 8192 samples = 16 KB each) were in internal RAM, plus the 8 KB stack of each hosted task
(raised from 4096 to fix the `pserial_task` overflow). Minimum internal RAM under traffic fell to 10 to 15 KB.

Experiments (each measured over 40 s of TCP + link tone, minimum RAM during the run):

| Change | Minimum internal RAM | Result |
| --- | --- | --- |
| Hosted SPI queue 20 down to 10 | about 15 KB | No RAM improvement, worse throughput; reverted |
| WiFi IRAM_OPT and RX_IRAM_OPT off | not measured | RX throughput dropped to 4 to 6 Mbps; reverted |
| Hosted task stack 8192 down to 6144 | RX 31 KB, TX 9.8 KB | Free RAM up 12 KB, WiFi connection still fine (at 4096 `pserial_task` overflows) |
| Stream buffers to PSRAM (`xStreamBufferCreateWithCaps`) | **TX 41.8 KB, RX 40.4 KB** | Meets the 40 KB threshold; free RAM when idle about 103 KB |

Final configuration: hosted stack 6144, both stream buffers in PSRAM, WiFi/hosted unchanged (WiFi IRAM_OPT on, SPI queue 20).
TCP throughput at the same time as the AFE and link tone (40 s, one run per direction): RX average 11.0 Mbps (9.5 to 12.3), TX average 8.7 Mbps (6.2 to 11.7).

Note on throughput noise: repeating the same RX firmware gave 7.7, 11 and 9.9 Mbps. Differences between configurations below about 2 Mbps
are not statistically meaningful (the PC and the AP both use WiFi). Longer runs and a wired PC are needed for an exact comparison.
