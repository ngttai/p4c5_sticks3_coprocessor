#pragma once
#include "esp_err.h"

/* StickS3 audio front-end: ES8311 (mic + digital ref) -> AFE (AEC/VAD/NS, pinned to core 1) -> I2S link to the P4.
 * In the other direction: playback from the P4 over the link -> ES8311 -> speaker (the PMIC amplifier is on only while there is sound). */
esp_err_t audio_fe_start(void);
