// Smart card transport: the dongle is a USB CCID smart card reader with a
// permanently inserted card running the bridge applet. RDP smart card
// redirection carries APDUs from every devbox session; Windows' smart card
// service serialises them, and each devbox gets a "delivered" answer back.
#include "esp_log.h"
#include "ccid_usb.h"
#include "ksb_bridge.h"

void app_main(void)
{
    ksb_bridge_start("CARD");
    ccid_usb_start();
    ESP_LOGI("ksb_card", "smart card reader up");
}
