// USB CCID reader on TinyUSB, as an application class driver. Message logic
// lives in kscore/ksc_card.c (host-tested); this file is only USB plumbing.
//
// Bulk OUT is read one 64-byte packet at a time and reassembled using the
// CCID header's length, so a message that is an exact multiple of 64 bytes
// can't stall waiting for a short packet. Replies that are an exact multiple
// of 64 get a zero-length packet so the host's read completes.
#include "ccid_usb.h"

#include <string.h>
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#include "ksb_bridge.h"
#include "ksc_card.h"

static const char *TAG = "ccid";

#define EP_OUT 0x01
#define EP_IN 0x81
#define EP_INT 0x82
#define PKT 64

// ---------------------------------------------------------------- descriptors

static const tusb_desc_device_t DEVICE = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, // per interface
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,   // Espressif. Fine for a personal device; change if you distribute it.
    .idProduct = 0x4B53,  // "KS"
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (9 + 9 + 54 + 7 * 3)

static const uint8_t CONFIG[CONFIG_TOTAL_LEN] = {
    // configuration
    9, TUSB_DESC_CONFIGURATION, CONFIG_TOTAL_LEN, 0, 1, 1, 0, 0x80, 50,
    // interface: smart card class, 3 endpoints
    9, TUSB_DESC_INTERFACE, 0, 0, 3, 0x0B, 0x00, 0x00, 0,
    // CCID class descriptor (CCID rev 1.10, section 5.1)
    54, 0x21,
    0x10, 0x01,             // bcdCCID 1.10
    0x00,                   // bMaxSlotIndex
    0x07,                   // bVoltageSupport 5V/3V/1.8V
    0x02, 0x00, 0x00, 0x00, // dwProtocols: T=1
    0xA0, 0x0F, 0x00, 0x00, // dwDefaultClock 4000 kHz
    0xA0, 0x0F, 0x00, 0x00, // dwMaximumClock
    0x00,                   // bNumClockSupported
    0x00, 0x2A, 0x00, 0x00, // dwDataRate 10752 bps
    0x00, 0x2A, 0x00, 0x00, // dwMaxDataRate
    0x00,                   // bNumDataRatesSupported
    0xFE, 0x00, 0x00, 0x00, // dwMaxIFSD 254
    0x00, 0x00, 0x00, 0x00, // dwSynchProtocols
    0x00, 0x00, 0x00, 0x00, // dwMechanical
    0xFE, 0x04, 0x02, 0x00, // dwFeatures: automatic everything + short APDU level
    0x0F, 0x01, 0x00, 0x00, // dwMaxCCIDMessageLength 271
    0xFF,                   // bClassGetResponse: echo
    0xFF,                   // bClassEnvelope: echo
    0x00, 0x00,             // wLcdLayout: none
    0x00,                   // bPINSupport: none
    0x01,                   // bMaxCCIDBusySlots
    // endpoints
    7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, PKT, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, PKT, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_INT, TUSB_XFER_INTERRUPT, 8, 0, 16,
};

static const char *STRINGS[] = {"DIY", "KS Light Bridge", "KSB0001"};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&DEVICE; }

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return CONFIG;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    size_t n;
    if (index == 0) {
        buf[1] = 0x0409; // English
        n = 1;
    } else {
        if (index > sizeof(STRINGS) / sizeof(STRINGS[0])) return NULL;
        const char *s = STRINGS[index - 1];
        n = strlen(s);
        if (n > 31) n = 31;
        for (size_t i = 0; i < n; i++) buf[1 + i] = (uint8_t)s[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

// ---------------------------------------------------------------- class driver

static ksc_app_t s_app;
static ksc_ccid_t s_ccid = {.app = &s_app};
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_rx[KSC_MAX_MSG + PKT];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_tx[KSC_MAX_MSG + 16];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_notify[2];
static size_t s_rx_len, s_tx_len;
static bool s_zlp_pending;

static void arm_out(uint8_t rhport)
{
    if (s_rx_len + PKT > sizeof s_rx) s_rx_len = 0; // overflow: drop the partial message
    usbd_edpt_xfer(rhport, EP_OUT, s_rx + s_rx_len, PKT);
}

static void ccid_init(void) {}
static bool ccid_deinit(void) { return true; }

static void ccid_reset(uint8_t rhport)
{
    (void)rhport;
    s_rx_len = s_tx_len = 0;
    s_zlp_pending = false;
    s_ccid.powered = false;
    s_app.selected = false;
}

static uint16_t ccid_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != 0x0B) return 0;
    uint16_t len = sizeof(tusb_desc_interface_t);
    uint8_t const *p = tu_desc_next(itf);
    int eps = 0;
    while (len < max_len && eps < itf->bNumEndpoints) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            if (!usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p)) return 0;
            eps++;
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    s_rx_len = 0;
    arm_out(rhport);
    // A card is always present: tell the host so it powers it on.
    s_notify[0] = 0x50; // RDR_to_PC_NotifySlotChange
    s_notify[1] = 0x03; // slot 0: present, changed
    usbd_edpt_xfer(rhport, EP_INT, s_notify, sizeof s_notify);
    ESP_LOGI(TAG, "reader opened");
    return len;
}

static bool ccid_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    // Class requests: ABORT (1) is acknowledged; clock/data-rate queries (2, 3)
    // are unsupported since bNumClockSupported/bNumDataRatesSupported are 0.
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && req->bRequest == 0x01) {
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(rhport, req);
        return true;
    }
    return false; // stall
}

static bool ccid_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n)
{
    if (ep == EP_OUT) {
        s_rx_len += n;
        size_t need = ksc_ccid_msg_len(s_rx, s_rx_len);
        if (need > KSC_MAX_MSG) {
            s_rx_len = 0; // oversize or garbage
        } else if (need && s_rx_len >= need) {
            s_tx_len = ksc_ccid_handle(&s_ccid, s_rx, need, s_tx, sizeof s_tx);
            s_rx_len = 0;
            if (s_tx_len) {
                s_zlp_pending = (s_tx_len % PKT) == 0;
                usbd_edpt_xfer(rhport, EP_IN, s_tx, (uint16_t)s_tx_len);
                return true; // re-arm OUT once the reply is out
            }
        }
        arm_out(rhport);
    } else if (ep == EP_IN) {
        if (s_zlp_pending) {
            s_zlp_pending = false;
            usbd_edpt_xfer(rhport, EP_IN, NULL, 0);
            return true;
        }
        arm_out(rhport);
    }
    return true;
}

static const usbd_class_driver_t DRIVER = {
    .name = "CCID",
    .init = ccid_init,
    .deinit = ccid_deinit,
    .reset = ccid_reset,
    .open = ccid_open,
    .control_xfer_cb = ccid_control,
    .xfer_cb = ccid_xfer,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = 1;
    return &DRIVER;
}

// ---------------------------------------------------------------- applet hooks

static int app_submit(const ksa_cmd_t *cmd, uint16_t *gen, void *ctx)
{
    (void)ctx;
    return ksb_bridge_submit(cmd, gen);
}

static size_t app_status(uint8_t *out, size_t cap, void *ctx)
{
    (void)ctx;
    return ksb_bridge_status(out, cap);
}

static void usb_task(void *arg)
{
    for (;;) tud_task(); // blocks internally on the device event queue
}

void ccid_usb_start(void)
{
    s_app.submit = app_submit;
    s_app.status = app_status;
    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
    };
    usb_phy_handle_t h;
    ESP_ERROR_CHECK(usb_new_phy(&phy, &h));
    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init failed");
        return;
    }
    xTaskCreate(usb_task, "tud", 4096, NULL, 5, NULL);
}
