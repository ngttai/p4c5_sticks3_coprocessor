/*
 * Minimal stand-in for common/gfx_log_priv.h of esp_emote_gfx: the EAF decoder logs through ESP_LOG.
 */
#pragma once

#include "esp_log.h"

#define GFX_LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#define GFX_LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
#define GFX_LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
