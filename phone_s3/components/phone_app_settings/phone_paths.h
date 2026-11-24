/*
 * Where things are on the memory card, in one place.
 *
 *   DCIM/ Music/ Videos/      the user's media, under the usual names (a PC or another phone finds them)
 *   apps/                     left to apps installed on the card (ESP-Brookesia can install apps there)
 *   appdata/<app>/            what an app keeps: settings, lists, assets that replace the built-in ones
 *   system/                   files of the system itself (speed test, ...)
 *
 * Folder names under appdata/ are the app names in lower case.
 */
#pragma once

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "sdkconfig.h"

#define PHONE_SD_ROOT               CONFIG_BSP_SD_MOUNT_POINT

/* media */
#define PHONE_SD_DCIM               PHONE_SD_ROOT "/DCIM"
#define PHONE_SD_MUSIC              PHONE_SD_ROOT "/Music"

/* app data */
#define PHONE_SD_APPDATA            PHONE_SD_ROOT "/appdata"
#define PHONE_SD_RADIO_STATIONS     PHONE_SD_APPDATA "/radio/stations.json"
#define PHONE_SD_MUSIC_FAVORITES    PHONE_SD_APPDATA "/music/favorites.txt"
#define PHONE_SD_ASSISTANT_EMOTE    PHONE_SD_APPDATA "/assistant/emote"

/* system */
#define PHONE_SD_SYSTEM             PHONE_SD_ROOT "/system"
#define PHONE_SD_SDBENCH            PHONE_SD_SYSTEM "/sdbench.bin"

/* where v0.1.0 kept them: still read when the new file is not there */
#define PHONE_SD_OLD_RADIO_STATIONS     PHONE_SD_ROOT "/radio/stations.json"
#define PHONE_SD_OLD_MUSIC_FAVORITES    PHONE_SD_ROOT "/Music/favorites.txt"

/** Create the folders above a file path ("/sdcard/appdata/radio/x.json" -> appdata, appdata/radio); 0 when they exist. */
static inline int phone_sd_make_dirs(const char *file_path)
{
    char dir[128];
    if (strlen(file_path) >= sizeof(dir)) {
        return -1;
    }
    strcpy(dir, file_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    /* skip the mount point itself */
    for (char *p = dir + strlen(PHONE_SD_ROOT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    return (mkdir(dir, 0775) != 0 && errno != EEXIST) ? -1 : 0;
}
