#include "ksbus.h"
#include <math.h>
#include <string.h>

uint32_t ksbus_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint16_t x = a;
    uint8_t out = 0;
    while (b) {
        if (b & 1) out ^= (uint8_t)x;
        b >>= 1;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    return out;
}

static void rs_parity(const uint8_t *data, size_t length, uint8_t parity[32])
{
    uint8_t generator[33] = {1}, root = 1;
    for (int degree = 0; degree < 32; degree++) {
        for (int j = degree + 1; j > 0; j--)
            generator[j] ^= gf_mul(generator[j - 1], root);
        root = gf_mul(root, 2);
    }
    memset(parity, 0, 32);
    for (size_t i = 0; i < length; i++) {
        uint8_t factor = data[i] ^ parity[0];
        memmove(parity, parity + 1, 31);
        parity[31] = 0;
        for (int j = 0; j < 32; j++) parity[j] ^= gf_mul(generator[j + 1], factor);
    }
}

size_t ksbus_snapshot(uint8_t *out, size_t cap, const ksb_model_t *m,
                     uint8_t link, uint16_t gen, uint16_t applied, uint8_t result,
                     const ksc_lamp_info_t *lamps, int count, uint32_t uptime,
                     uint8_t include_rules,
                     const ksbus_claim_t *claim)
{
    if (cap < KSBUS_MAX_PAYLOAD) return 0;
    size_t n = ksc_status_encode(out, cap, m, link, gen, applied, result, lamps, count);
    if (!n) return 0;
    out[n++] = 0xB1;
    out[n++] = 3;
    out[n++] = (uint8_t)((include_rules ? 1 : 0) | (claim ? 2 : 0));
    for (int b = 0; b < 4; b++) out[n++] = (uint8_t)(uptime >> (8 * b));
    for (int l = 0; l < KSB_MAX_LAMPS; l++) {
        ksb_scene_t scene;
        ksb_desired(m, l, &scene);
        ksb_scene_rgb(&scene, &out[n], &out[n + 1], &out[n + 2]);
        n += 3;
        out[n++] = scene.kind;
    }
    for (int ch = 0; ch < 4; ch++) {
        out[n++] = m->channels[ch].used;
        out[n++] = m->channels[ch].id;
        memcpy(out + n, m->channels[ch].name, KSA_SIGNAL_NAME_MAX);
        n += KSA_SIGNAL_NAME_MAX;
    }
    if (include_rules)
        for (int ch = 0; ch < 4; ch++)
            for (int p = 0; p < 2; p++)
                for (int s = 0; s < KSA_SIG_COUNT; s++) {
                    const ksb_rule_t *r = &m->rules[ch][p][s];
                    out[n++] = r->r; out[n++] = r->g; out[n++] = r->b;
                    out[n++] = r->bright; out[n++] = r->lamp;
                }
    if (claim) {
        out[n++] = claim->state;
        out[n++] = claim->slot;
        for (int b = 0; b < 4; b++) out[n++] = (uint8_t)(claim->since_ms >> (8 * b));
    }
    return n;
}

int ksbus_tx_begin(ksbus_tx_t *tx, const uint8_t *payload, size_t length)
{
    if (!length || length > KSBUS_MAX_PAYLOAD) return -1;
    memset(tx, 0, sizeof *tx);
    memset(tx->bytes, 0x55, 16);
    tx->bytes[16] = 0xD3; tx->bytes[17] = 0x91;
    tx->bytes[18] = (uint8_t)length; tx->bytes[19] = (uint8_t)(length >> 8);
    uint8_t body[KSBUS_MAX_PAYLOAD + 4];
    memcpy(body, payload, length);
    uint32_t crc = ksbus_crc32(payload, length);
    for (int i = 0; i < 4; i++) body[length + i] = (uint8_t)(crc >> (8 * i));
    tx->length = 20;
    for (size_t at = 0; at < length + 4;) {
        size_t n = length + 4 - at;
        if (n > 223) n = 223;
        memcpy(tx->bytes + tx->length, body + at, n);
        rs_parity(body + at, n, tx->bytes + tx->length + n);
        tx->length += n + 32;
        at += n;
    }
    return 0;
}

size_t ksbus_tx_render(ksbus_tx_t *tx, int16_t *pcm, size_t samples)
{
    static int16_t sine[240];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 240; i++)
            sine[i] = (int16_t)(6554.0f * sinf(6.28318530718f * (float)i / 240.0f));
        ready = 1;
    }
    size_t written = 0, total = tx->length * KSBUS_BITS_PER_BYTE * KSBUS_SAMPLES_PER_BIT;
    while (written < samples && tx->sample < total) {
        size_t bit = tx->sample / KSBUS_SAMPLES_PER_BIT;
        int uart = (int)(bit % KSBUS_BITS_PER_BYTE);
        int value = uart == 9 ? 1 : uart == 0 ? 0 : (tx->bytes[bit / KSBUS_BITS_PER_BYTE] >> (uart - 1)) & 1;
        pcm[written++] = sine[tx->phase];
        // Continuous-phase 1000/1800 Hz avoids the harmonic pair voice codecs distort.
        tx->phase = (uint16_t)((tx->phase + (value ? 9 : 5)) % 240);
        tx->sample++;
    }
    return written;
}
