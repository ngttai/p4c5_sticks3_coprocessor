#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * Radio: HLS internet radio stations listed in /sdcard/appdata/radio/stations.json (no built-in list), with a
 * 12-band spectrum. It keeps playing in the background; closing the app stops it, and so does another
 * player taking the speaker.
 */
class PhoneRadio: public systems::phone::App {
public:
    static PhoneRadio *requestInstance();
    ~PhoneRadio();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool keepRunning() const override;     // playing: not closed at the running app limit

private:
    PhoneRadio();

    inline static PhoneRadio *_instance = nullptr;
};

} // namespace esp_brookesia::apps
