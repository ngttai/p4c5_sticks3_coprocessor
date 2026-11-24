#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

/** The next start or resume of Settings shows the Wi-Fi page (quick settings, long press on Wi-Fi). */
void phone_settings_open_wifi_next(void);

namespace esp_brookesia::apps {

class PhoneSettings: public systems::phone::App {
public:
    static PhoneSettings *requestInstance();
    ~PhoneSettings();

protected:
    bool run() override;
    bool back() override;
    bool resume() override;

private:
    PhoneSettings();

    inline static PhoneSettings *_instance = nullptr;
};

} // namespace esp_brookesia::apps
