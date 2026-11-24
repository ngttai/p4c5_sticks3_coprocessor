/*
 * Minimal stand-in for core/gfx_types.h of esp_emote_gfx: only what the EAF decoder needs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef union {
    uint16_t full;      /*!< RGB565 */
} gfx_color_t;

static inline uint16_t gfx_color_to_native_u16(gfx_color_t color, bool swap_bytes)
{
    return swap_bytes ? (uint16_t)((color.full << 8) | (color.full >> 8)) : color.full;
}
