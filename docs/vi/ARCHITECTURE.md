# Kiến trúc

[English](../ARCHITECTURE.md) | Tiếng Việt

P4 (KSDIY P4C5) là host chạy giao diện, ứng dụng và wake word. M5StickS3 (ESP32-S3) là co-processor: WiFi cho P4 qua SPI, và làm sạch mic bằng AFE (AEC, khử nhiễu, VAD) rồi trả lại qua I2S. ESP32-C5 trên P4C5 không dùng (EN kéo thấp ở GPIO54).

## Sơ đồ khối

```mermaid
flowchart LR
    subgraph P4["KSDIY P4C5 (ESP32-P4, host)"]
        UI["LVGL + Brookesia<br/>Settings, Music, Radio, Assistant"]
        WN["WakeNet 'Hi ESP'<br/>(esp-sr trên P4)"]
        NET["esp_hosted host 3.1.0<br/>lwIP, WiFi API từ xa"]
        BSPA["esp32p4c5_audio.c<br/>codec ảo + link I2S slave"]
        UI --> NET
        UI --> BSPA
        WN --> UI
        BSPA --> WN
    end

    subgraph S3["M5StickS3 (ESP32-S3, co-processor)"]
        CP["esp_hosted cp 3.1.0<br/>WiFi 2,4 GHz"]
        FE["audio_fe.c<br/>link_task / codec_task / fetch_task"]
        AFE["esp-sr AFE FD HIGH<br/>AEC + NS + VAD"]
        ES["ES8311<br/>mic + loa + DAC loopback"]
        PM["PMIC M5PM1<br/>nguồn L3B, ampli AW8737"]
        FE --> AFE
        FE --> ES
        FE --> PM
    end

    AP["Router WiFi 2,4 GHz"]
    SPK["Loa"]
    MIC["Mic"]

    NET <-->|"SPI full-duplex 26 MHz<br/>CLK MOSI MISO CS HS DR + RESET"| CP
    BSPA <-->|"I2S 16 kHz 16-bit stereo<br/>BCLK WS mic playback"| FE
    CP <--> AP
    ES --> SPK
    MIC --> ES
```

## Luồng dữ liệu

Hai đường chạy cùng lúc. Cả hai đi qua link I2S theo khối 8 ms (128 khung); S3 là master của I2S.

### Mic: từ ES8311 tới trợ lý

```mermaid
sequenceDiagram
    participant MIC as ES8311 (mic + DAC loopback)
    participant CT as S3 codec_task (core 1)
    participant AFE as S3 AFE FD HIGH (trong feed/fetch)
    participant LS as S3 link_task (core 0)
    participant LP as P4 link_task
    participant AS as P4 Assistant (WakeNet, Opus)

    loop mỗi khối 32 ms (512 mẫu)
        MIC->>CT: L = mic, R = tham chiếu DAC
        CT->>AFE: feed(mic, ref)
        AFE->>LS: fetch: mic sạch, AEC + NS + VAD (txq ở PSRAM)
        LS->>LP: khung I2S, L = mic sạch, R = bộ đếm 16 bit
        LP->>LP: kiểm bộ đếm (ctr_err), ghi nguyên khối 256 byte
        LP->>AS: stream buffer mic, 16 kHz mono
    end
    Note over AFE: tham chiếu chính là tiếng loa đang phát, nên AFE trừ tiếng vọng khỏi mic
```

### Loa: từ ứng dụng tới ES8311

```mermaid
sequenceDiagram
    participant APP as P4 app (nhạc, radio, bíp, TTS)
    participant VOL as P4 esp_codec_dev (volume phần mềm)
    participant RS as P4 resampler
    participant LP as P4 link_task
    participant LS as S3 link_task (core 0)
    participant CT as S3 codec_task (core 1)
    participant SPK as ES8311 DAC, ampli, loa

    APP->>VOL: PCM, tần số lấy mẫu bất kỳ
    VOL->>RS: nhân theo đường cong âm lượng (-24 đến -8 dB)
    RS->>LP: 16 kHz mono (stream buffer playback)
    LP->>LS: khung I2S, L = playback, R = magic 0xA55A
    alt hơn 3/4 số khung có magic
        LS->>CT: khối playback (rxq ở PSRAM)
        CT->>SPK: PCM, ampli bật khi đỉnh lớn hơn 30
    else không có magic (G3 hở hoặc P4 chưa chạy)
        LS->>CT: im lặng
    end
    SPK-->>CT: DAC loopback, kênh R thành tham chiếu cho AFE
```

Chi tiết định dạng khung I2S: [I2S_LINK.md](I2S_LINK.md).

## Phân chia CPU và bộ nhớ trên S3

| Core | Việc | Tải đo được (soak 1 giờ) |
| --- | --- | --- |
| 0 | WiFi, esp_hosted SPI, `link_task` I2S (ưu tiên 10) | trung bình 38%, đỉnh 58,6% |
| 1 | `codec_task` (đọc ES8311 và feed AFE), `fetch_task` (lấy ngõ ra AFE) | trung bình 45,5%, đỉnh 60,2% |

- esp-sr 2.6.2 giao diện V1 không có task AFE riêng, AEC và VAD chạy ngay trong `feed()` và `fetch()`, nên hai task này được ghim vào core 1.
- RAM nội tối thiểu khoảng 40 KB. Stream buffer của audio_fe và stack của `codec_task`, `fetch_task` đặt ở PSRAM. Stack task hosted là 6144, buffer WiFi đã giảm (RX động 16, TX động 16, RX tĩnh 8).
- S3 chỉ có FPU đơn: không dùng `double` trong vòng lặp nóng.

## Phía P4

- **WiFi:** `esp32p4c5_wifi.c` kéo GPIO54 (EN của C5) xuống thấp, rồi kết nối S3 bằng esp_hosted. Chỉ có WiFi 2,4 GHz, không BLE.
- **Âm thanh:** `esp32p4c5_audio.c` cung cấp `bsp_audio_codec_speaker_init()` và `bsp_audio_codec_microphone_init()` như BSP gốc, nhưng thiết bị phía sau là codec ảo (không có `codec_if`), dùng software volume của esp_codec_dev. Loa nhận PCM mono ở tần số bất kỳ và được hạ mẫu xuống 16 kHz.
- **Assistant:** `assistant_afe_open()` trả `false` khi `CONFIG_BSP_AUDIO_S3_LINK=y`: không chạy AFE trên P4. Wake word và bộ mã hóa Opus đọc mic sạch từ link.
- **Thông tin S3:** trang Cài đặt, Giới thiệu hiện các bộ đếm link (khung lỗi hoặc mất, underrun loa).
