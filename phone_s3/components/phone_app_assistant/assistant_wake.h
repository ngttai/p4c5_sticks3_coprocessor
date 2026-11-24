/*
 * Wake word ("Hi ESP", WakeNet 9 from esp-sr): fed with 16 kHz mono PCM while the assistant is idle.
 * The model is read from the "model" partition.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "model_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the model; false when the partition or the model is missing. */
bool assistant_wake_open(void);
void assistant_wake_close(void);
/** Number of 16-bit samples one assistant_wake_feed() call takes (0 when closed). */
int assistant_wake_chunk(void);
/** The loaded model list (shared with assistant_afe), NULL when closed. */
srmodel_list_t *assistant_wake_models(void);
/** Feed one chunk; true when the wake word was heard. */
bool assistant_wake_feed(const int16_t *pcm);

#ifdef __cplusplus
}
#endif
