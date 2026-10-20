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

#if !CONFIG_KSA_DEBUG_NO_USB
#include "usb_device_uac.h"
#endif

static const char *TAG = "ksa_audio";

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define DEDUP_MS 2000 // ks.py sends each frame twice

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
                  (now - d->last_tick) < pdMS_TO_TICKS(DEDUP_MS);
    memcpy(d->last, bytes, len);
    d->last_len = len;
    d->last_tick = now;
    if (repeat) return; // second copy of a command we already applied
    ESP_LOGI(TAG, "ch%d: op %d", ch, cmd.op);
    if (ksb_bridge_submit(&cmd, NULL) != 0) ksb_bridge_bad_input();
}

static esp_err_t uac_output(uint8_t *buf, size_t len, void *ctx)
{
    // UAC speaker task: never block; drop whole chunks if behind (a partial
    // write could split a stereo sample).
    if (xStreamBufferSpacesAvailable(s_audio) >= len) xStreamBufferSend(s_audio, buf, len, 0);
    return ESP_OK;
}

static void decoder_task(void *arg)
{
    static int16_t pcm[480 * CHANNELS]; // 10 ms
    static float mono[480];
    for (;;) {
        size_t got = xStreamBufferReceive(s_audio, pcm, sizeof(pcm), portMAX_DELAY);
        size_t frames = got / (sizeof(int16_t) * CHANNELS);
        for (size_t i = 0; i < frames; i++) mono[i] = ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) / 65536.0f;
        for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_feed(&s_rx[c], mono, frames);
    }
}
#else
static void demo_task(void *arg)
{
    static const uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 136, 0}};
    for (int i = 0;; i++) {
        ksa_cmd_t c;
        switch (i % 6) {
        case 4: // fake an agent asking for you; press BOOT to ack
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 7, .args = {0x42, KSA_SIG_NEEDS_YOU, 'd', 'e', 'm', 'o', '!'}};
            break;
        case 5:
            c = (ksa_cmd_t){.op = KSA_OP_SIGNAL, .nargs = 2, .args = {0x42, KSA_SIG_IDLE}};
            break;
        default:
            c = (ksa_cmd_t){.op = KSA_OP_RGB, .nargs = 4,
                            .args = {colors[i % 4][0], colors[i % 4][1], colors[i % 4][2], 100}};
        }
        ESP_LOGI(TAG, "demo step %d (op %d)", i % 6, c.op);
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
    // ~170 ms of 48 kHz stereo 16-bit; the decoders easily keep up.
    s_audio = xStreamBufferCreate(32 * 1024, 480 * CHANNELS * sizeof(int16_t));
    for (int c = 0; c < KSA_MAX_CHANNELS; c++) ksa_rx_init(&s_rx[c], SAMPLE_RATE, c, on_frame, (void *)(intptr_t)c);
    xTaskCreate(decoder_task, "ksa_decoder", 4096, NULL, 6, NULL);

    uac_device_config_t cfg = {
        .output_cb = uac_output,
        // No volume/mute callbacks: Windows sends its volume to the device
        // instead of scaling samples, so the decoders see full-level audio.
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
    ESP_LOGI(TAG, "USB speaker up; listening on %d channels", KSA_MAX_CHANNELS);
#endif
}
