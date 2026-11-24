# afe_s3

[Tiếng Việt](README.md) | English

A standalone AFE test on the StickS3 (no WiFi, no I2S link), used to choose the AFE configuration in phase 4. Results: [../docs/logs/2026-11-23_phase4_afe_standalone.md](../docs/logs/2026-11-23_phase4_afe_standalone.md) (in Vietnamese).

Select the test with `TEST_MODE` in `main/main.c`:
- `1`: 1 kHz tone, cycling through SR LOW, FD LOW, FD HIGH, VC LOW and a no-AEC control.
- `2`: repeated synthetic speech (flite), same five configurations.
- `3`: double-talk. Each round waits for KEY1 and lasts 20 s: three beeps, far-end speech from second 3, a single beep at second 8 means start talking, another at second 15 means stop; `ALL DONE` is printed at the end. Log it with `../tools/cap_dt.py`.

Build: `idf.py set-target esp32s3 && idf.py build && idf.py -p <S3 port> flash`.
