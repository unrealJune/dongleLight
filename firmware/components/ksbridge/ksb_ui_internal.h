#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ksui.h"

#define KSB_HOLD_CLAIM_MS KSUI_HOLD_CLAIM_MS   // hold BOOT this long: claim another lamp
#define KSB_HOLD_FORGET_MS KSUI_HOLD_FORGET_MS // this long: forget all lamps and devbox mappings

void ksb_ui_start(void);
void ksb_bridge_view(ksui_view_t *v);
// Button state from the UI task: called every frame while held, once on release.
void ksb_bridge_button(uint32_t held_ms, bool released);
