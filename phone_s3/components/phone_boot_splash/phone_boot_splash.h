#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Boot splash over every screen, chosen by Kconfig PHONE_BOOT_SPLASH: the animated logo (splash.bin,
 * QOI frames at 15 fps, see tools/make_splash.py, splash_logo.cpp), a hand-drawn line and a greeting
 * (splash_simple.cpp) or none. Call with the LVGL lock held, right after the display starts.
 */
void phone_boot_splash_show(void);

/**
 * Move the splash onto the current system layer and in front: ESP-Brookesia replaces the system
 * layer of the display when the phone begins. Call with the LVGL lock held, after Phone::begin().
 */
void phone_boot_splash_raise(void);

/**
 * The phone is ready (launcher and apps installed): the animation plays (it waits on its first
 * frame until then), then the splash fades out. Call with the LVGL lock held.
 */
void phone_boot_splash_ready(void);

#ifdef __cplusplus
}
#endif
