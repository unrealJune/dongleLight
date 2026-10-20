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

typedef struct {
    uint8_t link;               // ksui_link_t
    char lamp_name[20];         // advertised name, "" if none yet
    const char *transport;      // "AUDIO" / "CARD" / "DEBUG"
    ksb_model_t model;          // snapshot
    ksb_scene_t shown;          // scene last written to the lamp
    uint32_t error_until_ms;    // write failure / bad frame: show a frown until then
} ksui_view_t;

// Render into fb (row-major, native-endian RGB565).
void ksui_render(uint16_t *fb, const ksui_view_t *v, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
