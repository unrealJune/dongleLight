// Lamp protocol families and GATT profiles.
//
// Triones / HappyLighting: cross-checked against trionesControl, ha-triones and
//   Home Assistant's led-ble (flux_led "LEDENET original" protocol).
// KS: ported from H4ch1Net/ks-led-controller (ks_light/protocol.py,
//   profiles.json); only KS03~ is hardware-tested upstream.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KS_FMT_NONE = 0,  // KS power only
    KS_FMT_STANDARD,  // KS: 7E 07 05 03 RR GG BB 00 EF
    KS_FMT_EXTENDED,  // KS03~: 5A 00 01 RR GG BB 00 LL 00 A5 (+ native effects, white)
    KS_FMT_TRIONES,   // Triones: CC 23 33 / 56 RR GG BB 00 F0 AA / BB MM DD 44
} ks_color_fmt_t;

#define KS_MAX_WRITE_CHRS 4

typedef struct {
    const char *prefix;                     // advertised-name prefix (case-insensitive)
    const char *family;                     // "Triones" / "KS"
    uint16_t write_chr[KS_MAX_WRITE_CHRS];  // candidates, in priority order, 0-terminated
    ks_color_fmt_t fmt;
    bool effects;                           // built-in effects usable
    uint16_t notify_chr[KS_MAX_WRITE_CHRS]; // state-reply candidates; empty = no readback
} ks_profile_t;

// What a lamp reports about itself (Triones 0x66 reply; layout per flux_led
// LEDENETOriginalRawState: 66 model power pattern mode speed R G B W ver).
typedef struct {
    bool on;
    uint8_t model, pattern, mode, speed;
    uint8_t r, g, b, w;
} ks_lamp_state_t;

// Longest-prefix match on the advertised name. NULL if not a known lamp.
const ks_profile_t *ks_profile_for_name(const char *name);

#define KS_MAX_PACKET 12

size_t ks_pkt_power(const ks_profile_t *p, bool on, uint8_t *out);
// brightness_pct 0..100. Formats without a brightness byte scale the RGB instead.
size_t ks_pkt_rgb(const ks_profile_t *p, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t brightness_pct, uint8_t *out);
// effect: our ids 0x82..0x8A (KS03~ native numbering; mapped for Triones).
// Returns 0 if unsupported or args out of range.
size_t ks_pkt_effect(const ks_profile_t *p, uint8_t effect, uint8_t speed,
                     uint8_t brightness_pct, uint8_t *out);
// Dedicated white channel; 0 if the family has none we can rely on.
size_t ks_pkt_white(const ks_profile_t *p, uint8_t level, uint8_t *out);

// State query (Triones EF 01 77); 0 if the family can't report state.
size_t ks_pkt_query(const ks_profile_t *p, uint8_t *out);

// Parse a state notification. Returns true for a valid state reply.
bool ks_status_parse(const ks_profile_t *p, const uint8_t *data, size_t len, ks_lamp_state_t *st);

#ifdef __cplusplus
}
#endif
