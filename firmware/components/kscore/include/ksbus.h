#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ksc_card.h"

#define KSBUS_MAX_PAYLOAD 512
#define KSBUS_SAMPLES_PER_BIT 80
#define KSBUS_BITS_PER_BYTE 10

typedef struct {
    uint8_t bytes[KSBUS_MAX_PAYLOAD + 128];
    size_t length, sample;
    uint16_t phase;
} ksbus_tx_t;

typedef struct {
    uint8_t state, slot;
    uint32_t since_ms;
} ksbus_claim_t;

uint32_t ksbus_crc32(const uint8_t *data, size_t len);
size_t ksbus_snapshot(uint8_t *out, size_t cap, const ksb_model_t *model,
                     uint8_t link, uint16_t gen, uint16_t applied, uint8_t result,
                     const ksc_lamp_info_t *lamps, int count, uint32_t uptime,
                     const ksbus_claim_t *claim);
int ksbus_tx_begin(ksbus_tx_t *tx, const uint8_t *payload, size_t length);
size_t ksbus_tx_render(ksbus_tx_t *tx, int16_t *pcm, size_t samples);
