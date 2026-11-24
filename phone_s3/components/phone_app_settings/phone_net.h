#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool wifi_connected;
    bool wifi_connecting;
    int rssi;
    char ssid[33];
    char ip[16];
    bool time_synced;
} phone_net_status_t;

/** Start the background task: brings Wi-Fi up (ESP32-S3 over ESP-Hosted), auto-connects to saved networks, SNTP, RSSI polling. */
void phone_net_start(void);
void phone_net_get_status(phone_net_status_t *out);
bool phone_net_ready(void);
/** Internet reachable: Wi-Fi has an address, or mobile data (PPP over 4G) is up. Apps check this, not Wi-Fi. */
bool phone_net_online(void);
/** Wi-Fi co-processor (ESP32-S3) firmware version, read once Wi-Fi is up; false until then. */
bool phone_net_c5_version(char *out, size_t len);
/** User-initiated connect: saves the network at the front of the list and connects; auto reconnect pauses until it succeeds. */
void phone_net_connect(const char *ssid, const char *pass);
/** Stop an auto-reconnect attempt (the Wi-Fi page scans), Wi-Fi stays on; phone_net_enable() resumes it. */
void phone_net_pause_auto(void);
/** User turns Wi-Fi off: drop the link and stop auto reconnect. */
void phone_net_disconnect(void);
/** User turns Wi-Fi on: reconnect through the saved list and resume auto reconnect. */
void phone_net_enable(void);
/** The Wi-Fi switch (Settings, quick settings): kept across a restart (NVS key "wifi"), then disconnect or enable. */
void phone_net_set_enabled(bool on);
/** Wi-Fi on for the user: true until phone_net_disconnect(), again after phone_net_enable(). */
bool phone_net_is_enabled(void);
/** Number of saved networks (0..5). */
int phone_net_saved_count(void);
/** Copy the saved SSID at index (0 = most recent) into out; returns false when index is out of range. */
bool phone_net_saved_at(int index, char *out, size_t len);
/** Copy the most recent saved SSID into out; returns false when no network is saved. */
bool phone_net_saved_ssid(char *out, size_t len);
/** Forget one saved network; drops the link when it is the active one, auto reconnect moves to the next saved network. */
void phone_net_forget_ssid(const char *ssid);
/** Forget the most recent saved network. */
void phone_net_forget(void);
/** Connect to the saved entry at index using its stored password. */
void phone_net_connect_saved(int index);

#ifdef __cplusplus
}
#endif
