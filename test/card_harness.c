// Card harness: a virtual bridge (model + applet + CCID) driven over stdin.
// Each input line is a hex APDU; it is wrapped in a CCID XfrBlock, processed,
// and the response data+SW is printed as hex. Lines starting with '!' are raw
// CCID messages (hex) whose full RDR_to_PC reply is printed.
#include <stdio.h>
#include <string.h>
#include "ksc_card.h"

static ksb_model_t model;
static uint16_t gen;

static int submit(const ksa_cmd_t *cmd, uint16_t *g, void *ctx)
{
    (void)ctx;
    if (!ksb_apply(&model, cmd, 1000)) return -1;
    *g = ++gen;
    return 0;
}

static size_t status(uint8_t *out, size_t cap, void *ctx)
{
    (void)ctx;
    ksc_lamp_info_t lamps[2] = {
        {.claimed = true, .connected = true, .readback = true, .verified = true, .name = "Triones-1A2B"},
        {.claimed = true, .connected = false, .name = "KS03~77C0"},
    };
    return ksc_status_encode(out, cap, &model, 2, gen, gen, KSC_RESULT_OK, lamps, 2);
}

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    unsigned v;
    while (*s && *s != '\n' && sscanf(s, "%2x", &v) == 1) { out[n++] = (uint8_t)v; s += 2; }
    return n;
}

int main(void)
{
    ksb_init(&model);
    ksb_set_lamp_count(&model, 2);
    ksc_app_t app = {.submit = submit, .status = status};
    ksc_ccid_t ccid = {.app = &app};
    uint8_t seq = 0, msg[KSC_MAX_MSG + 16], out[KSC_MAX_MSG + 16];
    // power on first, like a reader would
    uint8_t on[10] = {0x62, 0, 0, 0, 0, 0, seq++, 0, 0, 0};
    ksc_ccid_handle(&ccid, on, 10, out, sizeof out);
    char line[1200];
    while (fgets(line, sizeof line, stdin)) {
        size_t n, r;
        if (line[0] == '!') {
            n = unhex(line + 1, msg);
            r = ksc_ccid_handle(&ccid, msg, n, out, sizeof out);
            for (size_t i = 0; i < r; i++) printf("%02x", out[i]);
            printf("\n");
            continue;
        }
        size_t alen = unhex(line, msg + 10);
        msg[0] = 0x6F;
        msg[1] = (uint8_t)alen; msg[2] = (uint8_t)(alen >> 8); msg[3] = msg[4] = 0;
        msg[5] = 0; msg[6] = seq++; msg[7] = msg[8] = msg[9] = 0;
        r = ksc_ccid_handle(&ccid, msg, 10 + alen, out, sizeof out);
        if (r < 10 || out[0] != 0x80 || (out[7] & 0x40)) { printf("CCIDERR %02x %02x\n", out[7], out[8]); continue; }
        for (size_t i = 10; i < r; i++) printf("%02x", out[i]);
        printf("\n");
    }
    return 0;
}
