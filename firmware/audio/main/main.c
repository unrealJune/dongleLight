// Audio transport: the dongle is a USB speaker. Devboxes "play" tone frames on
// one of four interleaved channels; a decoder per channel runs on the same audio,
// so several devboxes can talk at once.
//
//   USB audio (UAC2 48 kHz stereo) -> stream buffer -> decoder task
//     -> ksb_bridge_submit() -> model -> BLE sync task -> strip
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ksa_dtmf_rx.h"
#include "ksa_frame.h"
#include "ksb_bridge.h"
#include "ksbus.h"

#if !CONFIG_KSA_DEBUG_NO_USB
#include "usb_device_uac.h"
#endif

static const char *TAG = "ksa_audio";

#define SAMPLE_RATE 48000
#define CHANNELS 2

#if !CONFIG_KSA_DEBUG_NO_USB
static StreamBufferHandle_t s_audio;
static ksa_rx_t s_rx[KSA_MAX_CHANNELS];

typedef struct {
    uint8_t last[KSA_MAX_FRAME_BYTES];
    size_t last_len;
    TickType_t last_tick;
} dedup_t;
static dedup_t s_dedup[KSA_MAX_CHANNELS];

static void on_frame(const uint8_t *bytes, size_t len, void *ctx)
{
    int ch = (int)(intptr_t)ctx;
    ksa_cmd_t cmd;
    int err = ksa_frame_parse(bytes, len, &cmd);
    if (err) {
        ESP_LOGW(TAG, "ch%d: rejected %u-byte frame (err %d)", ch, (unsigned)len, err);
        ksb_bridge_bad_input();
        return;
    }
    dedup_t *d = &s_dedup[ch];
    TickType_t now = xTaskGetTickCount();
    bool repeat = len == d->last_len && memcmp(bytes, d->last, len) == 0 &&
                  (now - d->last_tick) < pdMS_TO_TICKS(KSA_AUDIO_DEDUP_MS);
    if (repeat) {
        d->last_len = 0; // consumed the second copy of a command we already applied
        return;
    }
    memcpy(d->last, bytes, len);
    d->last_len = len;
    d->last_tick = now;
    if (cmd.op == KSA_OP_HELLO && cmd.args[1] != ch) return;
    if (cmd.op == KSA_OP_RULE && cmd.args[0] != ch) return;
    ESP_LOGI(TAG, "ch%d: op %d", ch, cmd.op);
    if (ksb_bridge_audio_submit(&cmd, ch) != 0) ksb_bridge_bad_input();
}

static esp_err_t uac_output(uint8_t *buf, size_t len, void *ctx)
{
    // UAC speaker task: never block; drop whole chunks if behind (a partial
    // write could split a stereo sample).
    if (xStreamBufferSpacesAvailable(s_audio) >= len) xStreamBufferSend(s_audio, buf, len, 0);
    return ESP_OK;
}

static esp_err_t uac_input(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    static ksbus_tx_t tx;
    static uint32_t quiet;
    static uint8_t payload[KSBUS_MAX_PAYLOAD];
    size_t samples = len / sizeof(int16_t), offset = 0;
    int16_t *pcm = (int16_t *)buf;
    memset(buf, 0, len);
    while (offset < samples) {
        if (quiet) {
            size_t n = samples - offset < quiet ? samples - offset : quiet;
            offset += n;
            quiet -= n;
            continue;
        }
        if (!tx.length) {
            size_t n = ksb_bridge_bus_status(payload, sizeof payload);
            if (!n || ksbus_tx_begin(&tx, payload, n)) break;
        }
        size_t n = ksbus_tx_render(&tx, pcm + offset, samples - offset);
        offset += n;
        if (tx.sample == tx.length * KSBUS_BITS_PER_BYTE * KSBUS_SAMPLES_PER_BIT) {
            tx.length = 0;
            quiet = SAMPLE_RATE / 4; // complete snapshots repeat, even without writes
        }
    }
    *bytes_read = len;
    return ESP_OK;
}

static void decoder_task(void *arg)
{
    static int16_t pcm[480 * CHANNELS]; // 10 ms
    static float mono[480];
    for (;;) {
        size_t got = xStreamBufferReceive(s_audio, pcm, sizeof(pcm), pdMS_TO_TICKS(20));
        if (got > 0) {
            size_t frames = got / (sizeof(int16_t) * CHANNELS);
            for (size_t i = 0; i < frames; i++) mono[i] = ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) / 65536.0f;
            for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_feed(&s_rx[c], mono, frames);
        } else {
            bool active = false;
            for (int c = 0; c < KSA_MAX_CHANNELS; c++)
                active |= s_rx[c].nnib > 0 || s_rx[c].overflow || !s_rx[c].armed;
            if (active) {
                memset(mono, 0, sizeof(mono));
                for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_feed(&s_rx[c], mono, 480);
            }
        }
    }
}
#else
static void demo_task(void *arg)
{
    // Two fake devboxes take turns: with two lamps claimed, each gets its own.
    static const uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 136, 0}};
    for (int i = 0;; i++) {
        ksa_cmd_t c;
        switch (i % 8) {
        case 4:
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 7, .args = {0x41, KSA_SIG_NEEDS_YOU, 'd', 'e', 'm', 'o', '1'}};
            break;
        case 5:
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 7, .args = {0x42, KSA_SIG_DONE, 'd', 'e', 'm', 'o', '2'}};
            break;
        case 6:
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 2, .args = {0x41, KSA_SIG_IDLE}};
            break;
        case 7:
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 2, .args = {0x42, KSA_SIG_IDLE}};
            break;
        default:
            c = (ksa_cmd_t){.op = KSA_OP_RGB, .nargs = 4,
                            .args = {colors[i % 4][0], colors[i % 4][1], colors[i % 4][2], 100}};
        }
        ESP_LOGI(TAG, "demo step %d (op %d)", i % 8, c.op);
        ksb_bridge_submit(&c, NULL);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
#endif

void app_main(void)
{
#if CONFIG_KSA_DEBUG_NO_USB
    ksb_bridge_start("DEBUG");
    ESP_LOGW(TAG, "debug mode: USB audio disabled, running demo");
    xTaskCreate(demo_task, "ksa_demo", 3072, NULL, 4, NULL);
#else
    ksb_bridge_start("AUDIO");
    // ~170 ms of 48 kHz stereo 16-bit; 1 ms trigger level avoids stranding tail chunks.
    s_audio = xStreamBufferCreate(32 * 1024, 48 * CHANNELS * sizeof(int16_t));
    for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_init(&s_rx[c], SAMPLE_RATE, c, on_frame, (void *)(intptr_t)c);
    xTaskCreatePinnedToCore(decoder_task, "ksa_decoder", 4096, NULL, 5, NULL, 1);

    uac_device_config_t cfg = {
        .output_cb = uac_output,
        .input_cb = uac_input,
        // No volume/mute callbacks: Windows sends its volume to the device
        // instead of scaling samples, so the decoders see full-level audio.
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
    ESP_LOGI(TAG, "USB duplex audio up; %d uplinks and shared microphone downlink", KSA_MAX_CHANNELS);
#endif
}
