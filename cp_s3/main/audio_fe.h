#pragma once
#include "esp_err.h"

/* StickS3 audio front-end: ES8311 (mic + ref số) -> AFE (AEC/VAD/NS, ghim core 1) -> I2S link tới P4.
 * Ngược lại: playback từ P4 qua link -> ES8311 -> loa (ampli PMIC chỉ bật khi có tiếng). */
esp_err_t audio_fe_start(void);
