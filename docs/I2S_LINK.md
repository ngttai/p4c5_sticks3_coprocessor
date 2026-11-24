# I2S link protocol between the S3 and the P4

English | [Tiếng Việt](vi/I2S_LINK.md)

Implemented in `cp_s3/main/audio_fe.c` (S3 side) and `phone_s3/components/esp32p4c5_bsp/src/esp32p4c5_audio.c` (P4 side). A standalone test build is `host_p4/main/link_test.c`.

## Format

- **Master:** S3, I2S1, providing BCLK (G43) and WS (G44). **Slave:** P4, I2S0.
- 16 kHz, 16 bit, Philips, stereo (two channels, L and R, in every frame).
- Chunk of 128 frames (8 ms), DMA of 4 descriptors (about 32 ms). Electrical round-trip latency: 79 ms (measured in echo mode).

| Direction | L channel | R channel |
| --- | --- | --- |
| S3 to P4 (pin G4 to GPIO48) | Microphone after the AFE (AEC, NS) | 16-bit counter, +1 every frame |
| P4 to S3 (pin GPIO49 to G3) | Mono 16 kHz playback | Magic `0xA55A` when there is playback data, `0` otherwise |

## Rules

1. **Counter:** the P4 compares each frame's counter with the expected value. A mismatch increments `ctr_err` (shown on the phone's About page). One error at start-up is normal.
2. **Magic:** the S3 treats a chunk as valid when more than 3/4 of its frames carry `0xA55A` in the R channel. An invalid chunk is replaced with silence. This stops noise when pin G3 is floating or the P4 is not running yet.
3. **Amplifier:** the S3 switches the AW8737 amplifier on (through the PMIC) when a chunk is valid and its peak is above 30; it switches it off after 2 seconds without signal.
4. **Microphone buffer on the P4:** `link_task` writes to the microphone stream buffer only when there is room for a **whole** 256-byte block. A FreeRTOS stream buffer accepts a partial write when nearly full; an odd number of bytes shifts every later sample by one byte (full-scale noise, samples that are multiples of 256). If full, the whole block is dropped and `mic_overrun` is incremented.
5. **Open and close:** opening a device (`d_enable`) resets its stream buffer. Closing the speaker waits up to 1 second for the queue to drain, then 80 ms more, so a short beep is not cut off.

## P4 side

- Virtual codec: `bsp_audio_codec_speaker_init()` accepts mono 16-bit audio at any rate. The `esp_ae_rate_cvt` resampler converts it to 16 kHz; its output buffer must be large enough for `esp_ae_rate_cvt_get_max_out_sample_num`.
- Volume: software, through esp_codec_dev. The curve is set in `bsp_audio_codec_speaker_init()`: −24 dB at 1% to −8 dB at 100% (0 is mute).
- `link_task` has priority 18, runs on core 1 with its stack in PSRAM, and logs every 5 seconds (`frames`, `ctr_err`, `play_underrun`, `mic_overrun`, `spk`, `mic`, `play_q`).

## S3 side

- `link_task` (core 0, priority 10): reads and writes 128-frame chunks; pushes playback into `rxq` and takes clean microphone audio from `txq`.
- `codec_task` (core 1): writes playback to the ES8311, reads the microphone and the reference (R channel of the DAC loopback), and feeds the AFE (`"MR"`).
- `fetch_task` (core 1): takes the AFE output into `txq`.
- ES8311 fixed level is 100; microphone gain is 24 dB.

## Link test commands (host_p4)

- `linktest [seconds] [tone]`: the P4 acts as slave and counts frames, printing the L-channel RMS and peak and the counter errors every second; it ends with `LINKTEST RESULT`. `tone` sends a 1 kHz tone to the S3.
- `linklat [n]`: sends a click pulse and measures how long it takes to come back. The electrical measurement needs `cp_s3` built with `AUDIO_FE_LINK_ECHO`; measuring through the speaker and microphone does not work because the microphone floor is noisy (see the phase 5 log).

## Common failures

| Symptom | Usual cause |
| --- | --- |
| Speaker plays noise or stays silent | Pin G3 floating (no magic), or the amplifier never turned on (peak not above 30) |
| Full-scale microphone noise after the wake word | The microphone buffer wrote a partial odd number of bytes (fixed in this version) |
| `ctr_err` keeps increasing | P4 and S3 out of frame sync (one side reset), or long or noisy I2S wires |
| `play_underrun` increases | The P4 is busy and cannot push playback in time |
