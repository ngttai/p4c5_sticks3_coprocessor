/*
 * Barge-in while the assistant speaks: the esp-sr front-end of type SR, echo cancellation (the reference is what
 * the speaker plays) followed by WakeNet. Saying the wake word over the answer interrupts it; the speaker's own
 * voice is cancelled, so it does not wake the assistant. Uses the models loaded by assistant_wake.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Create the front-end (needs assistant_wake_open() first); false when it cannot be created. */
bool assistant_afe_open(void);
void assistant_afe_close(void);
/** Samples per channel that one assistant_afe_feed() takes (0 when closed). */
int assistant_afe_chunk(void);
/** Feed one chunk of microphone and of what the speaker played at the same time. */
void assistant_afe_feed(const int16_t *mic, const int16_t *ref);
/** True once when the wake word was heard since the last call. */
bool assistant_afe_woke(void);
/** Forget queued audio and a pending wake word (start of an answer). */
void assistant_afe_reset(void);

#ifdef __cplusplus
}
#endif
