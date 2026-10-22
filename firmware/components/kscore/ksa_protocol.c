#include "ksa_protocol.h"
#include <ctype.h>
#include <string.h>

// Triones write characteristic differs by controller batch; led-ble's priority
// order (it notes FFE5 "potentially invalid, try last").
#define TRIONES_CHRS {0xFFD5, 0xFFD9, 0xFFE9, 0xFFE5}
// Where state replies arrive (led-ble's order); FFD4 confirmed on Iris's strips.
#define TRIONES_NOTIFY {0xFFD0, 0xFFD4, 0xFFE0, 0xFFE4}

static const ks_profile_t PROFILES[] = {
    // Triones / HappyLighting family (names from the HappyLighting app, ha-triones, led-ble)
    {"Triones", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, true, TRIONES_NOTIFY},
    {"LEDBLE", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, true, TRIONES_NOTIFY},
    {"LEDBlue", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, true, TRIONES_NOTIFY},
    {"QHM", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, true, TRIONES_NOTIFY},
    {"BRGlight", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, true, TRIONES_NOTIFY},
    {"Dream", "Triones", TRIONES_CHRS, KS_FMT_TRIONES, false, TRIONES_NOTIFY}, // led-ble: different effect set
    // KS family
    {"KS03~", "KS", {0xAFD1}, KS_FMT_EXTENDED, true, {0}}, // hardware-tested upstream
    {"KS03-", "KS", {0xFFF3}, KS_FMT_STANDARD, false, {0}},
    {"KS04-", "KS", {0xFFF3}, KS_FMT_STANDARD, false, {0}},
    {"KS01-", "KS", {0xAE01}, KS_FMT_STANDARD, false, {0}},
    {"KS02-", "KS", {0xAE01}, KS_FMT_STANDARD, false, {0}},
    {"KS04~", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS05-", "KS", {0xAE02}, KS_FMT_NONE, false, {0}},
    {"KS07-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS08-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS09-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS10-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS11-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS12-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS13-", "KS", {0xAE10}, KS_FMT_NONE, false, {0}},
    {"KS15~", "KS", {0xAFD3}, KS_FMT_NONE, false, {0}},
};

static bool prefix_ci(const char *name, const char *prefix)
{
    for (; *prefix; prefix++, name++) {
        if (!*name || tolower((unsigned char)*name) != tolower((unsigned char)*prefix)) return false;
    }
    return true;
}

const ks_profile_t *ks_profile_for_name(const char *name)
{
    if (!name) return NULL;
    const ks_profile_t *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        size_t n = strlen(PROFILES[i].prefix);
        if (prefix_ci(name, PROFILES[i].prefix) && n > best_len) {
            best = &PROFILES[i];
            best_len = n;
        }
    }
    return best;
}

size_t ks_pkt_power(const ks_profile_t *p, bool on, uint8_t *out)
{
    if (p->fmt == KS_FMT_TRIONES) {
        const uint8_t pkt[3] = {0xCC, on ? 0x23 : 0x24, 0x33};
        memcpy(out, pkt, 3);
        return 3;
    }
    const uint8_t pkt[4] = {0x5B, on ? 0xF0 : 0x0F, 0x01, 0xB5};
    memcpy(out, pkt, 4);
    return 4;
}

static uint8_t clamp_pct(uint8_t v) { return v > 100 ? 100 : v; }
static uint8_t scale(uint8_t c, uint8_t pct) { return (uint8_t)((c * pct + 50) / 100); }

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
    if (p->fmt == KS_FMT_STANDARD || p->fmt == KS_FMT_TRIONES) {
        brightness_pct = clamp_pct(brightness_pct);
        r = scale(r, brightness_pct);
        g = scale(g, brightness_pct);
        b = scale(b, brightness_pct);
        if (p->fmt == KS_FMT_TRIONES) {
            // 0xF0 = "use the RGB bytes" (0x0F would select the white channel)
            const uint8_t pkt[7] = {0x56, r, g, b, 0x00, 0xF0, 0xAA};
            memcpy(out, pkt, 7);
            return 7;
        }
        const uint8_t pkt[9] = {0x7E, 0x07, 0x05, 0x03, r, g, b, 0x00, 0xEF};
        memcpy(out, pkt, 9);
        return 9;
    }
    return 0;
}

// Our effect ids (KS03~ numbering) -> Triones built-in modes (flux_led names).
static const uint8_t TRIONES_MODE[9] = {
    0x25, // 0x82 seven-color fade  -> 7-color cross fade
    0x25, // 0x83 RGB fade          -> 7-color cross fade (closest)
    0x26, // 0x84 red breathing     -> red gradual change
    0x27, // 0x85 green
    0x28, // 0x86 blue
    0x29, // 0x87 yellow (the needs-you pulse)
    0x2A, // 0x88 cyan
    0x2B, // 0x89 purple
    0x2C, // 0x8A white
};

size_t ks_pkt_effect(const ks_profile_t *p, uint8_t effect, uint8_t speed,
                     uint8_t brightness_pct, uint8_t *out)
{
    if (!p->effects) return 0;
    if (effect < 0x82 || effect > 0x8A || speed > 100 || brightness_pct < 1 || brightness_pct > 100) return 0;
    if (p->fmt == KS_FMT_TRIONES) {
        // Delay 1 (fastest) .. 31 (slowest), as flux_led's speedToDelay. Built-in
        // modes have no brightness byte; they run at full brightness.
        uint8_t delay = (uint8_t)((100 - speed) * 30 / 100 + 1);
        const uint8_t pkt[4] = {0xBB, TRIONES_MODE[effect - 0x82], delay, 0x44};
        memcpy(out, pkt, 4);
        return 4;
    }
    if (p->fmt != KS_FMT_EXTENDED) return 0;
    const uint8_t pkt[7] = {0x5C, 0x00, effect, speed, brightness_pct, 0x00, 0xC5};
    memcpy(out, pkt, 7);
    return 7;
}

size_t ks_pkt_white(const ks_profile_t *p, uint8_t level, uint8_t *out)
{
    // Triones has 56 00 00 00 WW 0F AA, but RGB-only strips have no white LEDs
    // and would just go dark, so white is rendered through RGB there instead.
    if (p->fmt != KS_FMT_EXTENDED) return 0;
    const uint8_t pkt[10] = {0x5A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, level, 0x00, 0xA5};
    memcpy(out, pkt, 10);
    return 10;
}

size_t ks_pkt_query(const ks_profile_t *p, uint8_t *out)
{
    if (p->fmt != KS_FMT_TRIONES) return 0; // KS lamps don't answer usefully (upstream)
    const uint8_t pkt[3] = {0xEF, 0x01, 0x77};
    memcpy(out, pkt, 3);
    return 3;
}

bool ks_status_parse(const ks_profile_t *p, const uint8_t *d, size_t len, ks_lamp_state_t *st)
{
    // light.py on real strips accepts >= 9 bytes starting 0x66; flux_led expects 11.
    if (p->fmt != KS_FMT_TRIONES || len < 9 || d[0] != 0x66) return false;
    if (d[2] != 0x23 && d[2] != 0x24) return false;
    memset(st, 0, sizeof *st);
    st->model = d[1];
    st->on = d[2] == 0x23;
    st->pattern = d[3];
    st->mode = d[4];
    st->speed = d[5];
    st->r = d[6];
    st->g = d[7];
    st->b = d[8];
    st->w = len > 9 ? d[9] : 0;
    return true;
}
