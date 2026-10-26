#include <stdio.h>
#include <string.h>
#include "ksa_dtmf_rx.h"
#include "ksbus.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static ksb_model_t duplex_model;
static uint32_t duplex_now = 1000;
static uint16_t duplex_generation;
static int duplex_pairing_mode;
static ksbus_claim_t duplex_claim;
static struct {
    uint8_t frame[KSA_MAX_FRAME_BYTES];
    size_t length;
    uint32_t seen_ms;
} duplex_dedup[KSA_MAX_CHANNELS];

static void duplex_frame(const uint8_t *bytes, size_t length, void *context)
{
    int channel = (int)(intptr_t)context;
    ksa_cmd_t cmd;
    if (ksa_frame_parse(bytes, length, &cmd)) return;
    bool repeat = length == duplex_dedup[channel].length &&
        !memcmp(bytes, duplex_dedup[channel].frame, length) &&
        duplex_now - duplex_dedup[channel].seen_ms < KSA_AUDIO_DEDUP_MS;
    if (repeat) {
        duplex_dedup[channel].length = 0;
        return;
    }
    memcpy(duplex_dedup[channel].frame, bytes, length);
    duplex_dedup[channel].length = length;
    duplex_dedup[channel].seen_ms = duplex_now;
    if (cmd.op == KSA_OP_HELLO && cmd.args[1] != channel) return;
    if (cmd.op == KSA_OP_RULE && cmd.args[0] != channel) return;
    const ksb_channel_t *owner = &duplex_model.channels[channel];
    if (cmd.op != KSA_OP_HELLO &&
        (!owner->used || duplex_now - owner->seen_ms >= 60000)) return;
    if (cmd.op == KSA_OP_CLAIM && duplex_pairing_mode) {
        if (duplex_claim.state != KS_CLAIM_SEARCHING) {
            duplex_claim.state = duplex_model.n_lamps >= 4 ? KS_CLAIM_FULL : KS_CLAIM_SEARCHING;
            duplex_claim.slot = duplex_model.n_lamps >= 4 ? 3 : duplex_model.n_lamps;
            duplex_claim.since_ms = duplex_now;
        }
        duplex_generation++;
        return;
    }
    if (ksb_apply(&duplex_model, &cmd, duplex_now)) duplex_generation++;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int duplex(int pairing_mode)
{
    // Only the USB/RTOS/BLE boundary is simulated. PCM traverses the production
    // tone decoder, frame parser, state model, snapshot encoder, and return modem.
    static ksa_rx_t rx[KSA_MAX_CHANNELS];
    duplex_pairing_mode = pairing_mode;
    duplex_claim = (ksbus_claim_t){.state = pairing_mode == 4 ? KS_CLAIM_SEARCHING : KS_CLAIM_IDLE,
                                 .since_ms = duplex_now};
    ksb_init(&duplex_model);
    ksb_set_lamp_count(&duplex_model, pairing_mode && pairing_mode != 3 ? 0 : 4);
    for (int ch = 0; ch < KSA_MAX_CHANNELS; ch++)
        ksa_rx_init(&rx[ch], 48000, ch, duplex_frame, (void *)(intptr_t)ch);
    const ksc_lamp_info_t lamps[4] = {
        {.claimed = true, .name = "virtual-1"}, {.claimed = true, .name = "virtual-2"},
        {.claimed = true, .name = "virtual-3"}, {.claimed = true, .name = "virtual-4"}
    };
    uint8_t header[8];
    size_t got;
    while ((got = fread(header, 1, sizeof header, stdin))) {
        if (got != sizeof header) return 3;
        uint32_t length = read_u32(header);
        if (length > 16 * 1024 * 1024 || length % 4) return 4;
        duplex_now += read_u32(header + 4);
        ksb_tick(&duplex_model, duplex_now);
        while (length) {
            int16_t stereo[960];
            float mono[480];
            size_t frames = length / 4 < 480 ? length / 4 : 480;
            if (fread(stereo, 4, frames, stdin) != frames) return 5;
            for (size_t i = 0; i < frames; i++)
                mono[i] = ((float)stereo[2 * i] + stereo[2 * i + 1]) / 65536.0f;
            duplex_now += (uint32_t)(frames / 48);
            for (int ch = 0; ch < KSA_MAX_CHANNELS; ch++) ksa_rx_feed(&rx[ch], mono, frames);
            length -= (uint32_t)frames * 4;
        }
        float silence[480] = {0};
        for (int i = 0; i < 40; i++) {
            duplex_now += 10;
            for (int ch = 0; ch < KSA_MAX_CHANNELS; ch++) ksa_rx_feed(&rx[ch], silence, 480);
        }
        ksb_tick(&duplex_model, duplex_now);
        if (pairing_mode && pairing_mode != 4 && duplex_claim.state == KS_CLAIM_SEARCHING &&
            duplex_now - duplex_claim.since_ms >= 4000) {
            duplex_claim.state = pairing_mode == 2 ? KS_CLAIM_NONE_FOUND : KS_CLAIM_OK;
            duplex_claim.since_ms = duplex_now;
            if (duplex_claim.state == KS_CLAIM_OK)
                ksb_set_lamp_count(&duplex_model, duplex_model.n_lamps + 1);
        }
        uint8_t payload[KSBUS_MAX_PAYLOAD];
        size_t n = ksbus_snapshot(payload, sizeof payload, &duplex_model, 1,
                                 duplex_generation, 0, 1, lamps, duplex_model.n_lamps, duplex_now,
                                 pairing_mode ? &duplex_claim : NULL);
        ksbus_tx_t tx;
        if (!n || ksbus_tx_begin(&tx, payload, n)) return 6;
        uint32_t pcm_bytes = (uint32_t)(tx.length * KSBUS_BITS_PER_BYTE * KSBUS_SAMPLES_PER_BIT + 48000) * 2;
        uint8_t size[4] = {(uint8_t)pcm_bytes, (uint8_t)(pcm_bytes >> 8),
                           (uint8_t)(pcm_bytes >> 16), (uint8_t)(pcm_bytes >> 24)};
        if (fwrite(size, 1, sizeof size, stdout) != sizeof size) return 7;
        int16_t pcm[480];
        while ((n = ksbus_tx_render(&tx, pcm, 480)))
            if (fwrite(pcm, sizeof *pcm, n, stdout) != n) return 7;
        memset(pcm, 0, sizeof pcm);
        for (int i = 0; i < 100; i++)
            if (fwrite(pcm, sizeof *pcm, 480, stdout) != 480) return 7;
        if (fflush(stdout)) return 7;
    }
    return ferror(stdin) ? 8 : 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc > 1 && !strcmp(argv[1], "--duplex")) return duplex(0);
    if (argc > 1 && !strcmp(argv[1], "--duplex-pair")) return duplex(1);
    if (argc > 1 && !strcmp(argv[1], "--duplex-missing")) return duplex(2);
    if (argc > 1 && !strcmp(argv[1], "--duplex-full")) return duplex(3);
    if (argc > 1 && !strcmp(argv[1], "--duplex-busy")) return duplex(4);
    ksb_model_t model;
    ksb_init(&model);
    ksb_set_lamp_count(&model, 2);
    ksa_cmd_t hello = {.op = KSA_OP_HELLO, .nargs = 7, .args = {17, 0, 'b', 'o', 'x', '-', 'a'}};
    ksb_apply(&model, &hello, 100);
    ksa_cmd_t signal = {.op = KSA_OP_STYLED, .nargs = 9, .args = {22, 0x14, 12, 34, 56, 80, 'a', 'b', 'c'}};
    ksb_apply(&model, &signal, 200);
    if (argc > 1 && !strcmp(argv[1], "--ack")) ksb_ack_all(&model, 300);
    ksc_lamp_info_t lamps[2] = {
        {.claimed = true, .connected = true, .readback = true, .verified = true, .name = "Triones-1"},
        {.claimed = true, .name = "KS03~2"}
    };
    uint8_t payload[KSBUS_MAX_PAYLOAD];
    ksbus_claim_t claim = {.state = KS_CLAIM_OK, .slot = 0, .since_ms = 12000};
    size_t length = ksbus_snapshot(payload, sizeof payload, &model, 2, 7, 6, 5, lamps, 2, 12345,
                                  argc > 1 && !strcmp(argv[1], "--pairing") ? &claim : NULL);
    if (!length) return 1;
    if (argc > 1 && (!strcmp(argv[1], "--pcm") || !strcmp(argv[1], "--frame"))) {
        ksbus_tx_t tx;
        if (ksbus_tx_begin(&tx, payload, length)) return 2;
        if (!strcmp(argv[1], "--frame")) {
            fwrite(tx.bytes, 1, tx.length, stdout);
            return 0;
        }
        int16_t pcm[480];
        size_t n;
        while ((n = ksbus_tx_render(&tx, pcm, 480))) fwrite(pcm, sizeof *pcm, n, stdout);
        memset(pcm, 0, sizeof pcm);
        for (int i = 0; i < 20; i++) fwrite(pcm, sizeof *pcm, 480, stdout);
    } else fwrite(payload, 1, length, stdout);
    return 0;
}
