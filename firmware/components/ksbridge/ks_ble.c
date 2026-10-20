// NimBLE central: scan for a KS lamp, connect, find its write characteristic,
// and reconnect forever. KS lamps accept only one connection, so close the KS
// app on your phone while the dongle is plugged in.
#include "ks_ble.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

static const char *TAG = "ks_ble";

#define READY_BIT BIT0

static EventGroupHandle_t s_events;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connecting;
static const ks_profile_t *s_profile;
static uint16_t s_svc_start, s_svc_end, s_val_handle;
static uint8_t s_chr_props;
static ble_uuid16_t s_svc_uuid, s_chr_uuid;
static uint8_t s_target_addr[6];
static bool s_have_target_addr;
static volatile uint32_t s_link_gen;
static char s_pending_name[32], s_lamp_name[32];

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

static bool parse_addr(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[5 - i] = (uint8_t)v[i]; // NimBLE stores LSB first
    return true;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        s_val_handle = chr->val_handle;
        s_chr_props = chr->properties;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s_val_handle) {
        bool writable = s_chr_props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP);
        if (!writable) {
            ESP_LOGE(TAG, "characteristic %04X is not writable", s_profile->write_chr);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "ready: %s char %04X handle %u props 0x%02x", s_profile->prefix,
                 s_profile->write_chr, s_val_handle, s_chr_props);
        strlcpy(s_lamp_name, s_pending_name, sizeof s_lamp_name);
        s_link_gen++;
        xEventGroupSetBits(s_events, READY_BIT);
        return 0;
    }
    ESP_LOGE(TAG, "characteristic %04X not found (status %d)", s_profile->write_chr, err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s_svc_end) {
        s_chr_uuid = (ble_uuid16_t)BLE_UUID16_INIT(s_profile->write_chr);
        ble_gattc_disc_chrs_by_uuid(conn, s_svc_start, s_svc_end, &s_chr_uuid.u, on_chr, NULL);
        return 0;
    }
    ESP_LOGE(TAG, "service %04X not found (status %d)", s_profile->service, err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void handle_adv(const struct ble_gap_disc_desc *d)
{
    if (s_connecting || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (s_have_target_addr && memcmp(d->addr.val, s_target_addr, 6) != 0) return;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 || !f.name || !f.name_len) return;
    char name[32];
    size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
    memcpy(name, f.name, n);
    name[n] = 0;

    if (strncmp(name, CONFIG_KSA_NAME_PREFIX, strlen(CONFIG_KSA_NAME_PREFIX)) != 0) return;
    const ks_profile_t *p = ks_profile_for_name(name);
    if (!p) {
        ESP_LOGW(TAG, "'%s' looks like KS but matches no known profile", name);
        return;
    }
    if (p->fmt == KS_FMT_NONE) {
        ESP_LOGW(TAG, "'%s' (%s) is power-only upstream; color commands will be skipped", name, p->prefix);
    }
    ESP_LOGI(TAG, "found '%s' rssi %d -> profile %s", name, d->rssi, p->prefix);
    s_profile = p;
    strlcpy(s_pending_name, name, sizeof s_pending_name);
    s_connecting = true;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(s_own_addr_type, &d->addr, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "connect failed to start: %d", rc);
        s_connecting = false;
        start_scan();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_adv(&event->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) start_scan();
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_svc_start = s_svc_end = s_val_handle = 0;
        s_svc_uuid = (ble_uuid16_t)BLE_UUID16_INIT(s_profile->service);
        ble_gattc_disc_svc_by_uuid(s_conn, &s_svc_uuid.u, on_svc, NULL);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected: reason 0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_val_handle = 0;
        xEventGroupClearBits(s_events, READY_BIT);
        start_scan();
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (ble_gap_disc_active()) return;
    struct ble_gap_disc_params p = {
        .passive = 0,            // active: KS names may only be in the scan response
        .filter_duplicates = 0,  // we need repeats to catch the scan response
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "scanning for '%s*'", CONFIG_KSA_NAME_PREFIX);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset: %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ks_ble_start(void)
{
    s_events = xEventGroupCreate();
    s_have_target_addr = CONFIG_KSA_TARGET_ADDR[0] && parse_addr(CONFIG_KSA_TARGET_ADDR, s_target_addr);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}

const ks_profile_t *ks_ble_wait_ready(int timeout_ms, uint32_t *link_gen)
{
    EventBits_t b = xEventGroupWaitBits(s_events, READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (link_gen) *link_gen = s_link_gen;
    return (b & READY_BIT) ? s_profile : NULL;
}

int ks_ble_write(const uint8_t *data, size_t len)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_val_handle) return -1;
    // Upstream tries write-without-response first when advertised.
    if (s_chr_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
        if (ble_gattc_write_no_rsp_flat(conn, s_val_handle, data, len) == 0) return 0;
    }
    if (s_chr_props & BLE_GATT_CHR_PROP_WRITE) {
        return ble_gattc_write_flat(conn, s_val_handle, data, len, NULL, NULL);
    }
    return -1;
}

void ks_ble_drop(void)
{
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

bool ks_ble_ready(void)
{
    return (xEventGroupGetBits(s_events) & READY_BIT) != 0;
}

void ks_ble_lamp_name(char *out, size_t cap)
{
    strlcpy(out, s_lamp_name, cap); // last connected lamp; kept while reconnecting
}
