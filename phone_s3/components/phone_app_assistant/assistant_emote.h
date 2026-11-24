/*
 * Animated face of the assistant: EAF eye animations picked by the emotion name of the Xiaozhi
 * protocol ("happy", "thinking", ...). Built-in set: Espressif's Emote Pack (360x360); a card can override
 * any of them with appdata/assistant/emote/ on the card,<emotion>.eaf or pack_<name>.eaf. All functions run in the LVGL task.
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASSISTANT_EMOTE_SIZE 360

/** Create the face (ASSISTANT_EMOTE_SIZE square) in parent; background is the color drawn behind the eyes. */
lv_obj_t *assistant_emote_create(lv_obj_t *parent, uint32_t background);
/** Play the animation of an emotion; unknown names play "neutral". The same emotion keeps playing. */
void assistant_emote_set(const char *emotion);
/** Free the frame buffer and the decoder; the LVGL object and the timer are deleted with the app. */
void assistant_emote_destroy(void);

#ifdef __cplusplus
}
#endif
