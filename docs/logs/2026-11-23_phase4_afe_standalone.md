# Phase 4: standalone AFE on the StickS3 (project `afe_s3`)

English | [Tiếng Việt](vi/2026-11-23_phase4_afe_standalone.md)

Date 2026-11-23. IDF 6.1, esp-sr 2.6.2, S3 at 240 MHz, 16 kHz, input "MR" (microphone + digital reference from the ES8311, `no_dac_ref=false`),
microphone gain 24 dB, speaker volume 45, 1 kHz tone at -12 dBFS, 14 s per configuration (tone from second 3 to 11). NS/VAD use WebRTC
(the nsnet2 model is not loaded, so "VC" here is not yet VC with the neural-network NS). The speaker amplifier is on only while the tone plays.

Reference path check: the R channel of the recording has an RMS of 5657 = exactly the tone amplitude divided by the square root of 2, so the ES8311 DAC loopback onto the right channel works.

| Configuration | CPU core 0 (audio + fetch + AFE) | Minimum internal RAM | 1 kHz tone at the AFE output | Mic -> output RMS |
| --- | --- | --- | --- | --- |
| SR LOW | 17.2% | 261 KB | down 72 dB, below the floor | 309 -> 70 (12.9 dB) |
| SR HIGH | 23.8% | 260 KB | 76 dB | 308 -> 60 (14.3 dB) |
| FD LOW | 29.1% | 260 KB | 76 dB | 309 -> 62 (14.0 dB) |
| FD HIGH | 36.7% | 260 KB | 78 dB | 309 -> 63 (13.8 dB) |
| VC LOW | 60.3% | 248 KB | 97 dB | 309 -> 7 (32.8 dB) |
| VC HIGH | 62.8% | 206 KB | 93 dB | 309 -> 11 (28.6 dB) |
| SR LOW, AEC off (control) | 1.6% | 206 KB | 0 dB | 309 -> 309 (0 dB) |

How to read it:
- The tone at the raw microphone is 1275 RMS (volume 70) or 309 (volume 45), while the AFE output is only at the silent noise floor (SR/FD about 62-70 RMS, VC about 8 because of NS). AEC removes the whole tone, down to the microphone noise floor.
- The "ERLE" numbers of 72 to 97 dB come from the amplitude at the 1 kHz bin against the noise floor, so they are only a lower bound, not a real ERLE. The RMS figure is limited by the microphone noise floor (about 62 RMS at 24 dB gain): SR/FD reach only 13-14 dB in full-band RMS even though the tone is completely gone.
- The plan threshold is 20 dB by RMS: only VC meets it, because VC also lowers the floor. It needs re-measuring with real speech (not a tone) and with a lower microphone gain to get meaningful numbers.

Problems to handle:
- (Handled) Core 1 was once 100% idle: the esp-sr 2.6.2 V1 interface does not create its own AFE task; AEC/VAD run inside `feed()`/`fetch()` of the calling task, so `afe_perferred_core` has no effect (no AFE task appeared in the task list mid-run). The fix is to create the feed task (reads the ES8311 + feed) and the fetch task with `xTaskCreatePinnedToCore(..., 1)`.
- Not measured: speech, VAD with voice, nsnet2, microphone -> P4 latency.
- The old speaker hiss came from the amplifier being switched on by earlier firmware (PMIC reg 0x11 bit 3); it is now off in cp_s3 and afe_s3.

## After pinning feed/fetch to core 1 (same day)

| Configuration | CPU core 0 | CPU core 1 |
| --- | --- | --- |
| SR LOW | 0.0% | 16.6% |
| SR HIGH | 0.0% | 23.7% |
| FD LOW | 0.0% | 29.1% |
| FD HIGH | 0.0% | 36.5% |
| VC LOW | 0.1% | 60.7% |
| VC HIGH | 0.1% | 63.2% |

Core 0 is completely free for WiFi, hosted SPI and the I2S link in phase 5. The plan threshold for the AFE core is 60% or lower: SR and FD meet it, VC touches it.
AEC quality is unchanged from before the pinning. These CPU figures include codec reads/writes and tone generation in the audio task, so they are slightly higher than the pure AFE.

## Synthetic speech (far-end only, nobody talking)

Source: 12 seconds of TTS (flite, three voices: slt, kal16, awb), normalized to -14 LUFS (RMS 5761, peak 29202), looped from flash,
speaker volume 45, microphone gain 24 dB, AFE feed/fetch pinned to core 1. Seconds 3 to 14 play speech; nobody speaks. Speech at the microphone is
about 200 to 800 RMS, microphone floor 65 RMS. The reduction is computed on the total microphone and AFE output energy in the playback window.

| Configuration | Reduction, whole window | Reduction after convergence (seconds 7 to 14) | Output floor when silent | CPU core 1 | Comment |
| --- | --- | --- | --- | --- | --- |
| SR LOW | -6.9 dB | 3.4 dB | 65.6 | 17.7% | AEC barely removes the speech; VAD reports "speaking" 100% because of the echo |
| FD LOW | 5.8 dB | 7.4 dB | 66.0 | 30.7% | Weak |
| FD HIGH | 18.8 dB | 26.7 dB | 65.1 | 38.2% | Good: output 1 to 19 RMS, VAD 0% during playback |
| VC LOW | 22.8 dB | 22.1 dB | 16.7 | 62.8% | Good, with NS, but CPU close to the threshold |
| SR LOW, no AEC | 0.0 dB | 0.0 dB | 66.1 | 1.8% | Control |

Interim conclusions:
- The 1 kHz tone is too easy a test: every configuration removes it completely. With speech, only FD HIGH (and VC) cancel it.
- SR_LOW (`NLP_OFF`) cannot be used for full duplex: the echo keeps VAD reporting speech all the time.
- Candidate for phase 5: FD HIGH (about 38% of core 1, minimum internal RAM 259 KB).
- Double-talk was not yet measured (a person talking over the speaker), so it was unknown whether FD HIGH keeps the human voice.
- flite voices differ from real voices in spectrum and rhythm, so this needs re-checking with a real voice through an external speaker.

## Double-talk (a real person talks while the speaker plays speech)

Procedure per round (20 s): three beeps (seconds 1 to 2), the speaker plays TTS speech from second 3 to 16, a single beep at second 8 = start talking,
a single beep at second 15 = stop talking. Far-end-only window = seconds 6 to 7, double-talk = seconds 10 to 14, floor = seconds 17 to 19.
The same person talked, mouth about 15 to 20 cm from the microphone. The human voice is estimated as microphone energy (DT) minus the
echo energy measured in far-end-only, so it is only approximate (the TTS content differs between segments).

| Configuration | Far-end only: mic -> out (RMS) | Double-talk: mic -> out (RMS) | Estimated human voice | out / human voice | VAD while talking | Floor when silent |
| --- | --- | --- | --- | --- | --- | --- |
| FD HIGH | 337 -> 0.2 to 0.4 | 566 -> 249 | 455 | -5.2 dB | 100% | 23 |
| VC LOW | 406 -> 16 | 575 -> 237 | 407 | -4.7 dB | 100% | 13 |
| SR LOW, no AEC (control) | 291 -> 291 | 492 -> 492 | 397 | +1.9 dB | 100% | 40 |

Remarks:
- Both FD HIGH and VC LOW keep the human voice while the speaker plays: the output stays at about 237 to 249 RMS, only about 5 dB below the original voice, and VAD reports speech 100%. Far-end echo is still erased completely with FD HIGH (0.2 to 0.4 RMS when only the speaker plays).
- VAD in far-end-only: FD HIGH 0% in seconds 5 to 8, while VC LOW sometimes reports speech (93% at second 6, 22% at second 8): VC is easily triggered by the speaker.
- The first FD HIGH run was discarded: the output was only 23 RMS during double-talk, but the speaker had not talked at the right time in that window, so it was not used (the log existed only in the working session).
- The "BROADBAND ... out floor" number in this mode is wrong (it includes the 3 beeps at seconds 1 to 2); do not use it. The correct floor is the last column of the table above.
- One person, one run per configuration, one posture. Not measured at a distance, with louder or quieter speech, or with the speaker louder.

Phase 4 conclusion: choose FD HIGH for phase 5 (about 38% of core 1, minimum internal RAM 259 KB, removes speech echo
to below the floor, keeps the human voice, VAD is not falsely triggered by the speaker). VC LOW costs about 63% of core 1 and is easily falsely triggered by the speaker.
