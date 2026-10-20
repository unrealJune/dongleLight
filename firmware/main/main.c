// KS audio bridge: a USB speaker that listens for DTMF lamp commands and relays
// them to a KS Bluetooth LED controller.
//
//   USB audio (UAC2, 48 kHz stereo) -> stream buffer -> decoder task
//     -> frame queue -> worker task -> NimBLE write to the lamp
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ks_ble.h"
#include "ksa_dtmf_rx.h"
#include "ksa_frame.h"
#include "ksa_protocol.h"
#include "status_led.h"

#if !CONFIG_KSA_DEBUG_NO_USB
#include "usb_device_uac.h"
#endif

static const char *TAG = "ksa";

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define DEDUP_MS 2000          // ks_tx.py sends each frame twice by default
#define WRITE_SPACING_MS 100   // upstream hub spacing
#define SETTLE_MS 300          // upstream delay after connecting
#define READY_TIMEOUT_MS 8000

static QueueHandle_t s_cmds;

#if !CONFIG_KSA_DEBUG_NO_USB
static StreamBufferHandle_t s_audio;
static ksa_rx_t s_rx;

// ---------- frames -> commands ----------

static void on_frame(const uint8_t *bytes, size_t len, void *ctx)
{
    static uint8_t last[KSA_MAX_FRAME_BYTES];
    static size_t last_len;
    static TickType_t last_tick;

    ksa_cmd_t cmd;
    int err = ksa_frame_parse(bytes, len, &cmd);
    if (err) {
        ESP_LOGW(TAG, "rejected %u-byte frame (err %d)", (unsigned)len, err);
        status_led_flash(LED_BAD_FRAME, 150);
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (len == last_len && memcmp(bytes, last, len) == 0 && (now - last_tick) < pdMS_TO_TICKS(DEDUP_MS)) {
        last_tick = now;
        return; // repeat copy of a command we already queued
    }
    memcpy(last, bytes, len);
    last_len = len;
    last_tick = now;

    status_led_flash(LED_FRAME, 120);
    if (xQueueSend(s_cmds, &cmd, 0) != pdTRUE) {
        ksa_cmd_t dropped;
        xQueueReceive(s_cmds, &dropped, 0); // full: newest command wins
        xQueueSend(s_cmds, &cmd, 0);
    }
}
#endif

// ---------- commands -> KS packets ----------

typedef struct {
    uint8_t data[KS_MAX_PACKET];
    size_t len;
} packet_t;

// Returns packet count. Color/effect/white are preceded by power-on, as upstream does.
static int build_packets(const ks_profile_t *p, const ksa_cmd_t *c, packet_t out[2])
{
    int n = 0;
    switch (c->op) {
    case KSA_OP_POWER:
        out[n].len = ks_pkt_power(c->args[0] != 0, out[n].data);
        return 1;
    case KSA_OP_RGB:
        out[1].len = ks_pkt_rgb(p, c->args[0], c->args[1], c->args[2], c->args[3], out[1].data);
        break;
    case KSA_OP_EFFECT:
        out[1].len = ks_pkt_effect(p, c->args[0], c->args[1], c->args[2], out[1].data);
        break;
    case KSA_OP_WHITE:
        out[1].len = ks_pkt_white(p, c->args[0], out[1].data);
        break;
    default:
        return 0;
    }
    if (!out[1].len) {
        ESP_LOGW(TAG, "op %d not supported by profile %s", c->op, p->prefix);
        return 0;
    }
    out[0].len = ks_pkt_power(true, out[0].data);
    return 2;
}

static bool send_packets(const packet_t *pk, int n)
{
    for (int i = 0; i < n; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(WRITE_SPACING_MS));
        if (ks_ble_write(pk[i].data, pk[i].len) != 0) return false;
    }
    return true;
}

static void worker_task(void *arg)
{
    uint32_t seen_gen = 0;
    ksa_cmd_t cmd;
    for (;;) {
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        for (int attempt = 0; attempt < 2; attempt++) {
            uint32_t gen;
            const ks_profile_t *p = ks_ble_wait_ready(READY_TIMEOUT_MS, &gen);
            if (!p) {
                ESP_LOGW(TAG, "no lamp connected; dropping op %d", cmd.op);
                status_led_flash(LED_ERROR, 500);
                break;
            }
            if (gen != seen_gen) {
                vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
                seen_gen = gen;
            }
            packet_t pk[2];
            int n = build_packets(p, &cmd, pk);
            if (!n) {
                status_led_flash(LED_ERROR, 300);
                break;
            }
            if (send_packets(pk, n)) {
                ESP_LOGI(TAG, "sent op %d (%d packet%s)", cmd.op, n, n > 1 ? "s" : "");
                break;
            }
            // Cheap BLE controllers often drop the link after some writes:
            // reconnect and replay once. Commands are idempotent.
            ESP_LOGW(TAG, "write failed, reconnecting (attempt %d)", attempt + 1);
            status_led_flash(LED_ERROR, 300);
            ks_ble_drop();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// ---------- audio ----------

#if !CONFIG_KSA_DEBUG_NO_USB
static esp_err_t uac_output(uint8_t *buf, size_t len, void *ctx)
{
    // Runs in the UAC speaker task. Never block it; drop whole chunks if we fall
    // behind (a partial write could split a stereo sample and swap L/R alignment).
    if (xStreamBufferSpacesAvailable(s_audio) >= len) {
        xStreamBufferSend(s_audio, buf, len, 0);
    }
    return ESP_OK;
}

static void decoder_task(void *arg)
{
    static int16_t pcm[480 * CHANNELS]; // 10 ms
    static float mono[480];
    for (;;) {
        size_t got = xStreamBufferReceive(s_audio, pcm, sizeof(pcm), portMAX_DELAY);
        size_t frames = got / (sizeof(int16_t) * CHANNELS);
        for (size_t i = 0; i < frames; i++) {
            mono[i] = ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) / 65536.0f;
        }
        ksa_rx_feed(&s_rx, mono, frames);
    }
}
#else
static void demo_task(void *arg)
{
    static const uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 136, 0}};
    for (int i = 0;; i++) {
        ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4};
        memcpy(c.args, colors[i % 4], 3);
        c.args[3] = 100;
        ESP_LOGI(TAG, "demo: rgb %02x%02x%02x", c.args[0], c.args[1], c.args[2]);
        xQueueSend(s_cmds, &c, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
#endif

void app_main(void)
{
    status_led_init();
    s_cmds = xQueueCreate(4, sizeof(ksa_cmd_t));
    ks_ble_start();
    xTaskCreate(worker_task, "ks_worker", 4096, NULL, 5, NULL);

#if CONFIG_KSA_DEBUG_NO_USB
    ESP_LOGW(TAG, "debug mode: USB audio disabled, cycling colors");
    xTaskCreate(demo_task, "ks_demo", 3072, NULL, 4, NULL);
#else
    // ~170 ms of 48 kHz stereo 16-bit; the decoder easily keeps up.
    s_audio = xStreamBufferCreate(32 * 1024, 480 * CHANNELS * sizeof(int16_t));
    ksa_rx_init(&s_rx, SAMPLE_RATE, on_frame, NULL);
    xTaskCreate(decoder_task, "ks_decoder", 4096, NULL, 6, NULL);

    uac_device_config_t cfg = {
        .output_cb = uac_output,
        // No volume/mute callbacks: Windows sends its volume to the device
        // instead of scaling samples, so the decoder sees full-level audio.
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
    ESP_LOGI(TAG, "USB speaker up; listening for commands");
#endif
}
