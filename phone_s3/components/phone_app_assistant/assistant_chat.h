/*
 * Voice session of the assistant: connection to the Xiaozhi server (esp_xiaozhi), microphone ->
 * Opus -> server, server -> Opus -> speaker. Everything here is safe to call from the LVGL task:
 * the calls only post a command or read a state.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ASSISTANT_CHAT_OFF,         /*!< Not started */
    ASSISTANT_CHAT_CONNECTING,  /*!< Connecting to the server */
    ASSISTANT_CHAT_IDLE,        /*!< Connected, no conversation */
    ASSISTANT_CHAT_OPENING,     /*!< Opening the audio channel */
    ASSISTANT_CHAT_LISTENING,   /*!< Microphone is sent to the server */
    ASSISTANT_CHAT_THINKING,    /*!< The server heard a sentence and prepares the answer */
    ASSISTANT_CHAT_SPEAKING,    /*!< The answer is played */
    ASSISTANT_CHAT_ERROR,       /*!< Connection lost or failed; start again */
} assistant_chat_state_t;

typedef enum {
    ASSISTANT_CHAT_MSG_USER,        /*!< What the server understood from the microphone */
    ASSISTANT_CHAT_MSG_ASSISTANT,   /*!< One sentence of the answer */
    ASSISTANT_CHAT_MSG_EMOTION,     /*!< Emotion name for the face */
    ASSISTANT_CHAT_MSG_NOTICE,      /*!< Problem to show to the user */
} assistant_chat_msg_type_t;

typedef struct {
    assistant_chat_msg_type_t type;
    char text[256];
} assistant_chat_msg_t;

/** Connect to the server with the transports returned by esp_xiaozhi_chat_get_info(). */
void assistant_chat_start(bool has_mqtt, bool has_websocket);
/** End the conversation and disconnect; returns once the tasks have stopped (up to a few seconds). */
void assistant_chat_stop(void);
/**
 * The microphone button: start a conversation when idle, end it while listening or thinking,
 * cut the answer and listen again while speaking.
 */
void assistant_chat_button(void);
/** The "wake" or "barge" preference changed; the session applies it once no conversation is open. */
void assistant_chat_options_changed(void);
/** Measure the echo delay (noise burst, about 2 s) and keep it in pref "aecd"; the result comes as a notice. */
void assistant_chat_calibrate(void);
assistant_chat_state_t assistant_chat_state(void);
/** Next text or emotion from the server; false when there is none. */
bool assistant_chat_poll(assistant_chat_msg_t *msg);
/** Microphone level of the last frame sent, 0..100. */
int assistant_chat_mic_level(void);

#ifdef __cplusplus
}
#endif
