// KS lamp packets and GATT profiles, ported from H4ch1Net/ks-led-controller
// (ks_light/protocol.py, ks_light/profiles.json). Only KS03~ is hardware-tested
// upstream; the other profiles are inherited from the vendor app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KS_FMT_NONE = 0,  // power only
    KS_FMT_STANDARD,  // 7E 07 05 03 RR GG BB 00 EF
    KS_FMT_EXTENDED,  // 5A 00 01 RR GG BB 00 LL 00 A5 (+ native effects, white)
} ks_color_fmt_t;

typedef struct {
    const char *prefix;
    uint16_t service;
    uint16_t write_chr;
    ks_color_fmt_t fmt;
} ks_profile_t;

// Longest-prefix match on the advertised name. NULL if not a KS lamp.
const ks_profile_t *ks_profile_for_name(const char *name);

#define KS_MAX_PACKET 12

size_t ks_pkt_power(bool on, uint8_t *out);
// brightness_pct 0..100. Standard format has no brightness byte, so RGB is scaled.
size_t ks_pkt_rgb(const ks_profile_t *p, uint8_t r, uint8_t g, uint8_t b,
                  uint8_t brightness_pct, uint8_t *out);
// Extended format only; returns 0 if unsupported or args out of range.
size_t ks_pkt_effect(const ks_profile_t *p, uint8_t effect, uint8_t speed,
                     uint8_t brightness_pct, uint8_t *out);
size_t ks_pkt_white(const ks_profile_t *p, uint8_t level, uint8_t *out);

#ifdef __cplusplus
}
#endif
