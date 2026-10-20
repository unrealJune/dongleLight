// Runtime glue shared by both apps: the lamp model, the task that keeps the
// strip in sync with it over BLE, and the screen/LED/button UI.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ksa_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start BLE, the reconcile task and the UI. `transport` is shown on screen.
void ksb_bridge_start(const char *transport);

// Apply a validated command (thread-safe, never blocks for BLE). On success
// *gen (if non-NULL) is the generation that includes it. Returns 0 or -1.
int ksb_bridge_submit(const ksa_cmd_t *cmd, uint16_t *gen);

// Frame-shaped input that failed validation: Lumi frowns briefly.
void ksb_bridge_bad_input(void);

// Card-path status record (format in ksc_card.h).
size_t ksb_bridge_status(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
