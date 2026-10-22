// Multi-lamp NimBLE central with a claim flow. See ks_ble.h.
//
// Concurrency: NimBLE events run on the host task; ks_ble_claim/forget run on
// the UI task and the claim window closes on the esp_timer task. Shared state
// is guarded by s_mux (short critical sections, no NimBLE calls inside).
#include "ks_ble.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "ks_ble";

#define CLAIM_WINDOW_US (4 * 1000 * 1000)
#define NVS_NS "ksb"
#define NVS_KEY_LAMPS "lamps"

typedef struct {
    bool claimed;
    ble_addr_t addr;
    char name[24];
    const ks_profile_t *profile;
    uint16_t conn;
    uint16_t val;
    uint8_t props;
    int rank;            // index of the chosen candidate in profile->write_chr
    bool ready;
    uint32_t gen;
    // readback
    uint16_t nval;       // notify characteristic value handle (0 = none)
    int nrank;
    bool readback;       // notifications enabled
    bool dsc_done;       // descriptor walk finished (CCCD written or given up)
    ks_lamp_state_t state;
    volatile uint32_t state_seq;
    uint32_t state_ms;
} lamp_t;

typedef struct { // persisted per lamp
    uint8_t type;
    uint8_t val[6];
    char name[24];
} saved_lamp_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static lamp_t s_lamp[KSB_MAX_LAMPS];
static volatile int s_n;               // claimed slots 0..s_n-1
static volatile int s_connecting = -1; // slot with a pending ble_gap_connect
static uint8_t s_own_addr_type;
static bool s_synced;

// claim window
static volatile bool s_claim_active, s_claim_auto;
static int8_t s_best_rssi;
static ble_addr_t s_best_addr;
static char s_best_name[24];
static const ks_profile_t *s_best_profile;
static esp_timer_handle_t s_claim_timer;
static uint8_t s_claim_state = KS_CLAIM_IDLE, s_claim_slot;
static uint32_t s_claim_ms;

static uint8_t s_target_addr[6];
static bool s_have_target;

static void start_scan(void);
static int lamp_event(struct ble_gap_event *event, void *arg);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- persistence

static void save_lamps(void)
{
    saved_lamp_t saved[KSB_MAX_LAMPS] = {0};
    int n;
    portENTER_CRITICAL(&s_mux);
    n = s_n;
    for (int i = 0; i < n; i++) {
        saved[i].type = s_lamp[i].addr.type;
        memcpy(saved[i].val, s_lamp[i].addr.val, 6);
        memcpy(saved[i].name, s_lamp[i].name, sizeof saved[i].name);
    }
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (n) nvs_set_blob(h, NVS_KEY_LAMPS, saved, sizeof(saved_lamp_t) * (size_t)n);
    else nvs_erase_key(h, NVS_KEY_LAMPS);
    nvs_commit(h);
    nvs_close(h);
}

static void load_lamps(void)
{
    nvs_handle_t h;
    saved_lamp_t saved[KSB_MAX_LAMPS];
    size_t len = sizeof saved;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, NVS_KEY_LAMPS, saved, &len) == ESP_OK) {
        int n = (int)(len / sizeof(saved_lamp_t));
        for (int i = 0; i < n; i++) {
            lamp_t *l = &s_lamp[i];
            l->claimed = true;
            l->addr.type = saved[i].type;
            memcpy(l->addr.val, saved[i].val, 6);
            memcpy(l->name, saved[i].name, sizeof l->name);
            l->name[sizeof l->name - 1] = 0;
            l->profile = ks_profile_for_name(l->name);
            ESP_LOGI(TAG, "lamp %d: %s (saved)", i + 1, l->name);
        }
        s_n = n;
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- GATT discovery
// One pass over every characteristic; keep the writable one that ranks highest
// in the profile's candidate list (Triones batches differ: FFD5/FFD9/FFE9/FFE5).

static int candidate_rank(const ks_profile_t *p, const ble_uuid_t *u)
{
    if (u->type != BLE_UUID_TYPE_16) return -1;
    uint16_t v = BLE_UUID16(u)->value;
    for (int k = 0; k < KS_MAX_WRITE_CHRS && p->write_chr[k]; k++)
        if (p->write_chr[k] == v) return k;
    return -1;
}

static int notify_rank(const ks_profile_t *p, const ble_uuid_t *u)
{
    if (u->type != BLE_UUID_TYPE_16) return -1;
    uint16_t v = BLE_UUID16(u)->value;
    for (int k = 0; k < KS_MAX_WRITE_CHRS && p->notify_chr[k]; k++)
        if (p->notify_chr[k] == v) return k;
    return -1;
}

static void mark_ready(int i, const char *readback_note)
{
    lamp_t *l = &s_lamp[i];
    portENTER_CRITICAL(&s_mux);
    l->ready = true;
    l->gen++;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "lamp %d ready: %s (%s, write %04X, readback: %s)", i + 1, l->name, l->profile->family,
             l->profile->write_chr[l->rank], readback_note);
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    int i = (int)(intptr_t)arg;
    s_lamp[i].readback = err->status == 0;
    mark_ready(i, err->status == 0 ? "on" : "CCCD write failed");
    return 0;
}

// Walk the attributes after the notify value until its CCCD (0x2902) or the
// next characteristic/service declaration.
static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    int i = (int)(intptr_t)arg;
    if (err->status == 0 && dsc) {
        uint16_t u = dsc->uuid.u.type == BLE_UUID_TYPE_16 ? BLE_UUID16(&dsc->uuid.u)->value : 0;
        if (u == BLE_GATT_DSC_CLT_CFG_UUID16) {
            static const uint8_t enable[2] = {0x01, 0x00};
            s_lamp[i].dsc_done = true;
            if (ble_gattc_write_flat(conn, dsc->handle, enable, sizeof enable, on_cccd_written, arg) != 0) {
                mark_ready(i, "CCCD write not started");
            }
            return BLE_HS_EDONE; // stop the walk
        }
        if (u == 0x2803 || u == 0x2800 || u == 0x2801) { // next char / service: no CCCD
            s_lamp[i].dsc_done = true;
            mark_ready(i, "no CCCD");
            return BLE_HS_EDONE;
        }
        return 0;
    }
    if (s_lamp[i].dsc_done) return 0; // we already stopped the walk
    s_lamp[i].dsc_done = true;
    mark_ready(i, err->status == BLE_HS_EDONE ? "no CCCD" : "descriptor discovery failed");
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    int i = (int)(intptr_t)arg;
    lamp_t *l = &s_lamp[i];
    if (err->status == 0 && chr) {
        int rank = candidate_rank(l->profile, &chr->uuid.u);
        bool writable = chr->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP);
        if (rank >= 0 && writable && rank < l->rank) {
            l->rank = rank;
            l->val = chr->val_handle;
            l->props = chr->properties;
        }
        int nr = notify_rank(l->profile, &chr->uuid.u);
        if (nr >= 0 && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && nr < l->nrank) {
            l->nrank = nr;
            l->nval = chr->val_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE && l->val) {
        // Readback is optional: the lamp is usable either way, and ready is only
        // set once the CCCD step settles so GATT procedures don't overlap.
        if (!l->nval) {
            mark_ready(i, "not supported");
        } else if (ble_gattc_disc_all_dscs(conn, l->nval, 0xFFFF, on_dsc, arg) != 0) {
            mark_ready(i, "descriptor discovery not started");
        }
        return 0;
    }
    ESP_LOGE(TAG, "lamp %d: no writable %s characteristic (status %d)", i + 1, l->profile->family, err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void on_notify(int i, const struct ble_gap_event *event)
{
    lamp_t *l = &s_lamp[i];
    if (event->notify_rx.attr_handle != l->nval) return;
    uint8_t buf[32];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof buf, &len) != 0 && len == 0) return;
    ks_lamp_state_t st;
    if (!ks_status_parse(l->profile, buf, len, &st)) return;
    portENTER_CRITICAL(&s_mux);
    l->state = st;
    l->state_ms = now_ms();
    l->state_seq++;
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------- scanning

static bool adv_name(const struct ble_gap_disc_desc *d, char *out, size_t cap)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 || !f.name || !f.name_len) return false;
    size_t n = f.name_len < cap - 1 ? f.name_len : cap - 1;
    memcpy(out, f.name, n);
    out[n] = 0;
    return true;
}

static void connect_slot(int i)
{
    lamp_t *l = &s_lamp[i];
    if (!l->profile) {
        ESP_LOGW(TAG, "lamp %d: unknown profile for '%s'", i + 1, l->name);
        return;
    }
    s_connecting = i;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(s_own_addr_type, &l->addr, 10000, NULL, lamp_event, (void *)(intptr_t)i);
    if (rc != 0) {
        ESP_LOGW(TAG, "lamp %d: connect failed to start (%d)", i + 1, rc);
        s_connecting = -1;
        start_scan();
    }
}

static void handle_adv(const struct ble_gap_disc_desc *d)
{
    // 1) A claimed lamp we're not connected to: reconnect (by address; the
    //    name may only be in the scan response).
    if (s_connecting < 0) {
        for (int i = 0; i < s_n; i++) {
            if (ble_addr_cmp(&d->addr, &s_lamp[i].addr) == 0 && s_lamp[i].conn == BLE_HS_CONN_HANDLE_NONE) {
                connect_slot(i);
                return;
            }
        }
    }
    // 2) Claim window: remember the strongest unclaimed KS lamp.
    if (!s_claim_active) return;
    if (s_have_target && memcmp(d->addr.val, s_target_addr, 6) != 0) return;
    for (int i = 0; i < s_n; i++)
        if (ble_addr_cmp(&d->addr, &s_lamp[i].addr) == 0) return;
    char name[24];
    if (!adv_name(d, name, sizeof name)) return;
    if (strncmp(name, CONFIG_KSA_NAME_PREFIX, strlen(CONFIG_KSA_NAME_PREFIX)) != 0) return;
    const ks_profile_t *p = ks_profile_for_name(name);
    if (!p) return;
    portENTER_CRITICAL(&s_mux);
    if (!s_best_profile || d->rssi > s_best_rssi) {
        s_best_rssi = d->rssi;
        s_best_addr = d->addr;
        s_best_profile = p;
        memcpy(s_best_name, name, sizeof s_best_name);
    }
    portEXIT_CRITICAL(&s_mux);
}

static int scan_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) handle_adv(&event->disc);
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) start_scan();
    return 0;
}

static bool need_scan(void)
{
    if (s_claim_active) return true;
    for (int i = 0; i < s_n; i++)
        if (s_lamp[i].conn == BLE_HS_CONN_HANDLE_NONE) return true;
    return false;
}

static void start_scan(void)
{
    if (!s_synced || s_connecting >= 0 || ble_gap_disc_active() || !need_scan()) return;
    struct ble_gap_disc_params p = {
        .passive = 0,            // active: KS names may only be in the scan response
        .filter_duplicates = 0,  // we need repeated reports for RSSI and scan responses
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, scan_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "scan failed: %d", rc);
}

// ---------------------------------------------------------------- connections

static int lamp_event(struct ble_gap_event *event, void *arg)
{
    int i = (int)(intptr_t)arg;
    lamp_t *l = &s_lamp[i];
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = -1;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "lamp %d: connect failed (%d)", i + 1, event->connect.status);
            start_scan();
            return 0;
        }
        l->conn = event->connect.conn_handle;
        l->val = 0;
        l->rank = KS_MAX_WRITE_CHRS;
        l->nval = 0;
        l->nrank = KS_MAX_WRITE_CHRS;
        l->readback = false;
        l->dsc_done = false;
        ble_gattc_disc_all_chrs(l->conn, 1, 0xFFFF, on_chr, arg);
        start_scan(); // other claimed lamps may still be missing
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "lamp %d disconnected (0x%x)", i + 1, event->disconnect.reason);
        portENTER_CRITICAL(&s_mux);
        l->conn = BLE_HS_CONN_HANDLE_NONE;
        l->ready = false;
        l->readback = false;
        portEXIT_CRITICAL(&s_mux);
        start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        on_notify(i, event);
        return 0;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------- claiming

static void set_claim_state(uint8_t st, uint8_t slot)
{
    portENTER_CRITICAL(&s_mux);
    s_claim_state = st;
    s_claim_slot = slot;
    s_claim_ms = now_ms();
    portEXIT_CRITICAL(&s_mux);
}

static void open_window(bool automatic)
{
    portENTER_CRITICAL(&s_mux);
    s_best_profile = NULL;
    s_claim_auto = automatic;
    s_claim_active = true;
    portEXIT_CRITICAL(&s_mux);
    if (!automatic) set_claim_state(KS_CLAIM_SEARCHING, (uint8_t)s_n);
    esp_timer_start_once(s_claim_timer, CLAIM_WINDOW_US);
    start_scan();
}

static void close_window(void *arg)
{
    portENTER_CRITICAL(&s_mux);
    s_claim_active = false;
    bool found = s_best_profile != NULL && s_n < KSB_MAX_LAMPS;
    int slot = s_n;
    if (found) {
        lamp_t *l = &s_lamp[slot];
        memset(l, 0, sizeof *l);
        l->claimed = true;
        l->addr = s_best_addr;
        l->profile = s_best_profile;
        l->conn = BLE_HS_CONN_HANDLE_NONE;
        memcpy(l->name, s_best_name, sizeof l->name);
        s_n = slot + 1;
    }
    bool automatic = s_claim_auto;
    int8_t rssi = s_best_rssi;
    portEXIT_CRITICAL(&s_mux);

    if (found) {
        ESP_LOGI(TAG, "claimed lamp %d: %s (rssi %d)", slot + 1, s_lamp[slot].name, rssi);
        save_lamps();
        set_claim_state(KS_CLAIM_OK, (uint8_t)slot);
        start_scan(); // its next advertisement connects it
    } else if (automatic && s_n == 0) {
        open_window(true); // fresh device: keep looking for a first lamp
    } else if (!automatic) {
        ESP_LOGW(TAG, "claim: no unclaimed KS lamp heard");
        set_claim_state(KS_CLAIM_NONE_FOUND, (uint8_t)slot);
    }
}

void ks_ble_claim(void)
{
    if (s_n >= KSB_MAX_LAMPS) {
        set_claim_state(KS_CLAIM_FULL, KSB_MAX_LAMPS - 1);
        return;
    }
    if (s_claim_active) esp_timer_stop(s_claim_timer);
    open_window(false);
}

void ks_ble_forget_all(void)
{
    if (s_claim_active) esp_timer_stop(s_claim_timer);
    uint16_t conns[KSB_MAX_LAMPS];
    int n;
    portENTER_CRITICAL(&s_mux);
    s_claim_active = false;
    n = s_n;
    for (int i = 0; i < n; i++) conns[i] = s_lamp[i].conn;
    memset(s_lamp, 0, sizeof s_lamp);
    for (int i = 0; i < KSB_MAX_LAMPS; i++) s_lamp[i].conn = BLE_HS_CONN_HANDLE_NONE;
    s_n = 0;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++)
        if (conns[i] != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conns[i], BLE_ERR_REM_USER_CONN_TERM);
    save_lamps();
    set_claim_state(KS_CLAIM_FORGOT, 0);
    ESP_LOGW(TAG, "forgot all lamps");
    open_window(true);
}

void ks_ble_claim_status(uint8_t *state, uint8_t *slot, uint32_t *since)
{
    portENTER_CRITICAL(&s_mux);
    *state = s_claim_state;
    *slot = s_claim_slot;
    *since = s_claim_ms;
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------- public API

int ks_ble_lamp_count(void) { return s_n; }

const ks_profile_t *ks_ble_ready(int i, uint32_t *gen)
{
    if (i < 0 || i >= s_n) return NULL;
    const ks_profile_t *p = NULL;
    portENTER_CRITICAL(&s_mux);
    if (s_lamp[i].ready) {
        p = s_lamp[i].profile;
        if (gen) *gen = s_lamp[i].gen;
    }
    portEXIT_CRITICAL(&s_mux);
    return p;
}

int ks_ble_write(int i, const uint8_t *data, size_t len)
{
    if (!ks_ble_ready(i, NULL)) return -1;
    lamp_t *l = &s_lamp[i];
    // Upstream tries write-without-response first when advertised.
    if ((l->props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) && ble_gattc_write_no_rsp_flat(l->conn, l->val, data, len) == 0) {
        return 0;
    }
    if (l->props & BLE_GATT_CHR_PROP_WRITE) return ble_gattc_write_flat(l->conn, l->val, data, len, NULL, NULL);
    return -1;
}

void ks_ble_drop(int i)
{
    if (i < 0 || i >= s_n) return;
    uint16_t c = s_lamp[i].conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

bool ks_ble_has_readback(int i)
{
    return i >= 0 && i < s_n && s_lamp[i].ready && s_lamp[i].readback;
}

int ks_ble_query(int i, ks_lamp_state_t *out, int timeout_ms)
{
    const ks_profile_t *p = ks_ble_ready(i, NULL);
    uint8_t q[KS_MAX_PACKET];
    size_t n;
    if (!p || !s_lamp[i].readback || !(n = ks_pkt_query(p, q))) return -1;
    uint32_t seq0 = s_lamp[i].state_seq;
    if (ks_ble_write(i, q, n) != 0) return -2;
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_lamp[i].state_seq != seq0) {
            portENTER_CRITICAL(&s_mux);
            *out = s_lamp[i].state;
            portEXIT_CRITICAL(&s_mux);
            return 0;
        }
    }
    return -3; // no reply
}

void ks_ble_lamp_name(int i, char *out, size_t cap)
{
    out[0] = 0;
    if (i < 0 || i >= s_n) return;
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_lamp[i].name, cap);
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------- startup

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    if (s_n == 0) {
        ESP_LOGI(TAG, "no lamps claimed: auto-claiming the nearest one");
        open_window(true);
    } else {
        start_scan();
    }
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset: %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool parse_addr(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[5 - i] = (uint8_t)v[i]; // NimBLE stores LSB first
    return true;
}

void ks_ble_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    for (int i = 0; i < KSB_MAX_LAMPS; i++) s_lamp[i].conn = BLE_HS_CONN_HANDLE_NONE;
    load_lamps();
    s_have_target = CONFIG_KSA_TARGET_ADDR[0] && parse_addr(CONFIG_KSA_TARGET_ADDR, s_target_addr);
    const esp_timer_create_args_t t = {.callback = close_window, .name = "ks_claim"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_claim_timer));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
