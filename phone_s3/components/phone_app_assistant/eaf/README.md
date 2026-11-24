# EAF decoder

`gfx_eaf_dec.c` and `gfx_eaf_dec.h` are the EAF animation decoder of
[espressif2022/esp_emote_gfx](https://github.com/espressif2022/esp_emote_gfx) 3.0.5 (`src/lib/eaf`), unchanged,
under the Apache License 2.0 (see `LICENSE`).

`core/gfx_types.h` and `common/gfx_log_priv.h` are small stand-ins for the two headers of that library the
decoder includes, so that it builds without the rest of the library. The JPEG and Heatshrink block decoders
are left out (their Kconfig options are not defined here): the emote pack only uses RLE blocks.
