#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_LANG_VI = 0,
    PHONE_LANG_EN = 1,
} phone_lang_t;

/** Load language and timezone from NVS and apply the timezone. */
void phone_locale_init(void);
phone_lang_t phone_lang_get(void);
void phone_lang_set(phone_lang_t lang);
/** Text for the current language. */
const char *phone_tr(const char *vi, const char *en);
int phone_tz_count(void);
/** Timezone label in the current language. */
const char *phone_tz_label(int idx);
int phone_tz_get(void);
void phone_tz_set(int idx);

/** Persistent small integer setting in NVS. */
int phone_pref_get(const char *key, int def);
void phone_pref_set(const char *key, int value);

#ifdef __cplusplus
}
#endif
