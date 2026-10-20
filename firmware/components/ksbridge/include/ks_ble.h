#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksa_protocol.h"

// Starts NimBLE and keeps trying to hold one connection to a matching KS lamp.
void ks_ble_start(void);

// Wait until a lamp is connected and its write characteristic is found.
// Returns the lamp's profile, or NULL on timeout. *link_gen changes on every
// new connection (upstream waits 300 ms after connecting before writing).
const ks_profile_t *ks_ble_wait_ready(int timeout_ms, uint32_t *link_gen);

// Write one packet to the lamp's write characteristic. 0 on success.
int ks_ble_write(const uint8_t *data, size_t len);

// Drop the current link (e.g. after a failed write); scanning resumes.
void ks_ble_drop(void);

// True while connected with the write characteristic found.
bool ks_ble_ready(void);

// Advertised name of the current/last lamp ("" before the first connection).
void ks_ble_lamp_name(char *out, size_t cap);
