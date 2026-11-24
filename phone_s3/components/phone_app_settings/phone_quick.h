#pragma once

namespace esp_brookesia::systems::phone {
class Phone;
}

/**
 * Quick settings: a panel pulled down from the top edge (it follows the finger), with the time,
 * the date, the battery, a button to Settings, Wi-Fi and Mute tiles and brightness and volume
 * sliders. Closed by a swipe up from its handle, a tap below it or the lock screen. Not on the
 * lock screen. Call once, with the LVGL lock held, after the phone has begun.
 */
void phone_quick_start(esp_brookesia::systems::phone::Phone *phone);

/**
 * True while the panel is out or being dragged (LVGL task). It covers the top 376 px: apps that
 * animate there (spectrum bars) skip their updates meanwhile, since each change makes LVGL redraw the
 * panel and the dimmed shade over it (about +20 % of a core at 25 updates a second).
 */
bool phone_quick_visible(void);
