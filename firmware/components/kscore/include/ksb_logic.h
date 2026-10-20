// Lamp state model, portable and host-tested.
//
// Two layers decide what the strip shows:
//   base     what you set by hand (power / rgb / effect / white)
//   sources  agent signals from devboxes; the most urgent unacknowledged one
//            overlays the base until it is acked, cleared or expires
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
#define KSB_DONE_TTL_MS (2u * 60u * 1000u)
#define KSB_WORKING_TTL_MS (4u * 60u * 60u * 1000u)
#define KSB_ACKED_TTL_MS (10u * 60u * 1000u)

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
} ksb_source_t;

typedef enum { KSB_EV_NONE, KSB_EV_MANUAL, KSB_EV_SIGNAL, KSB_EV_ACK, KSB_EV_BAD } ksb_event_kind_t;

typedef struct {
    ksb_scene_t base;
    ksb_scene_t last_on;        // restored by "power on"
    ksb_source_t src[KSB_MAX_SOURCES];
    uint8_t last_event;         // ksb_event_kind_t, for the screen
    int8_t last_event_src;      // source index or -1
    uint32_t last_event_ms;
} ksb_model_t;

void ksb_init(ksb_model_t *m);

// Apply a validated command. Returns false for ops the model doesn't own.
bool ksb_apply(ksb_model_t *m, const ksa_cmd_t *cmd, uint32_t now_ms);

// Acknowledge every attention signal (button press or KSA_OP_ACK).
void ksb_ack_all(ksb_model_t *m, uint32_t now_ms);

// Expire stale sources. Call about once a second.
void ksb_tick(ksb_model_t *m, uint32_t now_ms);

// Index of the source currently overriding the lamp, or -1 for the base scene.
int ksb_top_source(const ksb_model_t *m);

// What the strip should show right now.
void ksb_desired(const ksb_model_t *m, ksb_scene_t *out);

bool ksb_scene_eq(const ksb_scene_t *a, const ksb_scene_t *b);

// Display color for a scene (what the screen creature glows), brightness applied.
void ksb_scene_rgb(const ksb_scene_t *s, uint8_t *r, uint8_t *g, uint8_t *b);

// Packets that put the lamp into `s` for profile `p`. Effects/white fall back to
// static colors on standard-format lamps. Returns packet count (0..2).
int ksb_packets(const ks_profile_t *p, const ksb_scene_t *s,
                uint8_t pkt[2][KS_MAX_PACKET], size_t len[2]);

const char *ksb_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif
