// 160x80 status screen: "Lumi", a little lamp spirit that glows in whatever
// color the strip is showing, plus status text and one badge per devbox.
// Portable C so it renders identically on the host (see test/ui_preview.c).
#pragma once
#include <stdint.h>
#include "ksb_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KSUI_W 160
#define KSUI_H 80

typedef enum { KSUI_LINK_BOOT, KSUI_LINK_SCANNING, KSUI_LINK_CONNECTED } ksui_link_t;
typedef enum { KSUI_CLAIM_NONE, KSUI_CLAIM_SEARCHING, KSUI_CLAIM_OK, KSUI_CLAIM_NONE_FOUND,
               KSUI_CLAIM_FULL, KSUI_CLAIM_FORGOT } ksui_claim_t;

typedef struct {
    char name[20];              // advertised name of the claimed lamp
    uint8_t link;               // ksui_link_t for this lamp
} ksui_lamp_t;

typedef struct {
    uint8_t link;               // overall: CONNECTED if any lamp is
    const char *transport;      // "AUDIO" / "CARD" / "DEBUG"
    ksb_model_t model;          // snapshot
    uint8_t n_lamps;            // claimed lamps
    ksui_lamp_t lamps[KSB_MAX_LAMPS];
    ksb_scene_t shown[KSB_MAX_LAMPS]; // what each lamp should be showing
    uint32_t error_until_ms;    // write failure / bad frame: show a frown until then
    uint8_t claim;              // ksui_claim_t
    uint8_t claim_slot;         // lamp index the claim is about
    uint32_t claim_ms;          // when the claim state last changed
    uint32_t button_held_ms;    // BOOT held this long (0 = not held)
} ksui_view_t;

#define KSUI_HOLD_CLAIM_MS 3000
#define KSUI_HOLD_FORGET_MS 10000

// Render into fb (row-major, native-endian RGB565).
void ksui_render(uint16_t *fb, const ksui_view_t *v, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
