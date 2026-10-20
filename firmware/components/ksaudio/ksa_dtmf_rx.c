#include "ksa_dtmf_rx.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float FREQS[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};

// Keypad position -> nibble. '*' = 0xE, '#' = 0xF.
static const uint8_t KEYMAP[4][4] = {
    {0x1, 0x2, 0x3, 0xA},
    {0x4, 0x5, 0x6, 0xB},
    {0x7, 0x8, 0x9, 0xC},
    {0xE, 0x0, 0xF, 0xD},
};

// Tuning. Ratios are power ratios.
// Only a CPU shortcut: silence and non-DTMF sound both count as "gap" for framing,
// so this can sit near the 16-bit floor. Tones are ~-10 dBFS RMS at full volume.
#define SILENCE_MEAN_POWER 1e-7f // -70 dBFS RMS
#define MIN_TONE_FRACTION 0.55f  // share of window energy in the two DTMF tones
#define MIN_PEAK_RATIO 6.0f      // winner vs runner-up within row/col group (~7.8 dB)
#define MAX_TWIST 6.3f           // |row - col| power ratio (~8 dB)

void ksa_rx_init(ksa_rx_t *rx, int sample_rate, ksa_frame_cb_t cb, void *ctx)
{
    memset(rx, 0, sizeof(*rx));
    if (sample_rate > KSA_MAX_RATE) sample_rate = KSA_MAX_RATE;
    rx->rate = sample_rate;
    rx->win = sample_rate * KSA_WIN_MS / 1000;
    rx->hop = sample_rate * KSA_HOP_MS / 1000;
    for (int i = 0; i < 8; i++) {
        rx->coef[i] = 2.0f * cosf(2.0f * (float)M_PI * FREQS[i] / (float)sample_rate);
    }
    rx->run_sym = -1;
    rx->armed = 1;
    rx->cb = cb;
    rx->ctx = ctx;
}

int ksa_classify(const ksa_rx_t *rx, const float *x, int n)
{
    float energy = 0;
    float s1[8] = {0}, s2[8] = {0};
    for (int i = 0; i < n; i++) {
        float v = x[i];
        energy += v * v;
        for (int k = 0; k < 8; k++) {
            float s0 = v + rx->coef[k] * s1[k] - s2[k];
            s2[k] = s1[k];
            s1[k] = s0;
        }
    }
    if (energy / (float)n < SILENCE_MEAN_POWER) return -1;

    float p[8];
    for (int k = 0; k < 8; k++) {
        p[k] = s1[k] * s1[k] + s2[k] * s2[k] - rx->coef[k] * s1[k] * s2[k];
    }
    int r = 0, c = 4;
    for (int k = 1; k < 4; k++) if (p[k] > p[r]) r = k;
    for (int k = 5; k < 8; k++) if (p[k] > p[c]) c = k;
    float r2 = 0, c2 = 0;
    for (int k = 0; k < 4; k++) if (k != r && p[k] > r2) r2 = p[k];
    for (int k = 4; k < 8; k++) if (k != c && p[k] > c2) c2 = p[k];

    if (p[r] < MIN_PEAK_RATIO * r2 || p[c] < MIN_PEAK_RATIO * c2) return -2;
    if (p[r] > MAX_TWIST * p[c] || p[c] > MAX_TWIST * p[r]) return -2;
    // A pure tone of amplitude A gives Goertzel power (A*N/2)^2 and energy N*A^2/2,
    // so P / (0.5*N*E) is the tone's share of the window energy.
    float frac = (p[r] + p[c]) / (0.5f * (float)n * energy);
    if (frac < MIN_TONE_FRACTION) return -2;
    return KEYMAP[r][c - 4];
}

static void close_frame(ksa_rx_t *rx)
{
    if (!rx->overflow && rx->nnib >= 6 && (rx->nnib % 2) == 0 && rx->cb) {
        uint8_t bytes[KSA_MAX_NIBBLES / 2];
        int n = rx->nnib / 2;
        for (int i = 0; i < n; i++) {
            bytes[i] = (uint8_t)((rx->nib[2 * i] << 4) | rx->nib[2 * i + 1]);
        }
        rx->frames_emitted++;
        rx->cb(bytes, (size_t)n, rx->ctx);
    }
    rx->nnib = 0;
    rx->overflow = 0;
}

static void on_window(ksa_rx_t *rx, int c)
{
    static const int end_hops = KSA_FRAME_END_MS / KSA_HOP_MS;
    if (c >= 0) {
        int last_accepted = rx->nnib ? rx->nib[rx->nnib - 1] : -1;
        if (c == rx->run_sym) {
            rx->run_len++;
        } else {
            rx->run_sym = c;
            rx->run_len = 1;
        }
        // Accept after a gap, or on a direct change of symbol (codec smear can eat
        // short gaps). A repeated nibble therefore always needs a real gap.
        if (rx->run_len == KSA_MIN_RUN && (rx->armed || c != last_accepted)) {
            rx->symbols_seen++;
            if (rx->nnib < KSA_MAX_NIBBLES) {
                rx->nib[rx->nnib++] = (uint8_t)c;
            } else {
                rx->overflow = 1;
            }
            rx->armed = 0;
        }
        rx->quiet_hops = 0;
    } else {
        rx->run_sym = -1;
        rx->run_len = 0;
        rx->armed = 1;
        if (++rx->quiet_hops == end_hops && (rx->nnib || rx->overflow)) {
            close_frame(rx);
        }
    }
}

void ksa_rx_feed(ksa_rx_t *rx, const float *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        rx->buf[rx->fill++] = x[i];
        if (rx->fill == rx->win) {
            on_window(rx, ksa_classify(rx, rx->buf, rx->win));
            memmove(rx->buf, rx->buf + rx->hop, (size_t)(rx->win - rx->hop) * sizeof(float));
            rx->fill = rx->win - rx->hop;
        }
    }
}
