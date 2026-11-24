#include <stdlib.h>
#include "phone_fonts.h"

#define PHONE_FONT_LIST(X) \
    X(12) X(14) X(16) X(18) X(20) X(24) X(28)

#define X(n) extern const lv_font_t phone_font_##n;
PHONE_FONT_LIST(X)
#undef X

static const struct {
    int size;
    const lv_font_t *font;
} s_fonts[] = {
#define X(n) { n, &phone_font_##n },
    PHONE_FONT_LIST(X)
#undef X
};

const lv_font_t *phone_font_by_size(int size_px)
{
    const lv_font_t *best = s_fonts[0].font;
    int best_diff = abs(size_px - s_fonts[0].size);

    for (size_t i = 1; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        int diff = abs(size_px - s_fonts[i].size);
        if (diff < best_diff) {
            best_diff = diff;
            best = s_fonts[i].font;
        }
    }
    return best;
}
