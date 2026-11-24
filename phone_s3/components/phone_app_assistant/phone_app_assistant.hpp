#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Assistant: voice chat with the xiaozhi.me service. */
class PhoneAssistant: public systems::phone::App {
public:
    static PhoneAssistant *requestInstance();
    ~PhoneAssistant();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    PhoneAssistant();

    inline static PhoneAssistant *_instance = nullptr;
};

} // namespace esp_brookesia::apps
