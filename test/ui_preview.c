// Render screen states to raw RGB565 frames for test/ui_preview.py.
// usage: ui_preview <scenario> <t_start_ms> <frames> <step_ms>  > frames.raw
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ksui.h"

static ksa_cmd_t sig(uint8_t id, uint8_t st, const char *name)
{
    ksa_cmd_t c = {.op = KSA_OP_SIGNAL, .nargs = 2 + strlen(name)};
    c.args[0] = id; c.args[1] = st;
    memcpy(c.args + 2, name, strlen(name));
    return c;
}

static void setup(ksui_view_t *v, int sc, uint32_t t0)
{
    memset(v, 0, sizeof *v);
    ksb_init(&v->model);
    v->transport = "CARD";
    v->link = KSUI_LINK_CONNECTED;
    strcpy(v->lamp_name, "KS03~1A2B");
    ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4, .args = {255, 136, 0, 80}};
    ksb_apply(&v->model, &c, 0);
    v->model.last_event = KSB_EV_NONE;
    switch (sc) {
    case 0: v->link = KSUI_LINK_SCANNING; v->lamp_name[0] = 0; v->transport = "AUDIO"; break;
    case 1: break;
    case 2: { ksa_cmd_t off = {.op = KSA_OP_POWER, .nargs = 1}; ksb_apply(&v->model, &off, 0); v->model.last_event = KSB_EV_NONE; break; }
    case 3: {
        ksa_cmd_t a = sig(1, KSA_SIG_WORKING, "devbox-2"), b = sig(2, KSA_SIG_DONE, "devbox-3"), n = sig(3, KSA_SIG_NEEDS_YOU, "devbox-1");
        ksb_apply(&v->model, &a, 0); ksb_apply(&v->model, &b, 0); ksb_apply(&v->model, &n, 0);
        v->model.last_event = KSB_EV_NONE; break;
    }
    case 4: {
        ksa_cmd_t e = sig(4, KSA_SIG_ERROR, "devbox-4"), w = sig(1, KSA_SIG_WORKING, "devbox-2");
        ksb_apply(&v->model, &w, 0); ksb_apply(&v->model, &e, t0); break; // sparkles at t0
    }
    case 5: {
        ksa_cmd_t n = sig(3, KSA_SIG_NEEDS_YOU, "devbox-1");
        ksb_apply(&v->model, &n, 0); ksb_ack_all(&v->model, t0); break; // heart at t0
    }
    case 6: {
        ksa_cmd_t fx = {.op = KSA_OP_EFFECT, .nargs = 3, .args = {0x89, 40, 100}};
        ksb_apply(&v->model, &fx, 0);
        const char *names[] = {"build-01", "build-02", "train", "notebook", "ci-box", "scratch"};
        for (int i = 0; i < 6; i++) { ksa_cmd_t s = sig((uint8_t)(10 + i), KSA_SIG_WORKING, names[i]); ksb_apply(&v->model, &s, (uint32_t)i); }
        v->model.last_event = KSB_EV_NONE; break;
    }
    }
    ksb_desired(&v->model, &v->shown);
}

static int run(char **argv);

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    return run(argv);
}

static int run(char **argv)
{
    int sc = atoi(argv[1]);
    uint32_t t0 = (uint32_t)atoi(argv[2]);
    int frames = atoi(argv[3]), step = atoi(argv[4]);
    static ksui_view_t v;
    static uint16_t fb[KSUI_W * KSUI_H];
    setup(&v, sc, t0);
    for (int f = 0; f < frames; f++) {
        ksui_render(fb, &v, t0 + (uint32_t)(f * step));
        fwrite(fb, sizeof fb, 1, stdout);
    }
    return 0;
}
