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
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_OFF, "starts off");

    ksa_cmd_t c = rgb(255, 136, 0, 80);
    ksb_apply(&m, &c, 0);
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.g == 136 && s.bright == 80, "manual rgb becomes base");

    c = sig(0x11, KSA_SIG_WORKING, "box-a");
    ksb_apply(&m, &c, 100);
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.r == 255 && s.g == 136, "working leaves the lamp alone");

    c = sig(0x22, KSA_SIG_DONE, "box-b");
    ksb_apply(&m, &c, 200);
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_RGB && s.g == 255 && s.r == 0, "done overlays green");

    c = sig(0x11, KSA_SIG_NEEDS_YOU, NULL);
    ksb_apply(&m, &c, 300);
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_EFFECT && s.effect == 0x87, "needs-you beats done (yellow breathing)");
    CHECK(strcmp(m.src[ksb_top_source(&m)].name, "box-a") == 0, "name kept when resent without one");

    c = sig(0x33, KSA_SIG_ERROR, "box-c");
    ksb_apply(&m, &c, 400);
    CHECK(ksb_top_source(&m) >= 0 && m.src[ksb_top_source(&m)].id == 0x11, "needs-you beats a newer error");

    ksb_ack_all(&m, 500);
    ksb_desired(&m, &s);
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
    ksb_desired(&m, &s);
    CHECK(s.kind == KSB_SCENE_OFF, "power off");
    ksb_apply(&m, &on, 2);
    ksb_desired(&m, &s);
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

    printf(fails ? "%d FAILED\n" : "all logic tests passed\n", fails);
    return fails != 0;
}
