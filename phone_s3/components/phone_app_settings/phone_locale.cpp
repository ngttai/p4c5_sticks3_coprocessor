#include "phone_locale.h"
#include <cstdlib>
#include <ctime>
#include "nvs.h"

namespace {

struct Zone {
    const char *vi;
    const char *en;
    const char *posix;
};

constexpr Zone ZONES[] = {
    {"(UTC-11) Pago Pago", "(UTC-11) Pago Pago", "<-11>11"},
    {"(UTC-10) Honolulu", "(UTC-10) Honolulu", "HST10"},
    {"(UTC-8) Los Angeles", "(UTC-8) Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"(UTC-7) Denver", "(UTC-7) Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"(UTC-6) Chicago", "(UTC-6) Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"(UTC-5) New York", "(UTC-5) New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"(UTC-3) São Paulo", "(UTC-3) São Paulo", "<-03>3"},
    {"(UTC+0) London", "(UTC+0) London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"(UTC+1) Paris, Berlin", "(UTC+1) Paris, Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"(UTC+2) Athens, Cairo", "(UTC+2) Athens, Cairo", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"(UTC+3) Moskva", "(UTC+3) Moscow", "MSK-3"},
    {"(UTC+4) Dubai", "(UTC+4) Dubai", "<+04>-4"},
    {"(UTC+5:30) Ấn Độ", "(UTC+5:30) India", "IST-5:30"},
    {"(UTC+7) Hà Nội, Bangkok", "(UTC+7) Hanoi, Bangkok", "<+07>-7"},
    {"(UTC+8) Singapore, Bắc Kinh", "(UTC+8) Singapore, Beijing", "<+08>-8"},
    {"(UTC+9) Tokyo, Seoul", "(UTC+9) Tokyo, Seoul", "JST-9"},
    {"(UTC+10) Sydney", "(UTC+10) Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"(UTC+12) Auckland", "(UTC+12) Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr int ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
constexpr int ZONE_DEFAULT = 13;

phone_lang_t s_lang = PHONE_LANG_VI;
int s_tz = ZONE_DEFAULT;

void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("phone", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void apply_tz()
{
    setenv("TZ", ZONES[s_tz].posix, 1);
    tzset();
}

} // namespace

extern "C" void phone_locale_init(void)
{
    nvs_handle_t h;
    uint8_t lang = PHONE_LANG_VI;
    uint8_t tz = ZONE_DEFAULT;
    if (nvs_open("phone", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "lang", &lang);
        nvs_get_u8(h, "tz", &tz);
        nvs_close(h);
    }
    s_lang = lang == PHONE_LANG_EN ? PHONE_LANG_EN : PHONE_LANG_VI;
    s_tz = tz < ZONE_COUNT ? tz : ZONE_DEFAULT;
    apply_tz();
}

extern "C" phone_lang_t phone_lang_get(void)
{
    return s_lang;
}

extern "C" void phone_lang_set(phone_lang_t lang)
{
    s_lang = lang;
    save_u8("lang", static_cast<uint8_t>(lang));
}

extern "C" const char *phone_tr(const char *vi, const char *en)
{
    return s_lang == PHONE_LANG_EN ? en : vi;
}

extern "C" int phone_tz_count(void)
{
    return ZONE_COUNT;
}

extern "C" const char *phone_tz_label(int idx)
{
    if (idx < 0 || idx >= ZONE_COUNT) {
        return "";
    }
    return s_lang == PHONE_LANG_EN ? ZONES[idx].en : ZONES[idx].vi;
}

extern "C" int phone_tz_get(void)
{
    return s_tz;
}

extern "C" void phone_tz_set(int idx)
{
    if (idx < 0 || idx >= ZONE_COUNT) {
        return;
    }
    s_tz = idx;
    apply_tz();
    save_u8("tz", static_cast<uint8_t>(idx));
}

extern "C" int phone_pref_get(const char *key, int def)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("phone", NVS_READONLY, &h) != ESP_OK) {
        return def;
    }
    esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : def;
}

extern "C" void phone_pref_set(const char *key, int value)
{
    save_u8(key, static_cast<uint8_t>(value));
}
