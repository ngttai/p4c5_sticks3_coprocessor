# Results: a StickS3 (ESP32-S3) in place of the C5 for WiFi and AEC on the P4C5

[Tiếng Việt](../RESULTS.md) | English

Updated 2026-11-24. IDF 6.1, esp_hosted 3.1.0, esp-sr 2.6.2. Per-phase measurements: [../logs/](../logs/) (in Vietnamese).

## Summary

The S3 works as a co-processor for the P4C5, doing two jobs at once: **2.4 GHz WiFi over SPI** and **AEC plus noise suppression for the microphone over I2S**. A real phone build runs on this setup (WiFi, music, radio, the voice assistant with wake word, network test).
In exchange, BLE, 5 GHz WiFi and music quality are lost (the link is 16 kHz mono). Some plan criteria were missed or not measured, see the sections below.

## Gained

| Item | Result | Source |
| --- | --- | --- |
| WiFi over hosted SPI at 26 MHz | TCP about 10 to 13 Mbps (10-minute soak: RX 12.8, TX 14.3). On the phone: download 11.7, upload 8.4 Mbps | phase 3 logs, phone |
| UDP | Saturates at about 20 Mbps (TX) and 17 Mbps (RX) | phase 3 logs |
| AEC on the S3 (FD HIGH) | Loudspeaker speech (far-end only): 26.7 dB reduction after convergence, output close to zero. With double-talk the near-end voice is kept, about 5 dB weaker, VAD correct | phase 4 logs |
| I2S link S3 to P4 | 16 kHz, 1-hour soak: 29 million frames per direction, 1 counter error (at start), 0 timeouts, no clock drift | phase 5 soak log |
| S3 CPU with everything running | Core 0 (WiFi, hosted, link): average 38%, peak 58.6%. Core 1 (AFE): average 45.5%, peak 60.2% | phase 5 soak log |
| AFE lag | Average 13 ms, maximum 26 ms | phase 5 soak log |
| Electrical link latency (P4 to S3 and back) | 79 ms round trip, constant | phase 5 log |
| Phone running on the S3 | WiFi, music, radio, assistant (wake word "Hi ESP", conversation), network test, an About page showing S3 info | phone_s3 |
| Off the P4 | The AFE no longer runs on the P4 when the S3 is used (the microphone is already clean) | phone_s3 |

## Lost

- **BLE:** not available (hosted HCI is not used).
- **5 GHz and WiFi 6:** the S3 only has WiFi 4, 2.4 GHz.
- **Music quality:** the link is 16 kHz mono, so everything above 8 kHz and stereo are lost. 44.1 and 48 kHz audio is downsampled to 16 kHz.
- **Barge-in through the P4's AEC:** off. AEC runs only on the S3; it cleans the microphone, but the P4 does not use it to interrupt the assistant.
- **Volume:** software volume on the P4 only (the phone cannot control the ES8311 gain).
- **KSDIY features removed from the phone_s3 build:** 4G, camera, USB camera, video, gallery, file manager, weather, second screen, calculator, game, level.

## Known limits

- **Throughput drops 18 to 26%** against the phase 3 baseline (no audio) when the AFE and link run together. The plan criterion was at most 10%: **not met**. It is still above the absolute 8 Mbps threshold (2.6 times a 4 Mbps video bitrate).
- **Throughput numbers are noisy:** differences below about 2 Mbps between runs are not meaningful, because the PC and the access point also use WiFi.
- **S3 internal RAM is tight:** about 40 KB minimum (plan threshold 40 KB). It was reached by moving the stream buffers and task stacks to PSRAM, lowering the hosted stack to 6144 and reducing the WiFi buffers. Adding features on the S3 will easily break it.
- **Core 1 touches the 60% threshold** at its peak (AFE FD HIGH).
- **Microphone-to-P4 latency was not measured directly.** Estimated at 67 to 72 ms on average and about 95 ms worst case (plan threshold 80 ms). About 16 ms can be saved by reducing the link DMA from 4 descriptors to 2.
- **StickS3 pin G3 is a strapping pin:** the playback link uses it as an input only, so the S3 ignores frames without the magic value (0xA55A); otherwise a floating pin plays noise.
- **Reset through the serial port:** some ways of opening the serial port (`idf.py flash` or `monitor`, tools that drive DTR/RTS) reset the board; resetting the P4 also resets the S3 through GPIO50. Opening the port without driving DTR/RTS (the scripts in `tools/`, `cat` after `stty`) did not reset in the runs done here.
- **Noisy microphone at 24 dB gain:** noise floor about 62 RMS, which limits RMS-based measurements; real voice at several distances still needs measuring.
- **The StickS3 speaker is small:** it needs its own volume curve (−24 to −8 dB), a fixed S3 level of 100 and a beep amplitude of 24000 to be audible at low settings.

## Not done

- **Baseline P4 + C5 over SDIO** (phases 1 and 2): not measured, so there is no direct comparison with the C5.
- **BLE** together with WiFi (phase 7).
- **P4 FPS and CPU** when decoding video through the S3 (phase 6): not measured; the video app was removed from the phone_s3 build. The P4 CPU recovered by moving the AFE to the S3 has no number.
- **8-hour soak** (only 1 hour was run).
- **Wired PC** to rule out the PC-side WiFi bottleneck when measuring throughput.
- **I2S link under real phone load** (LVGL, camera, flash writes): not measured. Theoretical risk: the P4 link task keeps its stack in PSRAM and could stall during flash writes, while the DMA buffer is only about 32 ms.
- **AEC while the assistant is speaking** (barge-in): not measured with real voice.
- **Neural noise suppression (nsnet2):** not loaded, so the VC configuration is not the full VC.
- **S31 (Mosaico)** and Bluetooth audio (A2DP): not tried; the S3 has no Bluetooth Classic.
- **H.265** on the P4: no official decoder found.
- **S3 RAM and CPU readout** on the About page: needs the hosted AUX channel.

## Problems found and how they were handled

| Problem | Cause | Fix |
| --- | --- | --- |
| `pserial_task` stack overflow on the S3 | Hosted stack of 4096 | Raised to 8192, later lowered to 6144 to save RAM |
| Task watchdog on the S3 while running the AFE | Double-precision trig on the S3 (single-precision FPU only) | Use float and a 16-entry table |
| AFE not running on core 1 | esp-sr V1 has no task of its own; it runs inside `feed()` and `fetch()` | Pin the feed and fetch tasks to core 1 |
| S3 ran at 160 MHz | Old Kconfig name ignored by IDF 6.1 | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` |
| P4 reboot loop (`ESP_ERR_NVS_NOT_INITIALIZED`) | `audio_fe` ran before NVS was initialized on the S3 | Initialize NVS and the event loop before `audio_fe_start()` |
| Speaker hiss and howl | Amplifier left on by older firmware; floating G3 produced noise | Switch the amplifier off through the PMIC; add the 0xA55A magic |
| S3 internal RAM exhausted (824 B) | 32 KB of stream buffers and large stacks in internal RAM | Stream buffers and task stacks to PSRAM |
| Beep inaudible | Closing the codec dropped the queued PCM | Wait for the queue to drain before closing |
| Music and radio silent, seconds counter too fast | Resampler output buffer too small | Size it with `get_max_out_sample_num` |
| Wake word heard but the assistant did not receive speech | `xStreamBufferSend` wrote an odd number of bytes when the microphone buffer was full, shifting every later sample by one byte | Write whole 256-byte blocks only |
| Low volume inaudible | The default −50 to 0 dB curve is too steep for a small speaker | −24 to −8 dB curve, higher S3 level and beep amplitude |
