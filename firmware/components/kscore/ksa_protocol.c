#include "ksa_protocol.h"
#include <string.h>

static const ks_profile_t PROFILES[] = {
    {"KS03~", 0xAFD0, 0xAFD1, KS_FMT_EXTENDED}, // hardware-tested upstream
    {"KS03-", 0xFFF0, 0xFFF3, KS_FMT_STANDARD},
    {"KS04-", 0xFFF0, 0xFFF3, KS_FMT_STANDARD},
    {"KS01-", 0xAE00, 0xAE01, KS_FMT_STANDARD},
    {"KS02-", 0xAE00, 0xAE01, KS_FMT_STANDARD},
    {"KS04~", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS05-", 0xAE00, 0xAE02, KS_FMT_NONE},
    {"KS07-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS08-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS09-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS10-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS11-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS12-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS13-", 0xAE00, 0xAE10, KS_FMT_NONE},
    {"KS15~", 0xAFD0, 0xAFD3, KS_FMT_NONE},
};

const ks_profile_t *ks_profile_for_name(const char *name)
{
    if (!name) return NULL;
    const ks_profile_t *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        size_t n = strlen(PROFILES[i].prefix);
        if (strncmp(name, PROFILES[i].prefix, n) == 0 && n > best_len) {
            best = &PROFILES[i];
            best_len = n;
        }
    }
    return best;
}

size_t ks_pkt_power(bool on, uint8_t *out)
{
    const uint8_t pkt[4] = {0x5B, on ? 0xF0 : 0x0F, 0x01, 0xB5};
    memcpy(out, pkt, 4);
    return 4;
}

static uint8_t clamp_pct(uint8_t v) { return v > 100 ? 100 : v; }

size_t ks_pkt_rgb(const ks_profile_t *p, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t brightness_pct, uint8_t *out)
{
    // Extended: byte passed through (upstream hub/app send a 0..100 percent; the
    // legacy CLI sent raw 0..255, so the golden fixtures include 0x7F/0xFF).
    if (p->fmt == KS_FMT_EXTENDED) {
        const uint8_t pkt[10] = {0x5A, 0x00, 0x01, r, g, b, 0x00, brightness_pct, 0x00, 0xA5};
        memcpy(out, pkt, 10);
        return 10;
    }
    if (p->fmt == KS_FMT_STANDARD) {
        brightness_pct = clamp_pct(brightness_pct);
        r =(uint8_t)((r * brightness_pct + 50) / 100);
        g = (uint8_t)((g * brightness_pct + 50) / 100);
        b = (uint8_t)((b * brightness_pct + 50) / 100);
        const uint8_t pkt[9] = {0x7E, 0x07, 0x05, 0x03, r, g, b, 0x00, 0xEF};
        memcpy(out, pkt, 9);
        return 9;
    }
    return 0;
}

size_t ks_pkt_effect(const ks_profile_t *p, uint8_t effect, uint8_t speed,
                     uint8_t brightness_pct, uint8_t *out)
{
    if (p->fmt != KS_FMT_EXTENDED) return 0;
    if (effect < 0x82 || effect > 0x8A || speed > 100 || brightness_pct < 1 || brightness_pct > 100) {
        return 0;
    }
    const uint8_t pkt[7] = {0x5C, 0x00, effect, speed, brightness_pct, 0x00, 0xC5};
    memcpy(out, pkt, 7);
    return 7;
}

size_t ks_pkt_white(const ks_profile_t *p, uint8_t level, uint8_t *out)
{
    if (p->fmt != KS_FMT_EXTENDED) return 0;
    const uint8_t pkt[10] = {0x5A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, level, 0x00, 0xA5};
    memcpy(out, pkt, 10);
    return 10;
}
