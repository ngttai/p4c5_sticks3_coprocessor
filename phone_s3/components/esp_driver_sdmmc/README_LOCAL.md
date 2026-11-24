# Local copy of `esp_driver_sdmmc` (ESP-IDF v6.1)

Copied from `$IDF_PATH/components/esp_driver_sdmmc` of ESP-IDF v6.1 (without `test_apps`). As a
project component it replaces the IDF one. Two changes:

1. `src/sd_trans_sdmmc.c`: `cmd_needs_auto_stop()` returns false for a command with the flag
   `SCF_NO_AUTO_STOP` (0x4000, a bit unused by `sd_protocol_types.h`).
2. `legacy/src/sdmmc_transaction.c` + `legacy/include/driver/sdmmc_host.h`:
   `sdmmc_host_set_counted_writes(slot, enable)`. When on, `sdmmc_host_do_transaction()` sends each
   WRITE_MULTIPLE_BLOCK (CMD25) of the slot as SET_BLOCK_COUNT (CMD23) + CMD25 with
   `SCF_NO_AUTO_STOP`, and a CMD12 after a failed CMD25 (the `sdmmc` protocol layer sends none, so
   the card stayed in the receive state). A card that does not answer CMD23 three times in a row
   goes back to the automatic CMD12. Commands already marked `SCF_NO_AUTO_STOP` pass unchanged.
   The phone turns it on for slot 0 after mounting the card (`phone_storage.cpp`).

Why: on this board the SDMMC host runs the microSD card (slot 0) and the ESP-Hosted SDIO link of the
ESP32-C5 (slot 1). During multiple block writes the automatic STOP_TRANSMISSION (CMD12) sometimes
reaches the card with a CRC error, mostly while Wi-Fi reconnects; the card stays in the receive state
(R1 status 0x800d00 / 0x400d00, state 6) and refuses every command until it is power-cycled. Seen with
the USB drive of the Files app (long write bursts, which write CMD23 + CMD25 itself) and with FAT
writes (photos of the USB camera: stuck after 14-26 shots with Wi-Fi on, 135 shots fine with Wi-Fi off).

When ESP-IDF is updated: copy the new component again and redo the changes, or delete this folder.
