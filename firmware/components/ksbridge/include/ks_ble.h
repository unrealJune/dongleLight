// NimBLE central for up to KSB_MAX_LAMPS claimed KS lamps.
//
// Lamps are claimed, not auto-picked: a claim scans for a few seconds and takes
// the strongest-signal KS lamp that isn't already claimed. Claimed addresses
// live in NVS; after that the dongle only ever (re)connects to those. A fresh
// device with nothing claimed keeps auto-claiming until it finds one lamp.
// KS lamps take a single connection, so the phone app can't connect while the
// dongle holds a lamp.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksa_protocol.h"
#include "ksb_logic.h"

typedef enum { KS_CLAIM_IDLE, KS_CLAIM_SEARCHING, KS_CLAIM_OK, KS_CLAIM_NONE_FOUND,
               KS_CLAIM_FULL, KS_CLAIM_FORGOT } ks_claim_state_t;

void ks_ble_start(void);

int ks_ble_lamp_count(void);   // claimed lamps (slots 0..n-1)

// Profile of a connected, ready lamp, or NULL. *link_gen (optional) changes on
// every new connection (upstream waits 300 ms after connecting before writing).
const ks_profile_t *ks_ble_ready(int lamp, uint32_t *link_gen);

int ks_ble_write(int lamp, const uint8_t *data, size_t len);  // 0 on success
void ks_ble_drop(int lamp);                                     // reconnect

void ks_ble_lamp_name(int lamp, char *out, size_t cap);

// Readback (lamps that report state, e.g. Triones). Blocking query: sends the
// state request and waits for the reply. 0 ok, <0 unsupported/failed/no reply.
bool ks_ble_has_readback(int lamp);
int ks_ble_query(int lamp, ks_lamp_state_t *out, int timeout_ms);

void ks_ble_claim(void);       // claim one more lamp (strongest nearby)
void ks_ble_forget_all(void);  // unclaim every lamp
void ks_ble_claim_status(uint8_t *state, uint8_t *slot, uint32_t *since_ms);
