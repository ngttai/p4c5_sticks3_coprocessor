# Architecture

English | [Tiếng Việt](vi/ARCHITECTURE.md)

The P4 (KSDIY P4C5) is the host: it runs the UI, the apps and wake-word detection. The M5StickS3 (ESP32-S3) is the co-processor: it gives the P4 WiFi over SPI and cleans the microphone with the AFE (AEC, noise suppression, VAD), returning the result over I2S. The ESP32-C5 on the P4C5 is unused (its EN pin, GPIO54, is held low). Terms such as AFE, AEC and ESP-Hosted are explained in the [Glossary](#glossary) at the end.

## Block diagram

```mermaid
flowchart LR
    subgraph P4["KSDIY P4C5 (ESP32-P4, host)"]
        UI["LVGL + Brookesia<br/>Settings, Music, Radio, Assistant"]
        WN["WakeNet 'Hi ESP'<br/>(esp-sr on the P4)"]
        NET["esp_hosted host 3.1.0<br/>lwIP, remote WiFi API"]
        BSPA["esp32p4c5_audio.c<br/>virtual codec + I2S slave link"]
        UI --> NET
        UI --> BSPA
        WN --> UI
        BSPA --> WN
    end

    subgraph S3["M5StickS3 (ESP32-S3, co-processor)"]
        CP["esp_hosted cp 3.1.0<br/>2.4 GHz WiFi"]
        FE["audio_fe.c<br/>link_task / codec_task / fetch_task"]
        AFE["esp-sr AFE FD HIGH<br/>AEC + NS + VAD"]
        ES["ES8311<br/>mic + speaker + DAC loopback"]
        PM["PMIC M5PM1<br/>L3B rail, AW8737 amplifier"]
        FE --> AFE
        FE --> ES
        FE --> PM
    end

    AP["2.4 GHz WiFi router"]
    SPK["Speaker"]
    MIC["Microphone"]

    NET <-->|"SPI full-duplex 26 MHz<br/>CLK MOSI MISO CS HS DR + RESET"| CP
    BSPA <-->|"I2S 16 kHz 16-bit stereo<br/>BCLK WS mic playback"| FE
    CP <--> AP
    ES --> SPK
    MIC --> ES
```

## Data flow

Two paths run at the same time. Both cross the I2S link in 8 ms chunks (128 frames); the S3 is the I2S master.

### Microphone: from the ES8311 to the assistant

```mermaid
sequenceDiagram
    participant MIC as ES8311 (mic + DAC loopback)
    participant CT as S3 codec_task (core 1)
    participant AFE as S3 AFE FD HIGH (inside feed/fetch)
    participant LS as S3 link_task (core 0)
    participant LP as P4 link_task
    participant AS as P4 Assistant (WakeNet, Opus)

    loop every 32 ms chunk (512 samples)
        MIC->>CT: L = mic, R = DAC reference
        CT->>AFE: feed(mic, ref)
        AFE->>LS: fetch: clean mic, AEC + NS + VAD (txq in PSRAM)
        LS->>LP: I2S frame, L = clean mic, R = 16-bit counter
        LP->>LP: check counter (ctr_err), write whole 256-byte blocks
        LP->>AS: mic stream buffer, 16 kHz mono
    end
    Note over AFE: the reference is what the speaker plays, so the AFE subtracts the echo from the mic
```

### Speaker: from an app to the ES8311

```mermaid
sequenceDiagram
    participant APP as P4 app (music, radio, beep, TTS)
    participant VOL as P4 esp_codec_dev (software volume)
    participant RS as P4 resampler
    participant LP as P4 link_task
    participant LS as S3 link_task (core 0)
    participant CT as S3 codec_task (core 1)
    participant SPK as ES8311 DAC, amplifier, speaker

    APP->>VOL: PCM, any sample rate
    VOL->>RS: scaled by the volume curve (-24 to -8 dB)
    RS->>LP: 16 kHz mono (playback stream buffer)
    LP->>LS: I2S frame, L = playback, R = magic 0xA55A
    alt more than 3/4 of the frames carry the magic
        LS->>CT: playback chunk (rxq in PSRAM)
        CT->>SPK: PCM, amplifier on when the peak is above 30
    else no magic (floating G3 or P4 not running)
        LS->>CT: silence
    end
    SPK-->>CT: DAC loopback, the R channel becomes the AFE reference
```

Details of the I2S frame format: [I2S_LINK.md](I2S_LINK.md).

## CPU and memory split on the S3

| Core | Work | Measured load (1-hour soak) |
| --- | --- | --- |
| 0 | WiFi, esp_hosted SPI, I2S `link_task` (priority 10) | average 38%, peak 58.6% |
| 1 | `codec_task` (reads the ES8311 and feeds the AFE), `fetch_task` (takes the AFE output) | average 45.5%, peak 60.2% |

- The esp-sr 2.6.2 V1 interface has no AFE task of its own: AEC and VAD run inside `feed()` and `fetch()`, so those two tasks are pinned to core 1.
- Minimum internal RAM is about 40 KB. The audio_fe stream buffers and the stacks of `codec_task` and `fetch_task` live in PSRAM. The hosted task stack is 6144 and the WiFi buffers are reduced (dynamic RX 16, dynamic TX 16, static RX 8).
- The S3 has a single-precision FPU only: avoid `double` in hot loops.

## P4 side

- **WiFi:** `esp32p4c5_wifi.c` pulls GPIO54 (the C5's EN) low, then connects to the S3 with esp_hosted. WiFi 2.4 GHz only, no BLE.
- **Audio:** `esp32p4c5_audio.c` provides `bsp_audio_codec_speaker_init()` and `bsp_audio_codec_microphone_init()` like the original BSP, but the device behind them is a virtual codec (no `codec_if`) that uses the esp_codec_dev software volume. The speaker accepts mono PCM at any rate and downsamples it to 16 kHz.
- **Assistant:** `assistant_afe_open()` returns `false` when `CONFIG_BSP_AUDIO_S3_LINK=y`: no AFE runs on the P4. Wake word and the Opus encoder read the clean microphone from the link.
- **S3 info:** Settings, About shows the link counters (frame errors or losses, speaker underruns).

## Glossary

| Term | Meaning |
| --- | --- |
| **esp-sr** | Espressif's speech recognition library (version 2.6.2 here). It contains the AFE and WakeNet. |
| **AFE** (Audio Front-End) | The audio processing stage between the microphone and the application. It takes a microphone signal and a reference signal and returns one cleaned microphone channel. |
| **Input format `"MR"`** | The AFE input layout: **M** = microphone, **R** = reference (a copy of what the speaker plays). |
| **Reference (ref)** | The signal the speaker is playing, given to the AFE so it can subtract the echo. Here it is the ES8311 DAC loopback on the R channel. |
| **AFE type SR / FD / VC** | Three esp-sr AFE variants. **SR** (Speech Recognition): light, weak echo removal. **FD** (Full-Duplex): talk and listen at the same time, used here. **VC** (Voice Communication): adds neural-network noise suppression, the heaviest on CPU. |
| **Mode LOW_COST / HIGH_PERF** | Resource trade-off of an AFE type. **HIGH_PERF** is better quality and costs more CPU. "FD HIGH" = Full-Duplex type in HIGH_PERF mode (about 38% of one S3 core). |
| **AEC** (Acoustic Echo Cancellation) | Removes the speaker sound that comes back into the microphone, using the reference. It lets the microphone hear you while the assistant is speaking. |
| **ERLE** | Echo Return Loss Enhancement: how many dB of echo the AEC removes. |
| **NS** (Noise Suppression) | Reduces steady background noise such as fans or room noise. |
| **VAD** (Voice Activity Detection) | Detects when someone is speaking. Used here for measurement only. |
| **Far-end / near-end** | Far-end = the sound played by the speaker (the other party). Near-end = the person at the microphone. |
| **Double-talk** | Both ends speak at once. A good AEC removes the far-end but keeps the near-end voice. |
| **Barge-in** | Interrupting the assistant while it is speaking. It needs AEC so the assistant does not hear itself. |
| **WakeNet** | The esp-sr wake-word detector ("Hi ESP"). It runs on the P4 on the clean microphone from the S3. |
| **Opus** | The audio codec used to send speech to the assistant server. |
| **TTS** | Text-to-speech: synthetic speech, used here as a test signal and as the assistant's voice. |
| **ES8311** | The audio codec chip on the StickS3 (microphone input, DAC output, loopback). |
| **PMIC** | Power-management IC (M5PM1 on the StickS3). It switches the audio rail and the speaker amplifier. |
| **ESP-Hosted** | Espressif's host and co-processor framework: the P4 (host) uses the WiFi of the S3 (co-processor) as if it were local. Version 3.1.0 on both sides. |
| **Host / co-processor (cp)** | Host = the P4 running the application. Co-processor = the S3 providing WiFi and audio processing. |
| **RPC** | Remote procedure call: how the P4 calls WiFi functions that run on the S3 through ESP-Hosted. |
| **SPI full-duplex** | The WiFi link between the two boards: clock, MOSI, MISO and CS, plus handshake, data-ready and reset lines. |
| **I2S master / slave** | The master supplies the clock. The S3 is master (BCLK and WS); the P4 is slave. |
| **BCLK / WS** | I2S bit clock and word select (left/right frame clock). |
| **Strapping pin** | A pin the chip reads at boot to choose its mode. G3 of the S3 is one, so it is used as an input only. |
| **PSRAM** | External RAM. The S3 keeps large buffers and some task stacks there to save internal RAM. |
| **Magic 0xA55A** | The marker the P4 puts in the R channel of valid playback frames; the S3 ignores frames without it. |
