// Signal overlay model: priorities, acks, expiry, eviction, packet fallbacks.
#include <stdio.h>
#include <string.h>
#include "ksb_logic.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static ksa_cmd_t sig(uint8_t id, uint8_t state, const char *name)
{
    ksa_cmd_t c = {.op = KSA_OP_SIGNAL, .nargs = 2};
    c.args[0] = id;
    c.args[1] = state;
    if (name) {
        size_t n = strlen(name);
        memcpy(c.args + 2, name, n);
        c.nargs += n;
    }
    return c;
}

static ksa_cmd_t rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t br)
{
    ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4, .args = {r, g, b, br}};
    return c;
}

int main(void)
{
    ksb_model_t m;
    ksb_scene_t s;
    ksb_init(&m);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_OFF, "starts off");

    ksa_cmd_t c = rgb(255, 136, 0, 80);
    ksb_apply(&m, &c, 0);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.g == 136 && s.bright == 80, "manual rgb becomes base");

    c = sig(0x11, KSA_SIG_WORKING, "box-a");
    ksb_apply(&m, &c, 100);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.r == 255 && s.g == 136, "working leaves the lamp alone");

    c = sig(0x22, KSA_SIG_DONE, "box-b");
    ksb_apply(&m, &c, 200);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.g == 255 && s.r == 0, "done overlays green");

    c = sig(0x11, KSA_SIG_NEEDS_YOU, NULL);
    ksb_apply(&m, &c, 300);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_EFFECT && s.effect == 0x87, "needs-you beats done (yellow breathing)");
    CHECK(strcmp(m.src[ksb_top_source(&m)].name, "box-a") == 0, "name kept when resent without one");

    c = sig(0x33, KSA_SIG_ERROR, "box-c");
    ksb_apply(&m, &c, 400);
    CHECK(ksb_top_source(&m) >= 0 && m.src[ksb_top_source(&m)].id == 0x11, "needs-you beats a newer error");

    ksb_ack_all(&m, 500);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.r == 255 && s.g == 136, "ack restores the base scene");

    c = sig(0x33, KSA_SIG_ERROR, NULL);
    ksb_apply(&m, &c, 600);
    CHECK(ksb_top_source(&m) < 0, "re-sending the same error after ack doesn't re-alert");
    c = sig(0x11, KSA_SIG_NEEDS_YOU, NULL);
    ksb_apply(&m, &c, 700);
    CHECK(ksb_top_source(&m) >= 0, "re-sending needs-you DOES re-alert");

    c = sig(0x11, KSA_SIG_IDLE, NULL);
    ksb_apply(&m, &c, 800);
    CHECK(ksb_top_source(&m) < 0, "idle clears the source");

    // done expiry
    c = sig(0x44, KSA_SIG_DONE, "box-d");
    ksb_apply(&m, &c, 1000);
    ksb_tick(&m, 1000 + KSB_DONE_TTL_MS - 1);
    CHECK(ksb_top_source(&m) >= 0, "done still showing before TTL");
    ksb_tick(&m, 1000 + KSB_DONE_TTL_MS);
    CHECK(ksb_top_source(&m) < 0, "done expires after TTL");

    // eviction: fill with 8 working sources plus one needs-you; nothing urgent lost
    ksb_init(&m);
    c = sig(0x01, KSA_SIG_NEEDS_YOU, "urgent");
    ksb_apply(&m, &c, 0);
    for (int i = 0; i < 10; i++) {
        c = sig((uint8_t)(0x40 + i), KSA_SIG_WORKING, NULL);
        ksb_apply(&m, &c, (uint32_t)(10 + i));
    }
    CHECK(ksb_top_source(&m) >= 0 && m.src[ksb_top_source(&m)].id == 0x01, "eviction keeps the urgent source");
    int used = 0;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) used += m.src[i].used;
    CHECK(used == KSB_MAX_SOURCES, "table full but bounded");

    // power off/on restores last scene
    ksb_init(&m);
    c = rgb(1, 2, 3, 50);
    ksb_apply(&m, &c, 0);
    ksa_cmd_t off = {.op = KSA_OP_POWER, .nargs = 1, .args = {0}};
    ksa_cmd_t on = {.op = KSA_OP_POWER, .nargs = 1, .args = {1}};
    ksb_apply(&m, &off, 1);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_OFF, "power off");
    ksb_apply(&m, &on, 2);
    ksb_desired(&m, 0, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.b == 3, "power on restores last color");

    // packets: needs-you on a standard lamp falls back to static yellow
    const ks_profile_t *std = ks_profile_for_name("KS03-x");
    const ks_profile_t *ext = ks_profile_for_name("KS03~x");
    uint8_t pk[2][KS_MAX_PACKET];
    size_t len[2];
    ksb_scene_t ny = {.kind = KSB_SCENE_EFFECT, .effect = 0x87, .speed = 70, .bright = 100};
    int n = ksb_packets(ext, &ny, pk, len);
    CHECK(n == 2 && pk[1][0] == 0x5C && pk[1][2] == 0x87, "ext lamp gets native yellow breathing");
    n = ksb_packets(std, &ny, pk, len);
    CHECK(n == 2 && pk[1][0] == 0x7E && pk[1][4] == 255 && pk[1][5] == 200, "std lamp gets static yellow");
    ksb_scene_t offs = {.kind = KSB_SCENE_OFF};
    n = ksb_packets(std, &offs, pk, len);
    CHECK(n == 1 && pk[0][1] == 0x0F, "off is one power-off packet");

    // frame validation for the new ops
    uint8_t body[] = {0x55, 0x10, KSA_SIG_NEEDS_YOU, 'h', 'i'};
    ksa_cmd_t parsed;
    CHECK(ksa_cmd_from_body(body, sizeof body, &parsed) == 0 && parsed.nargs == 4, "signal body parses");
    body[2] = 9;
    CHECK(ksa_cmd_from_body(body, sizeof body, &parsed) != 0, "bad state rejected");
    body[2] = 1;
    body[3] = 0x07;
    CHECK(ksa_cmd_from_body(body, sizeof body, &parsed) != 0, "non-printable name rejected");
    uint8_t ack[] = {0x56};
    CHECK(ksa_cmd_from_body(ack, 1, &parsed) == 0 && parsed.op == KSA_OP_ACK, "ack parses");

    // ---- two lamps, two devboxes
    ksb_init(&m);
    ksb_set_lamp_count(&m, 2);
    c = rgb(0, 0, 255, 100);
    ksb_apply(&m, &c, 0);
    c = sig(0xA1, KSA_SIG_WORKING, "box-a");
    ksb_apply(&m, &c, 1);
    c = sig(0xB2, KSA_SIG_WORKING, "box-b");
    ksb_apply(&m, &c, 2);
    CHECK(ksb_source_lamp(&m, 0) == 0 && ksb_source_lamp(&m, 1) == 1, "devboxes auto-map to lamps 1 and 2 in order");
    CHECK(m.map_dirty, "mapping marked for saving");
    c = sig(0xB2, KSA_SIG_NEEDS_YOU, NULL);
    ksb_apply(&m, &c, 3);
    ksb_scene_t s0, s1;
    ksb_desired(&m, 0, &s0);
    ksb_desired(&m, 1, &s1);
    CHECK(s0.kind == KSB_SCENE_RGB && s0.b == 255, "lamp 1 keeps its base while box-b needs you");
    CHECK(s1.kind == KSB_SCENE_EFFECT && s1.effect == 0x87, "lamp 2 shows box-b's needs-you");

    // a third devbox has no free lamp: shows on both
    c = sig(0xC3, KSA_SIG_ERROR, "box-c");
    ksb_apply(&m, &c, 4);
    int ci = -1;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) if (m.src[i].used && m.src[i].id == 0xC3) ci = i;
    CHECK(ci >= 0 && ksb_source_lamp(&m, ci) < 0, "third devbox stays unmapped");
    ksb_desired(&m, 0, &s0);
    ksb_desired(&m, 1, &s1);
    CHECK(s0.kind == KSB_SCENE_RGB && s0.r == 255 && s0.g == 0, "unmapped error shows on lamp 1");
    CHECK(s1.kind == KSB_SCENE_EFFECT, "lamp 2 still needs-you (more urgent than error)");

    // per-lamp ack: ack lamp 2 only -> box-b acked, unmapped box-c also acked (it shows there)
    ksa_cmd_t ack2 = {.op = KSA_OP_ACK, .nargs = 1, .args = {0x02}};
    ksb_apply(&m, &ack2, 5);
    ksb_desired(&m, 1, &s1);
    CHECK(s1.kind == KSB_SCENE_RGB && s1.b == 255, "ack on lamp 2 clears it");
    ksb_desired(&m, 0, &s0);
    CHECK(s0.kind == KSB_SCENE_RGB && s0.b == 255, "unmapped error acked too (it was on lamp 2)");

    // per-lamp manual color
    ksa_cmd_t red1 = {.op = KSA_OP_RGB, .nargs = 5, .args = {255, 0, 0, 100, 0x01}};
    ksb_apply(&m, &red1, 6);
    ksb_desired(&m, 0, &s0);
    ksb_desired(&m, 1, &s1);
    CHECK(s0.r == 255 && s0.b == 0 && s1.b == 255 && s1.r == 0, "--lamp 1 colors only lamp 1");
    ksa_cmd_t off2 = {.op = KSA_OP_POWER, .nargs = 2, .args = {0, 0x02}};
    ksa_cmd_t on2 = {.op = KSA_OP_POWER, .nargs = 2, .args = {1, 0x02}};
    ksb_apply(&m, &off2, 7);
    ksb_desired(&m, 1, &s1);
    CHECK(s1.kind == KSB_SCENE_OFF, "lamp 2 off alone");
    ksb_apply(&m, &on2, 8);
    ksb_desired(&m, 1, &s1);
    CHECK(s1.kind == KSB_SCENE_RGB && s1.b == 255, "lamp 2 back on with its own last color");

    // pinning: box-a asks for lamp 2 -> takes it, box-b loses its mapping
    ksa_cmd_t pin = sig(0xA1, KSA_SIG_WORKING, NULL);
    pin.args[1] = (uint8_t)((2 << 4) | KSA_SIG_WORKING);
    ksb_apply(&m, &pin, 9);
    int ai = -1, bi = -1;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        if (m.src[i].used && m.src[i].id == 0xA1) ai = i;
        if (m.src[i].used && m.src[i].id == 0xB2) bi = i;
    }
    CHECK(ksb_source_lamp(&m, ai) == 1 && ksb_source_lamp(&m, bi) < 0, "pinned devbox takes lamp 2 from the other");
    CHECK(ksa_cmd_from_body((uint8_t[]){0x55, 1, (5 << 4) | 1}, 3, &parsed) != 0, "lamp pin beyond 4 rejected");

    // mapping is sticky across a source going idle and coming back
    ksb_init(&m);
    ksb_set_lamp_count(&m, 2);
    c = sig(0x01, KSA_SIG_WORKING, "first");
    ksb_apply(&m, &c, 0);
    c = sig(0x02, KSA_SIG_WORKING, "second");
    ksb_apply(&m, &c, 0);
    c = sig(0x01, KSA_SIG_IDLE, NULL);
    ksb_apply(&m, &c, 1);
    c = sig(0x03, KSA_SIG_WORKING, "third");
    ksb_apply(&m, &c, 2);
    c = sig(0x01, KSA_SIG_DONE, NULL);
    ksb_apply(&m, &c, 3);
    int fi = -1;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) if (m.src[i].used && m.src[i].id == 0x01) fi = i;
    CHECK(ksb_source_lamp(&m, fi) == 0, "a devbox keeps its lamp after going idle");
    CHECK(ksa_cmd_lamps(&on2) == 0x02 && ksa_cmd_lamps(&on) == 0x0F, "lamp mask decoding");

    printf(fails ? "%d FAILED\n" : "all logic tests passed\n", fails);
    return fails != 0;
}
