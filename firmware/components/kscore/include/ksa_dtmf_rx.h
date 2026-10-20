// Streaming multi-tone receiver + silence-delimited framer. Each channel is a
// DTMF-style 4x4 tone grid; channels interleave on one 85 Hz-spaced grid
// (KSA_GRID_F0 + i*KSA_GRID_STEP, channel c owns i = c, c+4, ..., c+28), so
// several devboxes can talk at once on one USB speaker. Portable C, no ESP-IDF deps,
// so it is unit-tested on the host against codec-mangled audio (see test/).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KSA_WIN_MS 20       // Goertzel window
#define KSA_HOP_MS 10       // window hop (50% overlap)
#define KSA_MIN_RUN 2       // consecutive windows needed to accept a symbol
#define KSA_FRAME_END_MS 150
#define KSA_MAX_NIBBLES 32
#define KSA_MAX_RATE 48000
#define KSA_MAX_CHANNELS 4
#define KSA_GRID_F0 600.0f
#define KSA_GRID_STEP 85.0f

// Called with raw frame bytes (CRC included) whenever a frame closes with an
// even number of nibbles. Validation happens in ksa_frame_parse().
typedef void (*ksa_frame_cb_t)(const uint8_t *bytes, size_t len, void *ctx);

typedef struct {
    int rate, win, hop, channel;
    float coef[8];
    float buf[KSA_MAX_RATE * KSA_WIN_MS / 1000];
    int fill;           // samples in buf
    int run_sym, run_len;
    int armed;          // 1 = a gap was seen since the last accepted symbol
    int quiet_hops;
    uint8_t nib[KSA_MAX_NIBBLES];
    int nnib;
    int overflow;
    ksa_frame_cb_t cb;
    void *ctx;
    // stats for debugging
    uint32_t symbols_seen, frames_emitted;
    uint32_t hops, last_accept_hop;
} ksa_rx_t;

void ksa_rx_init(ksa_rx_t *rx, int sample_rate, int channel, ksa_frame_cb_t cb, void *ctx);

// Tone i (0..7: rows then cols) of a channel, in Hz.
float ksa_tone_hz(int channel, int i);

// Feed mono float samples in [-1, 1].
void ksa_rx_feed(ksa_rx_t *rx, const float *x, size_t n);

// Classify one window. Returns 0..15 for a DTMF nibble, -1 silence, -2 other.
// Exposed for tests.
int ksa_classify(const ksa_rx_t *rx, const float *x, int n);

#ifdef __cplusplus
}
#endif
