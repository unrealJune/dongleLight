// Render screen states to raw RGB565 frames for test/ui_preview.py.
// usage: ui_preview <scenario> <t_start_ms> <frames> <step_ms>  > frames.raw
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
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
    v->n_lamps = 1;
    strcpy(v->lamps[0].name, "KS03~1A2B");
    v->lamps[0].link = KSUI_LINK_CONNECTED;
    strcpy(v->lamps[1].name, "KS03~77C0");
    v->lamps[1].link = KSUI_LINK_CONNECTED;
    ksa_cmd_t c = {.op = KSA_OP_RGB, .nargs = 4, .args = {255, 136, 0, 80}};
    ksb_apply(&v->model, &c, 0);
    v->model.last_event = KSB_EV_NONE;
    switch (sc) {
    case 0: v->link = KSUI_LINK_SCANNING; v->lamps[0].link = KSUI_LINK_SCANNING; v->transport = "AUDIO"; break;
    case 1: break;
    case 2: { ksa_cmd_t off = {.op = KSA_OP_POWER, .nargs = 1}; ksb_apply(&v->model, &off, 0); v->model.last_event = KSB_EV_NONE; break; }
    case 3: { // FLASH: needs you (!)
        v->n_lamps = 2; ksb_set_lamp_count(&v->model, 2);
        ksa_cmd_t a = sig(1, KSA_SIG_WORKING, "devbox-1"), n = sig(2, KSA_SIG_NEEDS_YOU, "devbox-2");
        ksb_apply(&v->model, &a, 0); ksb_apply(&v->model, &n, t0); break;
    }
    case 4: { // FLASH: error (X)
        ksa_cmd_t w = sig(1, KSA_SIG_WORKING, "devbox-2"), e = sig(4, KSA_SIG_ERROR, "devbox-4");
        ksb_apply(&v->model, &w, 0); ksb_apply(&v->model, &e, t0); break;
    }
    case 5: { // FLASH: acked (heart)
        ksa_cmd_t n = sig(3, KSA_SIG_NEEDS_YOU, "devbox-1");
        ksb_apply(&v->model, &n, 0); ksb_ack_all(&v->model, t0); break;
    }
    case 6: { // FLASH: done (checkmark)
        v->n_lamps = 2; ksb_set_lamp_count(&v->model, 2);
        ksa_cmd_t d = sig(1, KSA_SIG_DONE, "devbox-1");
        ksb_apply(&v->model, &d, t0); break;
    }
    case 7: { // FLASH: working
        v->n_lamps = 2; ksb_set_lamp_count(&v->model, 2);
        ksa_cmd_t w = sig(1, KSA_SIG_WORKING, "devbox-1");
        ksb_apply(&v->model, &w, t0); break;
    }
    case 8: { // Steady after flash: 2 lamps, devbox-2 needs you -> Big Lumi + thick amber border
        v->n_lamps = 2;
        ksb_set_lamp_count(&v->model, 2);
        ksa_cmd_t a = sig(0xA1, KSA_SIG_WORKING, "devbox-1"), b = sig(0xB2, KSA_SIG_WORKING, "devbox-2");
        ksb_apply(&v->model, &a, 0); ksb_apply(&v->model, &b, 0);
        ksa_cmd_t teal2 = {.op = KSA_OP_RGB, .nargs = 5, .args = {0, 200, 180, 70, 0x02}};
        ksb_apply(&v->model, &teal2, 0);
        ksa_cmd_t n = sig(0xB2, KSA_SIG_NEEDS_YOU, "devbox-2");
        ksb_apply(&v->model, &n, 0);
        v->model.last_event = KSB_EV_NONE; break;
    }
    case 9: // FLASH: claiming lamp 2
        v->claim = KSUI_CLAIM_SEARCHING; v->claim_slot = 1; v->claim_ms = 0; break;
    case 10: // FLASH: claimed lamp 2
        v->n_lamps = 2; v->claim = KSUI_CLAIM_OK; v->claim_slot = 1; v->claim_ms = t0; break;
    case 11: { // FLASH: manual color change (teal on lamp 2)
        v->n_lamps = 2; ksb_set_lamp_count(&v->model, 2);
        ksa_cmd_t teal2 = {.op = KSA_OP_RGB, .nargs = 5, .args = {0, 200, 180, 90, 0x02}};
        ksb_apply(&v->model, &teal2, t0); break;
    }
    }
    for (int l = 0; l < KSB_MAX_LAMPS; l++) ksb_desired(&v->model, l, &v->shown[l]);
}

static int run(int argc, char **argv);

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return run(argc, argv);
}

static int run(int argc, char **argv)
{
    int sc = atoi(argv[1]);
    uint32_t t0 = (uint32_t)atoi(argv[2]);
    int frames = atoi(argv[3]), step = atoi(argv[4]);
    static ksui_view_t v;
    static uint16_t fb[KSUI_PIXELS];
    setup(&v, sc, t0);
    v.portrait = argc > 5 && argv[5][0] == 'p';
    for (int f = 0; f < frames; f++) {
        ksui_render(fb, &v, t0 + (uint32_t)(f * step));
        fwrite(fb, sizeof fb, 1, stdout);
    }
    return 0;
}
