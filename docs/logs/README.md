# Measurement logs

English | [Tiếng Việt](vi/README.md)

Read them in chronological order; each file keeps the numbers as they were at the time, including configurations that were replaced later.

| File | Contents | Note |
| --- | --- | --- |
| `2026-11-23_phase3_spi_sweep.md` | SPI clock sweep 10, 20, 26, 40 MHz | 26 MHz chosen |
| `2026-11-23_phase3_udp_soak.md` | UDP and 10-minute soak | The speaker hiss noted here was later traced to the amplifier |
| `2026-11-23_phase4_afe_standalone.md` | Standalone AFE: CPU, AEC with tone, speech, double-talk | FD HIGH chosen |
| `2026-11-23_phase5_integration.md` | Hosted + AFE + link together; latency, CPU, internal RAM fix | The RAM and throughput variants in the middle are experiments that were replaced; the final configuration is under "Internal RAM fix on the S3" |
| `2026-11-23_phase5_soak_1h.md` | 1-hour soak of the final configuration | Final numbers for the S3 firmware |

Summary and conclusions: [../RESULTS.md](../RESULTS.md). The scripts that produced these logs: [../../tools/](../../tools/).
