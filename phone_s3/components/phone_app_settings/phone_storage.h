#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sd_protocol_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_STORAGE_DISABLED = 0,    /*!< CONFIG_PHONE_SD_CARD_MOUNT is off */
    PHONE_STORAGE_CHECKING,        /*!< Mount in progress */
    PHONE_STORAGE_MOUNTED,
    PHONE_STORAGE_NO_CARD,         /*!< No card, or a card that could not be mounted (never formatted) */
    PHONE_STORAGE_USB,             /*!< Lent to a PC as a USB drive: /sdcard is not mounted */
} phone_storage_state_t;

/** Mount the microSD card in the background (no card detect pin: checked at boot only). */
void phone_storage_start(void);
/** Current state; total and free are filled in bytes when mounted (either may be NULL). */
phone_storage_state_t phone_storage_get(uint64_t *total, uint64_t *free);

/**
 * An app that may keep files of the card open (in the background too) holds the card from its
 * start to its close; the card cannot be lent to USB meanwhile. `who` is a string literal
 * (the app name), up to 4 holders.
 */
void phone_storage_hold(const char *who);
void phone_storage_release(const char *who);
/** Names of the holders, joined by ", "; returns how many there are. */
int phone_storage_holders(char *buf, size_t len);

/**
 * Unmount /sdcard and hand the card over for raw sector access (USB mass storage).
 * ESP_ERR_INVALID_STATE when not mounted or held by an app.
 */
esp_err_t phone_storage_lend(sdmmc_card_t **card);
/** Release the raw card and mount /sdcard again (never formatted). */
esp_err_t phone_storage_take_back(void);

/**
 * After a write that timed out: the card may stay in its receive state and refuse every later
 * command (ESP-IDF sends no STOP_TRANSMISSION). Unmount, power the card off and mount it again
 * (never formatted). Not done while an app holds the card (its open files would be lost):
 * ESP_ERR_INVALID_STATE then, and also when the card is not mounted.
 */
esp_err_t phone_storage_recover(void);

#ifdef __cplusplus
}
#endif
