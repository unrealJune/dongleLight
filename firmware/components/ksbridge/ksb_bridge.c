// Keeps every claimed lamp in sync with the model. Commands only touch the
// model (fast, from any task); one task diffs desired vs written per lamp and
// does all BLE writes, so reconnects heal by re-applying the current state.
#include "ksb_bridge.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ks_ble.h"
#include "ksb_logic.h"
#include "ksc_card.h"
#include "ksb_ui_internal.h"

static const char *TAG = "ksb";

#define WRITE_SPACING_MS 100 // upstream hub spacing
#define SETTLE_MS 300        // upstream delay after connecting
#define VERIFY_DELAY_MS 400  // light.py waits 0.5 s before reading back
#define QUERY_TIMEOUT_MS 1500
#define POLL_MS 5000         // look for hand-made changes this often

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static ksb_model_t s_model;
static uint16_t s_gen, s_applied;
static uint8_t s_result = KSC_RESULT_NONE;
static uint32_t s_error_until;
static uint32_t s_boot_ms;
static volatile uint32_t s_button_held_ms;
static volatile bool s_verified[KSB_MAX_LAMPS];
static const char *s_transport = "";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- mapping persistence

static void save_map(uint8_t valid, const uint8_t ids[KSB_MAX_LAMPS])
{
    uint8_t blob[1 + KSB_MAX_LAMPS] = {valid};
    memcpy(blob + 1, ids, KSB_MAX_LAMPS);
    nvs_handle_t h;
    if (nvs_open("ksb", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "map", blob, sizeof blob);
    nvs_commit(h);
    nvs_close(h);
}

static void load_map(void)
{
    uint8_t blob[1 + KSB_MAX_LAMPS];
    size_t len = sizeof blob;
    nvs_handle_t h;
    if (nvs_open("ksb", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "map", blob, &len) == ESP_OK && len == sizeof blob) {
        s_model.lamp_src_valid = blob[0];
        memcpy(s_model.lamp_src, blob + 1, KSB_MAX_LAMPS);
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- API

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

static int lamp_info(ksc_lamp_info_t out[KSB_MAX_LAMPS], bool *any)
{
    int n = ks_ble_lamp_count();
    *any = false;
    for (int l = 0; l < n; l++) {
        out[l].claimed = true;
        out[l].connected = ks_ble_ready(l, NULL) != NULL;
        out[l].readback = ks_ble_has_readback(l);
        out[l].verified = out[l].readback && s_verified[l];
        *any |= out[l].connected;
        ks_ble_lamp_name(l, out[l].name, sizeof out[l].name);
    }
    return n;
}

size_t ksb_bridge_status(uint8_t *out, size_t cap)
{
    ksc_lamp_info_t lamps[KSB_MAX_LAMPS];
    bool any;
    int n = lamp_info(lamps, &any);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = ksc_status_encode(out, cap, &s_model, any ? KSUI_LINK_CONNECTED : KSUI_LINK_SCANNING,
                                   s_gen, s_applied, s_result, lamps, n);
    xSemaphoreGive(s_lock);
    return len;
}

void ksb_bridge_view(ksui_view_t *v)
{
    uint32_t now = now_ms();
    ksc_lamp_info_t lamps[KSB_MAX_LAMPS];
    bool any;
    int n = lamp_info(lamps, &any);
    v->transport = s_transport;
    v->n_lamps = (uint8_t)n;
    for (int l = 0; l < n; l++) {
        memcpy(v->lamps[l].name, lamps[l].name, sizeof v->lamps[l].name);
        v->lamps[l].link = lamps[l].connected ? KSUI_LINK_CONNECTED : KSUI_LINK_SCANNING;
    }
    v->link = any ? KSUI_LINK_CONNECTED : (now - s_boot_ms < 1200 ? KSUI_LINK_BOOT : KSUI_LINK_SCANNING);
    ks_ble_claim_status(&v->claim, &v->claim_slot, &v->claim_ms);
    v->button_held_ms = s_button_held_ms;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v->model = s_model;
    for (int l = 0; l < KSB_MAX_LAMPS; l++) ksb_desired(&s_model, l, &v->shown[l]);
    v->error_until_ms = s_error_until;
    xSemaphoreGive(s_lock);
}

void ksb_bridge_button(uint32_t held_ms, bool released)
{
    s_button_held_ms = released ? 0 : held_ms;
    if (!released) return;
    if (held_ms >= KSB_HOLD_FORGET_MS) {
        ESP_LOGW(TAG, "button: forget all lamps and devbox mappings");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ksb_clear_map(&s_model);
        xSemaphoreGive(s_lock);
        ks_ble_forget_all();
    } else if (held_ms >= KSB_HOLD_CLAIM_MS) {
        ESP_LOGI(TAG, "button: claim another lamp");
        ks_ble_claim();
    } else {
        ESP_LOGI(TAG, "button: acknowledge");
        ksa_cmd_t ack = {.op = KSA_OP_ACK};
        ksb_bridge_submit(&ack, NULL);
    }
    xTaskNotifyGive(s_task);
}

// ---------------------------------------------------------------- reconcile

typedef struct {
    ksb_scene_t written;
    bool valid;
    uint32_t link;
    ksb_expect_t expect;       // what the lamp should report (readback lamps)
    ks_lamp_state_t prev;      // last polled state
    bool have_prev;
    uint32_t last_poll_ms;
} lamp_sync_t;

// Per-lamp outcome of one pass, worst wins when combined.
typedef enum { OUT_NONE, OUT_VERIFIED, OUT_OK, OUT_UNSUPPORTED, OUT_MISMATCH, OUT_FAILED } outcome_t;

static int send_scene(int l, const ks_profile_t *p, const ksb_scene_t *sc, bool *partial)
{
    uint8_t pk[2][KS_MAX_PACKET];
    size_t len[2];
    int np = ksb_packets(p, sc, pk, len);
    for (int i = 0; i < np; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(WRITE_SPACING_MS));
        if (ks_ble_write(l, pk[i], len[i]) != 0) return -1;
    }
    *partial = sc->kind != KSB_SCENE_OFF && np < 2;
    return 0;
}

// Read the lamp back after a write. Retries the write once on a mismatch, as a
// dropped packet is the likeliest cause.
static outcome_t verify(int l, const ks_profile_t *p, const ksb_scene_t *sc, lamp_sync_t *s)
{
    ksb_expect(p, sc, &s->expect);
    s->have_prev = false;
    if (!ks_ble_has_readback(l)) return OUT_OK;
    for (int attempt = 0; attempt < 2; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(VERIFY_DELAY_MS));
        ks_lamp_state_t st;
        if (ks_ble_query(l, &st, QUERY_TIMEOUT_MS) != 0) {
            ESP_LOGW(TAG, "lamp %d: no state reply; unverified", l + 1);
            return OUT_OK;
        }
        s->last_poll_ms = now_ms();
        switch (ksb_check(&s->expect, &st)) {
        case KSB_CHECK_MATCH:
            s_verified[l] = true;
            return OUT_VERIFIED;
        case KSB_CHECK_UNSURE:
            return OUT_OK;
        case KSB_CHECK_MISMATCH:
            if (attempt == 0) {
                ESP_LOGW(TAG, "lamp %d: reports %s %02x%02x%02x, expected %s %02x%02x%02x; resending", l + 1,
                         st.on ? "on" : "off", st.r, st.g, st.b, s->expect.on ? "on" : "off", s->expect.r,
                         s->expect.g, s->expect.b);
                bool partial;
                if (send_scene(l, p, sc, &partial) != 0) return OUT_FAILED;
                continue;
            }
            // Still different: accept what the lamp shows as the baseline, so the
            // poller doesn't mistake our own failed write for a hand-made change.
            ESP_LOGE(TAG, "lamp %d: still doesn't match after resend", l + 1);
            s->expect = (ksb_expect_t){.on = st.on, .has_rgb = st.on, .r = st.r, .g = st.g, .b = st.b};
            s_verified[l] = false;
            return OUT_MISMATCH;
        }
    }
    return OUT_OK;
}

// Idle check for changes made with the IR remote or the phone app.
static void poll_lamp(int l, const ks_profile_t *p, lamp_sync_t *s)
{
    ks_lamp_state_t st;
    s->last_poll_ms = now_ms();
    if (ks_ble_query(l, &st, QUERY_TIMEOUT_MS) != 0) return;
    bool drift = ksb_drift(&s->expect, &st, s->have_prev ? &s->prev : NULL);
    s->prev = st;
    s->have_prev = true;
    if (!drift) {
        s_verified[l] = ksb_check(&s->expect, &st) == KSB_CHECK_MATCH;
        return;
    }
    ksb_scene_t adopted;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ksb_adopt(&s_model, p, l, &st, now_ms(), &adopted, &s->expect);
    xSemaphoreGive(s_lock);
    s->written = adopted; // the lamp already shows it: nothing to write
    s->valid = true;
    s->have_prev = false;
    s_verified[l] = true;
    ESP_LOGI(TAG, "lamp %d changed by hand (%s %02x%02x%02x pattern %02x); adopted", l + 1, st.on ? "on" : "off",
             st.r, st.g, st.b, st.pattern);
}

static void reconcile_task(void *arg)
{
    lamp_sync_t sync[KSB_MAX_LAMPS] = {0};
    int last_count = -1;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        int n = ks_ble_lamp_count();
        if (n != last_count) {
            // Lamps added or forgotten: slots may hold different lamps now.
            memset(sync, 0, sizeof sync);
            for (int l = 0; l < KSB_MAX_LAMPS; l++) s_verified[l] = false;
            last_count = n;
        }

        ksb_scene_t want[KSB_MAX_LAMPS];
        uint8_t map_ids[KSB_MAX_LAMPS], map_valid = 0;
        bool map_dirty;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ksb_set_lamp_count(&s_model, (uint8_t)(n ? n : 1));
        ksb_tick(&s_model, now_ms());
        for (int l = 0; l < KSB_MAX_LAMPS; l++) ksb_desired(&s_model, l, &want[l]);
        uint16_t gen = s_gen;
        bool pending = s_applied != gen;
        map_dirty = s_model.map_dirty;
        if (map_dirty) {
            s_model.map_dirty = false;
            map_valid = s_model.lamp_src_valid;
            memcpy(map_ids, s_model.lamp_src, sizeof map_ids);
        }
        xSemaphoreGive(s_lock);
        if (map_dirty) save_map(map_valid, map_ids);

        bool any_ready = false;
        outcome_t worst = OUT_NONE;
        for (int l = 0; l < n; l++) {
            uint32_t link = 0;
            const ks_profile_t *p = ks_ble_ready(l, &link);
            lamp_sync_t *s = &sync[l];
            if (!p) {
                s->valid = false; // re-apply once it's back
                s_verified[l] = false;
                continue;
            }
            any_ready = true;
            if (link != s->link) {
                s->link = link;
                s->valid = false;
                s_verified[l] = false;
                vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
            }
            if (s->valid && ksb_scene_eq(&s->written, &want[l])) {
                if (!pending && ks_ble_has_readback(l) && now_ms() - s->last_poll_ms >= POLL_MS) poll_lamp(l, p, s);
                continue;
            }

            bool partial = false;
            outcome_t out;
            if (send_scene(l, p, &want[l], &partial) != 0) {
                ESP_LOGW(TAG, "lamp %d: write failed; reconnecting", l + 1);
                ks_ble_drop(l);
                s->valid = false;
                out = OUT_FAILED;
            } else {
                s->written = want[l];
                s->valid = true;
                out = verify(l, p, &want[l], s);
                if (out == OUT_FAILED) {
                    ks_ble_drop(l);
                    s->valid = false;
                } else if (partial && out < OUT_UNSUPPORTED) {
                    out = OUT_UNSUPPORTED;
                }
                ESP_LOGI(TAG, "lamp %d updated (kind %d, outcome %d)", l + 1, want[l].kind, out);
            }
            if (out > worst) worst = out;
        }

        if (pending || worst >= OUT_MISMATCH) {
            uint8_t result = !any_ready             ? KSC_RESULT_NO_LAMP
                           : worst == OUT_FAILED     ? KSC_RESULT_WRITE_FAILED
                           : worst == OUT_MISMATCH   ? KSC_RESULT_MISMATCH
                           : worst == OUT_UNSUPPORTED ? KSC_RESULT_UNSUPPORTED
                           : worst == OUT_VERIFIED   ? KSC_RESULT_VERIFIED
                                                     : KSC_RESULT_OK;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_applied = gen;
            s_result = result;
            if (worst >= OUT_MISMATCH) s_error_until = now_ms() + 1500;
            xSemaphoreGive(s_lock);
        }
        if (worst == OUT_FAILED) vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void ksb_bridge_start(const char *transport)
{
    s_transport = transport;
    s_boot_ms = now_ms();
    s_lock = xSemaphoreCreateMutex();
    ksb_init(&s_model);
    ks_ble_start(); // also initialises NVS
    load_map();
    xTaskCreate(reconcile_task, "ksb_sync", 4096, NULL, 5, &s_task);
    ksb_ui_start();
}
