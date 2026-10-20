#include "ksc_card.h"
#include <string.h>

const uint8_t KSC_AID[KSC_AID_LEN] = {0xF0, 'K', 'S', 'L', 'I', 'G', 'H', 'T'};

// TS=3B, T0=0x87 (TD1 present, 7 historical bytes), TD1=0x81 (TD2 present, T=1),
// TD2=0x01 (T=1), historical "KSLIGHT", TCK = XOR(T0..last historical byte).
static const uint8_t ATR[] = {0x3B, 0x87, 0x81, 0x01, 'K', 'S', 'L', 'I', 'G', 'H', 'T', 0x00};

const uint8_t *ksc_atr(size_t *len)
{
    static uint8_t atr[sizeof ATR];
    static bool ready;
    if (!ready) {
        memcpy(atr, ATR, sizeof ATR);
        uint8_t tck = 0;
        for (size_t i = 1; i < sizeof ATR - 1; i++) tck ^= atr[i];
        atr[sizeof ATR - 1] = tck;
        ready = true;
    }
    *len = sizeof ATR;
    return atr;
}

size_t ksc_status_encode(uint8_t *out, size_t cap, const ksb_model_t *m, uint8_t link,
                         uint16_t desired, uint16_t applied, uint8_t result, const char *lamp)
{
    size_t n = 0;
    if (cap < 9 + KSB_MAX_SOURCES * (4 + KSA_SIGNAL_NAME_MAX) + 1 + 20) return 0;
    out[n++] = 1;
    out[n++] = link;
    out[n++] = (uint8_t)(desired >> 8);
    out[n++] = (uint8_t)desired;
    out[n++] = (uint8_t)(applied >> 8);
    out[n++] = (uint8_t)applied;
    out[n++] = result;
    size_t count_at = n++;
    uint8_t count = 0;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        const ksb_source_t *s = &m->src[i];
        if (!s->used) continue;
        size_t nl = strlen(s->name);
        out[n++] = s->id;
        out[n++] = s->state;
        out[n++] = s->acked;
        out[n++] = (uint8_t)nl;
        memcpy(out + n, s->name, nl);
        n += nl;
        count++;
    }
    out[count_at] = count;
    size_t ll = lamp ? strlen(lamp) : 0;
    if (ll > 19) ll = 19;
    out[n++] = (uint8_t)ll;
    if (ll) memcpy(out + n, lamp, ll);
    return n + ll;
}

// ---------- applet ----------

static size_t sw(uint8_t *resp, size_t n, uint16_t code)
{
    resp[n] = (uint8_t)(code >> 8);
    resp[n + 1] = (uint8_t)code;
    return n + 2;
}

size_t ksc_app_apdu(ksc_app_t *app, const uint8_t *a, size_t len, uint8_t *resp, size_t cap)
{
    if (cap < 2) return 0;
    if (len < 4) return sw(resp, 0, 0x6700);
    uint8_t cla = a[0], ins = a[1];
    size_t lc = 0;
    const uint8_t *data = NULL;
    if (len > 5) {
        lc = a[4];
        if (lc == 0 || 5 + lc > len) return sw(resp, 0, 0x6700); // no extended APDUs
        data = a + 5;
    }

    if (cla == 0x00 && ins == 0xA4) {
        if (a[2] == 0x04 && lc == KSC_AID_LEN && memcmp(data, KSC_AID, KSC_AID_LEN) == 0) {
            app->selected = true;
            resp[0] = 0x01; // applet protocol version
            return sw(resp, 1, 0x9000);
        }
        app->selected = false;
        return sw(resp, 0, 0x6A82); // not our applet: "file not found"
    }
    if (cla == 0x00) return sw(resp, 0, 0x6D00);
    if (cla != 0x80) return sw(resp, 0, 0x6E00);
    if (!app->selected) return sw(resp, 0, 0x6985);

    switch (ins) {
    case KSC_INS_SUBMIT: {
        ksa_cmd_t cmd;
        if (!data || ksa_cmd_from_body(data, lc, &cmd) != 0) return sw(resp, 0, 0x6A80);
        uint16_t gen = 0;
        if (!app->submit || app->submit(&cmd, &gen, app->ctx) != 0) return sw(resp, 0, 0x6F00);
        resp[0] = (uint8_t)(gen >> 8);
        resp[1] = (uint8_t)gen;
        return sw(resp, 2, 0x9000);
    }
    case KSC_INS_STATUS: {
        size_t n = app->status ? app->status(resp, cap - 2, app->ctx) : 0;
        return sw(resp, n, 0x9000);
    }
    default:
        return sw(resp, 0, 0x6D00);
    }
}

// ---------- CCID ----------

enum {
    PC_SET_PARAMETERS = 0x61, PC_ICC_POWER_ON = 0x62, PC_ICC_POWER_OFF = 0x63,
    PC_GET_SLOT_STATUS = 0x65, PC_ESCAPE = 0x6B, PC_GET_PARAMETERS = 0x6C,
    PC_RESET_PARAMETERS = 0x6D, PC_XFR_BLOCK = 0x6F, PC_ABORT = 0x72,
    RDR_DATA_BLOCK = 0x80, RDR_SLOT_STATUS = 0x81, RDR_PARAMETERS = 0x82, RDR_ESCAPE = 0x83,
};
#define ICC_ACTIVE 0x00
#define ICC_INACTIVE 0x01
#define ICC_ABSENT 0x02
#define CMD_FAILED 0x40
#define ERR_CMD_NOT_SUPPORTED 0x00
#define ERR_BAD_SLOT 0x05
#define ERR_ICC_MUTE 0xFE

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

size_t ksc_ccid_msg_len(const uint8_t *in, size_t have)
{
    if (have < 10) return 0;
    uint32_t n = le32(in + 1);
    return n > KSC_MAX_MSG - 10 ? KSC_MAX_MSG + 1 : 10 + n; // oversize -> caller drops
}

static size_t reply(uint8_t *out, uint8_t type, const uint8_t *in, uint8_t status, uint8_t err,
                    uint8_t b9, const uint8_t *data, size_t n)
{
    out[0] = type;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    out[3] = out[4] = 0;
    out[5] = in[5]; // slot
    out[6] = in[6]; // seq
    out[7] = status;
    out[8] = err;
    out[9] = b9;
    if (n) memcpy(out + 10, data, n);
    return 10 + n;
}

size_t ksc_ccid_handle(ksc_ccid_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t need = ksc_ccid_msg_len(in, len);
    if (!need || need > len || cap < KSC_MAX_MSG) return 0;
    uint8_t type = in[0];
    uint8_t icc = c->powered ? ICC_ACTIVE : ICC_INACTIVE;

    if (in[5] != 0) {
        uint8_t rt = type == PC_XFR_BLOCK || type == PC_ICC_POWER_ON ? RDR_DATA_BLOCK
                   : type == PC_ESCAPE ? RDR_ESCAPE : RDR_SLOT_STATUS;
        return reply(out, rt, in, CMD_FAILED | ICC_ABSENT, ERR_BAD_SLOT, 0, NULL, 0);
    }

    switch (type) {
    case PC_ICC_POWER_ON: {
        size_t n;
        const uint8_t *atr = ksc_atr(&n);
        c->powered = true;
        if (c->app) c->app->selected = false;
        return reply(out, RDR_DATA_BLOCK, in, ICC_ACTIVE, 0, 0, atr, n);
    }
    case PC_ICC_POWER_OFF:
        c->powered = false;
        if (c->app) c->app->selected = false;
        return reply(out, RDR_SLOT_STATUS, in, ICC_INACTIVE, 0, 0, NULL, 0);
    case PC_GET_SLOT_STATUS:
    case PC_ABORT:
        return reply(out, RDR_SLOT_STATUS, in, icc, 0, 0, NULL, 0);
    case PC_XFR_BLOCK: {
        if (!c->powered || !c->app) return reply(out, RDR_DATA_BLOCK, in, CMD_FAILED | icc, ERR_ICC_MUTE, 0, NULL, 0);
        uint8_t resp[KSC_MAX_MSG - 10];
        size_t n = ksc_app_apdu(c->app, in + 10, need - 10, resp, sizeof resp);
        return reply(out, RDR_DATA_BLOCK, in, ICC_ACTIVE, 0, 0, resp, n);
    }
    case PC_GET_PARAMETERS:
    case PC_RESET_PARAMETERS:
    case PC_SET_PARAMETERS: {
        // T=1 protocol data: Fi/Di=0x11, TCCKS=0x10 (LRC), guard 0, BWI/CWI 0x4D,
        // clock stop 0, IFSC 254, NAD 0.
        static const uint8_t t1[7] = {0x11, 0x10, 0x00, 0x4D, 0x00, 0xFE, 0x00};
        return reply(out, RDR_PARAMETERS, in, icc, 0, 0x01, t1, sizeof t1);
    }
    case PC_ESCAPE:
        return reply(out, RDR_ESCAPE, in, CMD_FAILED | icc, ERR_CMD_NOT_SUPPORTED, 0, NULL, 0);
    default:
        return reply(out, RDR_SLOT_STATUS, in, CMD_FAILED | icc, ERR_CMD_NOT_SUPPORTED, 0, NULL, 0);
    }
}
