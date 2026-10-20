// Host harness: read 48 kHz s16le stereo PCM on stdin, decode every channel,
// print frames:  OK <ch> <hex> op=<n>   |   BAD <ch> <hex> err=<n>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "ksa_dtmf_rx.h"
#include "ksa_frame.h"

static void on_frame(const uint8_t *b, size_t n, void *ctx)
{
    int ch = (int)(intptr_t)ctx;
    ksa_cmd_t cmd;
    int err = ksa_frame_parse(b, n, &cmd);
    printf(err ? "BAD %d " : "OK %d ", ch);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    if (err) printf(" err=%d\n", err);
    else printf(" op=%d\n", cmd.op);
}

int main(void)
{
    static ksa_rx_t rx[KSA_MAX_CHANNELS];
    for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_init(&rx[c], 48000, c, on_frame, (void *)(intptr_t)c);
    short in[2 * 480];
    float mono[480];
    size_t got;
    while ((got = fread(in, sizeof(short) * 2, 480, stdin)) > 0) {
        for (size_t i = 0; i < got; i++) mono[i] = (in[2 * i] + in[2 * i + 1]) / 65536.0f;
        for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_feed(&rx[c], mono, got);
    }
    float z[480] = {0};
    for (int i = 0; i < 40; i++)
        for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_feed(&rx[c], z, 480);
    unsigned sym = 0, fr = 0;
    for (int c = 0; c < KSA_MAX_CHANNELS; c++) { sym += rx[c].symbols_seen; fr += rx[c].frames_emitted; }
    fprintf(stderr, "symbols=%u frames=%u\n", sym, fr);
    return 0;
}
