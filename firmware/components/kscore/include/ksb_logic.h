// Lamp state model, portable and host-tested.
//
// Each lamp shows two layers:
//   base     what you set by hand (power / rgb / effect / white), per lamp
//   signals  agent states from devboxes. Each devbox is mapped to one lamp
//            (auto-assigned in order of first signal, sticky, or pinned by the
//            devbox); unmapped devboxes overlay every lamp. The most urgent
//            unacknowledged signal for a lamp overrides its base.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksa_frame.h"
#include "ksa_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KSB_MAX_SOURCES 8
#define KSB_MAX_LAMPS KSA_MAX_LAMPS
#define KSB_DONE_TTL_MS (2u * 60u * 1000u)
#define KSB_WORKING_TTL_MS (4u * 60u * 60u * 1000u)
#define KSB_ACKED_TTL_MS (10u * 60u * 1000u)

typedef enum { KS_CLAIM_IDLE, KS_CLAIM_SEARCHING, KS_CLAIM_OK, KS_CLAIM_NONE_FOUND,
               KS_CLAIM_FULL, KS_CLAIM_FORGOT } ks_claim_state_t;

typedef enum { KSB_SCENE_OFF, KSB_SCENE_RGB, KSB_SCENE_EFFECT, KSB_SCENE_WHITE } ksb_scene_kind_t;

typedef struct {
    uint8_t kind;               // ksb_scene_kind_t
    uint8_t r, g, b, bright;    // RGB, bright 0..100
    uint8_t effect, speed;      // EFFECT (KS03~ native ids)
    uint8_t white;              // WHITE level 0..255
} ksb_scene_t;

typedef struct {
    bool used;
    uint8_t id;
    char name[KSA_SIGNAL_NAME_MAX + 1];
    uint8_t state;              // KSA_SIG_*
    bool acked;
    uint32_t since_ms;          // when state last changed
    bool styled;
    uint8_t r, g, b, bright;
} ksb_source_t;

typedef struct {
    uint8_t r, g, b, bright, lamp;
} ksb_rule_t;

typedef struct {
    bool used;
    uint8_t id;
    char name[KSA_SIGNAL_NAME_MAX + 1];
    uint32_t seen_ms;
} ksb_channel_t;

typedef enum { KSB_EV_NONE, KSB_EV_MANUAL, KSB_EV_SIGNAL, KSB_EV_ACK, KSB_EV_BAD } ksb_event_kind_t;

typedef struct {
    ksb_scene_t base[KSB_MAX_LAMPS];
    ksb_scene_t last_on[KSB_MAX_LAMPS];  // restored by "power on"
    ksb_source_t src[KSB_MAX_SOURCES];
    uint8_t n_lamps;                     // claimed lamps (set by the firmware)
    uint8_t lamp_src[KSB_MAX_LAMPS];     // devbox id mapped to each lamp
    uint8_t lamp_src_valid;              // bitmask of lamps with a mapping
    bool map_dirty;                      // mapping changed: persist it
    uint8_t last_event;                  // ksb_event_kind_t, for the screen
    int8_t last_event_src;               // source index or -1
    uint32_t last_event_ms;
    ksb_rule_t rules[4][2][KSA_SIG_COUNT];
    ksb_channel_t channels[4];
    bool rules_dirty;
} ksb_model_t;

void ksb_init(ksb_model_t *m);
void ksb_set_lamp_count(ksb_model_t *m, uint8_t n);
void ksb_clear_map(ksb_model_t *m);

// Apply a validated command. Returns false for ops/args the model rejects.
bool ksb_apply(ksb_model_t *m, const ksa_cmd_t *cmd, uint32_t now_ms);

// Acknowledge attention signals shown on the lamps in `mask` (0x0F = all).
void ksb_ack(ksb_model_t *m, uint8_t mask, uint32_t now_ms);
static inline void ksb_ack_all(ksb_model_t *m, uint32_t now_ms) { ksb_ack(m, 0x0F, now_ms); }

// Expire stale sources. Call about once a second.
void ksb_tick(ksb_model_t *m, uint32_t now_ms);

// Lamp a source is mapped to, or -1 (unmapped: shows on every lamp).
int ksb_source_lamp(const ksb_model_t *m, int src_index);

// Most urgent unacked source shown on `lamp`, or -1. lamp = -1: across all lamps.
int ksb_top_source_for(const ksb_model_t *m, int lamp);
static inline int ksb_top_source(const ksb_model_t *m) { return ksb_top_source_for(m, -1); }

// What `lamp` should show right now.
void ksb_desired(const ksb_model_t *m, int lamp, ksb_scene_t *out);

bool ksb_scene_eq(const ksb_scene_t *a, const ksb_scene_t *b);

// Display color for a scene (what the screen creature glows), brightness applied.
void ksb_scene_rgb(const ksb_scene_t *s, uint8_t *r, uint8_t *g, uint8_t *b);

// Packets that put a lamp into `s` for profile `p`. Effects/white fall back to
// static colors on standard-format lamps. Returns packet count (1..2).
int ksb_packets(const ks_profile_t *p, const ksb_scene_t *s,
                uint8_t pkt[2][KS_MAX_PACKET], size_t len[2]);

const char *ksb_state_name(uint8_t state);

// ---- readback (lamps that report state, e.g. Triones)

// What a lamp should report after being put into a scene, derived from the
// exact packets ksb_packets() sends, so the check can't drift from the encoder.
typedef struct {
    bool on;
    bool has_rgb;       // static color: R G B must match exactly
    uint8_t r, g, b;
    bool has_pattern;   // built-in effect: pattern byte should match (weak evidence)
    uint8_t pattern;
} ksb_expect_t;

typedef enum { KSB_CHECK_MATCH, KSB_CHECK_MISMATCH, KSB_CHECK_UNSURE } ksb_check_t;

void ksb_expect(const ks_profile_t *p, const ksb_scene_t *s, ksb_expect_t *out);

// MATCH: the lamp shows what we sent. MISMATCH: power or a static color differs.
// UNSURE: power is right but an effect can't be confirmed from the reply.
ksb_check_t ksb_check(const ksb_expect_t *e, const ks_lamp_state_t *st);

// Did someone change the lamp by hand? `prev` is the previous poll (or NULL).
// Power changes and built-in effects count at once; a different static color
// only once two polls agree (an effect started from the remote changes the
// reported RGB every poll, and must not be adopted as a stream of colors).
bool ksb_drift(const ksb_expect_t *e, const ks_lamp_state_t *now, const ks_lamp_state_t *prev);

// Someone changed `lamp` by hand (IR remote, phone app). Make what it shows the
// new base for that lamp, and treat the touch as acknowledging any alert that
// was showing there. Fills *adopted with the scene now expected and *e with
// what the lamp should keep reporting (so the same state isn't adopted again).
void ksb_adopt(ksb_model_t *m, const ks_profile_t *p, int lamp, const ks_lamp_state_t *st, uint32_t now_ms,
               ksb_scene_t *adopted, ksb_expect_t *e);

#ifdef __cplusplus
}
#endif
