#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Music: plays the audio files of the microSD card on the speaker, also in the background. */
class PhoneMusic: public systems::phone::App {
public:
    static PhoneMusic *requestInstance();
    ~PhoneMusic();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool keepRunning() const override;     // playing: not closed at the running app limit

private:
    PhoneMusic();

    inline static PhoneMusic *_instance = nullptr;
};

} // namespace esp_brookesia::apps
