// Host harness: read 48 kHz s16le stereo PCM on stdin, print decoded frames.
//   OK <hex> op=<n>     valid frame
//   BAD <hex> err=<n>   frame-shaped but rejected (CRC/version/args)
#include <stdio.h>
#include <stdlib.h>
#include "ksa_dtmf_rx.h"
#include "ksa_frame.h"

static void on_frame(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    ksa_cmd_t cmd;
    int err = ksa_frame_parse(b, n, &cmd);
    printf(err ? "BAD " : "OK ");
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    if (err) printf(" err=%d\n", err);
    else printf(" op=%d\n", cmd.op);
}

int main(int argc, char **argv)
{
    int rate = argc > 1 ? atoi(argv[1]) : 48000;
    static ksa_rx_t rx;
    ksa_rx_init(&rx, rate, on_frame, NULL);
    short in[2 * 480];
    float mono[480];
    size_t got;
    while ((got = fread(in, sizeof(short) * 2, 480, stdin)) > 0) {
        for (size_t i = 0; i < got; i++) mono[i] = (in[2 * i] + in[2 * i + 1]) / 65536.0f;
        ksa_rx_feed(&rx, mono, got);
    }
    // flush trailing silence so a frame at EOF still closes
    float z[480] = {0};
    for (int i = 0; i < 40; i++) ksa_rx_feed(&rx, z, 480);
    fprintf(stderr, "symbols=%u frames=%u\n", rx.symbols_seen, rx.frames_emitted);
    return 0;
}
