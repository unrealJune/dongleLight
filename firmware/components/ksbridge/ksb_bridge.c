// Keeps the strip in sync with the model. Commands only touch the model (fast,
// from any task); one task diffs desired vs written and does all BLE writes.
// That makes reconnects self-healing: a new link re-applies the current state.
#include "ksb_bridge.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ks_ble.h"
#include "ksb_logic.h"
#include "ksc_card.h"
#include "ksb_ui_internal.h"

static const char *TAG = "ksb";

#define WRITE_SPACING_MS 100 // upstream hub spacing
#define SETTLE_MS 300        // upstream delay after connecting

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static ksb_model_t s_model;
static uint16_t s_gen, s_applied;
static uint8_t s_result = KSC_RESULT_NONE;
static uint32_t s_error_until;
static uint32_t s_boot_ms;
static const char *s_transport = "";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

int ksb_bridge_submit(const ksa_cmd_t *cmd, uint16_t *gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = ksb_apply(&s_model, cmd, now_ms());
    if (ok) s_gen++;
    if (gen) *gen = s_gen;
    xSemaphoreGive(s_lock);
    if (ok) xTaskNotifyGive(s_task);
    return ok ? 0 : -1;
}

void ksb_bridge_bad_input(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_error_until = now_ms() + 1500;
    s_model.last_event = KSB_EV_BAD;
    s_model.last_event_ms = now_ms();
    xSemaphoreGive(s_lock);
}

size_t ksb_bridge_status(uint8_t *out, size_t cap)
{
    char lamp[24];
    ks_ble_lamp_name(lamp, sizeof lamp);
    uint8_t link = ks_ble_ready() ? KSUI_LINK_CONNECTED : KSUI_LINK_SCANNING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = ksc_status_encode(out, cap, &s_model, link, s_gen, s_applied, s_result, lamp);
    xSemaphoreGive(s_lock);
    return n;
}

void ksb_bridge_view(ksui_view_t *v)
{
    uint32_t now = now_ms();
    v->transport = s_transport;
    v->link = ks_ble_ready() ? KSUI_LINK_CONNECTED : (now - s_boot_ms < 1200 ? KSUI_LINK_BOOT : KSUI_LINK_SCANNING);
    ks_ble_lamp_name(v->lamp_name, sizeof v->lamp_name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v->model = s_model;
    ksb_desired(&s_model, &v->shown); // Lumi shows what the strip should be showing
    v->error_until_ms = s_error_until;
    xSemaphoreGive(s_lock);
}

void ksb_bridge_ack(void)
{
    ksa_cmd_t ack = {.op = KSA_OP_ACK};
    ksb_bridge_submit(&ack, NULL);
}

static void finish(uint16_t gen, uint8_t result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_applied = gen;
    s_result = result;
    if (result == KSC_RESULT_WRITE_FAILED) s_error_until = now_ms() + 1500;
    xSemaphoreGive(s_lock);
}

static void reconcile_task(void *arg)
{
    ksb_scene_t written = {0};
    bool written_valid = false;
    uint32_t seen_link = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        ksb_scene_t want;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ksb_tick(&s_model, now_ms());
        ksb_desired(&s_model, &want);
        uint16_t gen = s_gen;
        bool pending = s_applied != gen;
        xSemaphoreGive(s_lock);

        uint32_t link = 0;
        const ks_profile_t *p = ks_ble_ready() ? ks_ble_wait_ready(0, &link) : NULL;
        if (!p) {
            written_valid = false;            // re-apply everything once a lamp shows up
            if (pending) finish(gen, KSC_RESULT_NO_LAMP);
            continue;
        }
        if (link != seen_link) {
            seen_link = link;
            written_valid = false;
            vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        }
        if (written_valid && ksb_scene_eq(&written, &want)) {
            if (pending) finish(gen, KSC_RESULT_OK);
            continue;
        }

        uint8_t pk[2][KS_MAX_PACKET];
        size_t len[2];
        int n = ksb_packets(p, &want, pk, len);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            if (i) vTaskDelay(pdMS_TO_TICKS(WRITE_SPACING_MS));
            ok = ks_ble_write(pk[i], len[i]) == 0;
        }
        if (!ok) {
            // Cheap BLE controllers drop links; reconnect and the next pass replays.
            ESP_LOGW(TAG, "write failed; reconnecting");
            ks_ble_drop();
            written_valid = false;
            finish(gen, KSC_RESULT_WRITE_FAILED);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        written = want;
        written_valid = true;
        bool unsupported = want.kind != KSB_SCENE_OFF && n < 2;
        ESP_LOGI(TAG, "lamp updated (kind %d, %d packet%s)", want.kind, n, n > 1 ? "s" : "");
        finish(gen, unsupported ? KSC_RESULT_UNSUPPORTED : KSC_RESULT_OK);
    }
}

void ksb_bridge_start(const char *transport)
{
    s_transport = transport;
    s_boot_ms = now_ms();
    s_lock = xSemaphoreCreateMutex();
    ksb_init(&s_model);
    ks_ble_start();
    xTaskCreate(reconcile_task, "ksb_sync", 4096, NULL, 5, &s_task);
    ksb_ui_start();
}
