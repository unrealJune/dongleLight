// Lumi emulator shim: drives the real model (ksb_logic.c) and the real screen
// renderer (ksui_render.c) from a browser. Built to WebAssembly by build.sh;
// the page is tools/lumi-sim/lumi.html.
#include <string.h>
#include "ksb_logic.h"
#include "ksui.h"

#define EXPORT __attribute__((visibility("default")))

static ksui_view_t v;
static uint16_t fb[KSUI_W * KSUI_H];
static uint32_t now;

static void refresh(void)
{
    ksb_set_lamp_count(&v.model, v.n_lamps ? v.n_lamps : 1);
    for (int l = 0; l < KSB_MAX_LAMPS; l++) ksb_desired(&v.model, l, &v.shown[l]);
    v.link = KSUI_LINK_SCANNING;
    for (int l = 0; l < v.n_lamps; l++)
        if (v.lamps[l].link == KSUI_LINK_CONNECTED) v.link = KSUI_LINK_CONNECTED;
}

static void apply(ksa_cmd_t *c)
{
    ksb_apply(&v.model, c, now);
    refresh();
}

EXPORT void sim_init(const char *transport_card)
{
    memset(&v, 0, sizeof v);
    ksb_init(&v.model);
    v.transport = transport_card[0] == 'A' ? "AUDIO" : "CARD";
    v.portrait = 1;
    v.n_lamps = 2;
    strcpy(v.lamps[0].name, "Triones-1A2B");
    strcpy(v.lamps[1].name, "Triones-77C0");
    v.lamps[0].link = v.lamps[1].link = KSUI_LINK_CONNECTED;
    ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4, .args = {255, 136, 0, 80}};
    apply(&c);
    v.model.last_event = KSB_EV_NONE;
}

EXPORT uint16_t *sim_render(uint32_t t)
{
    now = t;
    ksb_tick(&v.model, now);
    refresh();
    ksui_render(fb, &v, now);
    return fb;
}

// lamp 0 = every lamp, 1..4 = one lamp
EXPORT void sim_rgb(int r, int g, int b, int bright, int lamp)
{
    ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4, .args = {(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)bright}};
    if (lamp) c.args[c.nargs++] = (uint8_t)(1u << (lamp - 1));
    apply(&c);
}

EXPORT void sim_effect(int effect, int speed, int lamp)
{
    ksa_cmd_t c = {.op = KSA_OP_EFFECT, .nargs = 3, .args = {(uint8_t)effect, (uint8_t)speed, 100}};
    if (lamp) c.args[c.nargs++] = (uint8_t)(1u << (lamp - 1));
    apply(&c);
}

EXPORT void sim_power(int on, int lamp)
{
    ksa_cmd_t c = {.op = KSA_OP_POWER, .nargs = 1, .args = {(uint8_t)(on != 0)}};
    if (lamp) c.args[c.nargs++] = (uint8_t)(1u << (lamp - 1));
    apply(&c);
}

EXPORT void sim_signal(int id, int state, const char *name)
{
    ksa_cmd_t c = {.op = KSA_OP_SIGNAL, .nargs = 2, .args = {(uint8_t)id, (uint8_t)state}};
    size_t n = strlen(name);
    if (n > KSA_SIGNAL_NAME_MAX) n = KSA_SIGNAL_NAME_MAX;
    memcpy(c.args + 2, name, n);
    c.nargs += n;
    apply(&c);
}

EXPORT void sim_ack(void)
{
    ksb_ack_all(&v.model, now);
    refresh();
}

EXPORT void sim_bad_input(void)
{
    v.error_until_ms = now + 1500;
    v.model.last_event = KSB_EV_BAD;
    v.model.last_event_ms = now;
}

// Someone used the IR remote on lamp 1..n: the dongle adopts it (readback).
EXPORT void sim_remote(int lamp, int on, int r, int g, int b)
{
    static const ks_profile_t *p;
    if (!p) p = ks_profile_for_name("Triones-X");
    ks_lamp_state_t st = {.on = on != 0, .pattern = 0x41, .r = (uint8_t)r, .g = (uint8_t)g, .b = (uint8_t)b};
    ksb_scene_t adopted;
    ksb_expect_t e;
    ksb_adopt(&v.model, p, lamp - 1, &st, now, &adopted, &e);
    refresh();
}

EXPORT void sim_lamps(int n, int searching_mask)
{
    v.n_lamps = (uint8_t)n;
    for (int l = 0; l < KSB_MAX_LAMPS; l++)
        v.lamps[l].link = (searching_mask >> l) & 1 ? KSUI_LINK_SCANNING : KSUI_LINK_CONNECTED;
    refresh();
}

EXPORT void sim_button_hold(uint32_t ms) { v.button_held_ms = ms; }

EXPORT void sim_set_portrait(int on) { v.portrait = on != 0; }

EXPORT void sim_claim(int state, int slot)
{
    v.claim = (uint8_t)state;
    v.claim_slot = (uint8_t)slot;
    v.claim_ms = now;
    if (state == KSUI_CLAIM_OK && slot >= v.n_lamps) {
        v.n_lamps = (uint8_t)(slot + 1);
        strcpy(v.lamps[slot].name, slot == 2 ? "Triones-C3D4" : slot == 3 ? "QHM-095F" : "Triones-77C0");
        v.lamps[slot].link = KSUI_LINK_CONNECTED;
    }
    if (state == KSUI_CLAIM_FORGOT) {
        v.n_lamps = 0;
        ksb_clear_map(&v.model);
    }
    refresh();
}

// What each lamp shows (for drawing the strips under the screen): 0xRRGGBB,
// with bit 24 set when it's running an effect, bit 25 when off.
EXPORT uint32_t sim_lamp_color(int lamp)
{
    if (lamp >= v.n_lamps) return 1u << 25;
    const ksb_scene_t *s = &v.shown[lamp];
    uint8_t r, g, b;
    ksb_scene_rgb(s, &r, &g, &b);
    uint32_t flags = s->kind == KSB_SCENE_OFF ? 1u << 25 : s->kind == KSB_SCENE_EFFECT ? 1u << 24 : 0;
    return flags | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

EXPORT int sim_lamp_speed(int lamp) { return v.shown[lamp].speed; }

// scratch buffer so JS can pass short strings without an allocator
static char strbuf[64];
EXPORT char *sim_strbuf(void) { return strbuf; }
