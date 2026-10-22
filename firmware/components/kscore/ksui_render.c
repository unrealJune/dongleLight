#include "ksui.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t ksui_font5x7[95][5];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t BG = {14, 17, 30};
static const rgb_t DIM = {118, 124, 150};
static const rgb_t TEXT = {232, 234, 245};
static const rgb_t RULE = {40, 46, 68};
static const rgb_t AMBER = {255, 176, 0};
static const rgb_t STATE_COLORS[KSA_SIG_COUNT] = {
    {110, 110, 130}, {90, 150, 255}, {80, 230, 120}, {255, 70, 70}, {255, 176, 0},
};

static uint16_t *s_fb;
static int s_w = KSUI_W, s_h = KSUI_H;

static uint16_t pack(rgb_t c) { return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3)); }

static rgb_t unpack(uint16_t v)
{
    rgb_t c = {(uint8_t)((v >> 8) & 0xF8), (uint8_t)((v >> 3) & 0xFC), (uint8_t)((v << 3) & 0xF8)};
    return c;
}

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    rgb_t c = {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)};
    return c;
}

static void px(int x, int y, rgb_t c)
{
    if (x >= 0 && x < s_w && y >= 0 && y < s_h) s_fb[y * s_w + x] = pack(c);
}

static void px_blend(int x, int y, rgb_t c, float a)
{
    if (x < 0 || x >= s_w || y < 0 || y >= s_h) return;
    s_fb[y * s_w + x] = pack(mix(unpack(s_fb[y * s_w + x]), c, a));
}

static void rect(int x, int y, int w, int h, rgb_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, c);
}

// Anti-aliased filled disc; alpha scales the whole disc.
static void disc(float cx, float cy, float r, rgb_t c, float alpha)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float cov = r + 0.5f - d;
            if (cov > 0) px_blend(x, y, c, alpha * (cov > 1 ? 1 : cov));
        }
    }
}

static void ellipse(float cx, float cy, float rx, float ry, rgb_t c)
{
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++) {
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) px(x, y, c);
        }
    }
}

static int text(int x, int y, const char *s, rgb_t c)
{
    for (; *s; s++, x += 6) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        for (int col = 0; col < 5; col++) {
            uint8_t bits = ksui_font5x7[ch - 0x20][col];
            for (int row = 0; row < 8; row++)
                if (bits & (1u << row)) px(x + col, y + row, c);
        }
    }
    return x;
}

static void text_clip(int x, int y, const char *s, int max_chars, rgb_t c)
{
    char buf[24];
    size_t n = strlen(s);
    if ((int)n > max_chars) {
        memcpy(buf, s, (size_t)max_chars - 1);
        buf[max_chars - 1] = '.';
        buf[max_chars] = 0;
        s = buf;
    }
    text(x, y, s, c);
}

static float wave(uint32_t now, uint32_t period_ms)
{
    return 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (float)(now % period_ms) / (float)period_ms);
}

// Small 4-point sparkle.
static void sparkle(int x, int y, rgb_t c, float a)
{
    px_blend(x, y, c, a);
    px_blend(x - 1, y, c, a * 0.7f);
    px_blend(x + 1, y, c, a * 0.7f);
    px_blend(x, y - 1, c, a * 0.7f);
    px_blend(x, y + 1, c, a * 0.7f);
    px_blend(x - 2, y, c, a * 0.3f);
    px_blend(x + 2, y, c, a * 0.3f);
    px_blend(x, y - 2, c, a * 0.3f);
    px_blend(x, y + 2, c, a * 0.3f);
}

static void heart(int x, int y, rgb_t c)
{
    static const uint8_t rows[5] = {0x36, 0x7F, 0x7F, 0x3E, 0x1C}; // 7 wide
    for (int r = 0; r < 5; r++)
        for (int i = 0; i < 7; i++)
            if (rows[r] & (0x40 >> i)) px(x + i, y + r, c);
    px(x + 3, y + 5, c);
}

// Lamp Lumi mirrors: the one showing the most urgent signal, else lamp 1.
static int focus_lamp(const ksui_view_t *v)
{
    int top = ksb_top_source(&v->model);
    int l = top >= 0 ? ksb_source_lamp(&v->model, top) : -1;
    return l >= 0 && l < KSB_MAX_LAMPS ? l : 0;
}

static rgb_t body_color(const ksui_view_t *v, uint32_t now, bool *asleep)
{
    const ksb_scene_t *s = &v->shown[focus_lamp(v)];
    uint8_t r, g, b;
    ksb_scene_rgb(s, &r, &g, &b);
    rgb_t c = {r, g, b};
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    *asleep = s->kind == KSB_SCENE_OFF || mx < 8;
    if (*asleep) return (rgb_t){62, 68, 92};
    // Keep the hue but make it readable on a tiny screen.
    float k = mx < 150 ? 150.0f / mx : 1.0f;
    c.r = (uint8_t)fminf(255, c.r * k);
    c.g = (uint8_t)fminf(255, c.g * k);
    c.b = (uint8_t)fminf(255, c.b * k);
    if (s->kind == KSB_SCENE_EFFECT) {
        uint32_t period = 600 + (uint32_t)(100 - (s->speed > 100 ? 100 : s->speed)) * 30;
        c = mix((rgb_t){c.r / 3, c.g / 3, c.b / 3}, c, 0.35f + 0.65f * wave(now, period));
    }
    return c;
}

// Lumi centred at (cx0, cy0); her extras are placed relative to her.
static void draw_lumi(const ksui_view_t *v, uint32_t now, float cx0, float cy0)
{
    const ksb_model_t *m = &v->model;
    int top = ksb_top_source(m);
    bool needs_you = top >= 0 && m->src[top].state == KSA_SIG_NEEDS_YOU;
    bool error = now < v->error_until_ms || (top >= 0 && m->src[top].state == KSA_SIG_ERROR);
    bool scanning = v->link != KSUI_LINK_CONNECTED;
    uint32_t since_ev = now - m->last_event_ms;
    bool excited = m->last_event != KSB_EV_NONE && since_ev < 900 &&
                   (m->last_event == KSB_EV_MANUAL || m->last_event == KSB_EV_SIGNAL);
    bool loved = m->last_event == KSB_EV_ACK && since_ev < 1500;
    uint32_t since_claim = now - v->claim_ms;
    bool claiming = v->claim == KSUI_CLAIM_SEARCHING;
    if (v->claim == KSUI_CLAIM_OK && since_claim < 2500) loved = excited = true, since_ev = since_claim % 900;
    if ((v->claim == KSUI_CLAIM_NONE_FOUND || v->claim == KSUI_CLAIM_FULL) && since_claim < 2500) error = true;
    scanning = scanning || claiming;
    bool asleep;
    rgb_t body = body_color(v, now, &asleep);
    asleep = asleep && !scanning && !needs_you;

    float bob = needs_you ? 2.0f * sinf((float)now / 90.0f) * (wave(now, 1400) > 0.5f ? 1.0f : 0.0f)
                          : 1.5f * sinf(2.0f * (float)M_PI * (float)(now % 2400) / 2400.0f);
    if (excited) bob -= 3.0f * (1.0f - (float)since_ev / 900.0f);
    float cx = cx0, cy = cy0 + bob, r = 22;
    int ax = (int)cx0, ay = (int)cy0; // anchor for extras (doesn't bob)

    // halo
    float halo = asleep ? 0.10f : needs_you ? 0.25f + 0.45f * wave(now, 900) : 0.22f;
    disc(cx, cy, r + 7, body, halo * 0.5f);
    disc(cx, cy, r + 4, body, halo);

    // antenna cord + bulb
    rgb_t bulb = scanning ? mix(BG, (rgb_t){80, 140, 255}, wave(now, 1000) > 0.5f ? 1.0f : 0.25f)
               : error ? (rgb_t){255, 70, 70}
               : needs_you ? AMBER
               : (rgb_t){90, 230, 130};
    for (int y = (int)(cy - r - 6); y < (int)(cy - r + 2); y++) px(ax, y, DIM);
    disc(cx, cy - r - 8, 2.6f, bulb, 1.0f);

    // body + little feet + soft top highlight
    disc(cx - 9, cy + r - 1, 4, mix(body, BG, 0.35f), 1.0f);
    disc(cx + 9, cy + r - 1, 4, mix(body, BG, 0.35f), 1.0f);
    disc(cx, cy, r, body, 1.0f);
    disc(cx - 7, cy - 9, 6, (rgb_t){255, 255, 255}, 0.18f);

    // face
    uint8_t lum = (uint8_t)((body.r * 3 + body.g * 6 + body.b) / 10);
    rgb_t ink = lum > 40 ? (rgb_t){22, 20, 34} : (rgb_t){235, 235, 250};
    int ey = (int)(cy - 3);
    int look = scanning ? ((now / 900) % 2 ? 2 : -2) : 0;
    bool blink = (now % 3700) < 130;
    if (asleep || blink) {
        rect((int)cx - 11, ey + 1, 6, 1, ink);
        rect((int)cx + 5, ey + 1, 6, 1, ink);
    } else {
        float ry = needs_you || excited ? 4.5f : 3.5f;
        ellipse(cx - 8 + look, (float)ey, 2.6f, ry, ink);
        ellipse(cx + 8 + look, (float)ey, 2.6f, ry, ink);
        px((int)cx - 8 + look, ey - 2, (rgb_t){255, 255, 255});
        px((int)cx + 8 + look, ey - 2, (rgb_t){255, 255, 255});
    }
    // cheeks
    disc(cx - 13, cy + 4, 2.4f, (rgb_t){255, 120, 160}, 0.55f);
    disc(cx + 13, cy + 4, 2.4f, (rgb_t){255, 120, 160}, 0.55f);

    // mouth
    int my = (int)(cy + 6);
    if (needs_you) {
        ellipse(cx, (float)my + 1, 3.0f, 3.0f, ink);
    } else if (error) {
        for (int i = -4; i <= 4; i++) px((int)cx + i, my + 2 - (4 - abs(i)) / 2, ink);
    } else if (asleep || scanning) {
        ellipse(cx, (float)my + 1, 1.6f, 1.6f, ink);
    } else {
        for (int i = -4; i <= 4; i++) px((int)cx + i, my + (16 - i * i) / 8, ink);
        if (excited) rect((int)cx - 2, my + 1, 5, 1, ink);
    }

    // extras
    if (asleep) {
        for (int k = 0; k < 2; k++) {
            uint32_t t = (now + k * 1100) % 2200;
            float a = 1.0f - (float)t / 2200.0f;
            char z[2] = {'z', 0};
            int zx = ax + 20 + k * 6 + (int)(t / 300), zy = ay - 18 - k * 8 - (int)(t / 180);
            if (a > 0.2f) text(zx, zy, z, mix(BG, DIM, a));
        }
    }
    if (needs_you && wave(now, 700) > 0.4f) {
        rect(ax + 26, ay - 34, 3, 12, AMBER);
        rect(ax + 26, ay - 20, 3, 3, AMBER);
    }
    if (excited) {
        float a = 1.0f - (float)since_ev / 900.0f;
        float spin = (float)since_ev / 250.0f;
        for (int k = 0; k < 4; k++) {
            float ang = spin + (float)k * (float)M_PI / 2.0f;
            sparkle((int)(cx + cosf(ang) * 31), (int)(cy + sinf(ang) * 27), (rgb_t){255, 255, 220}, a);
        }
    }
    if (loved) heart(ax + 22, (int)((float)ay - 30 - (float)since_ev / 150.0f), (rgb_t){255, 110, 150});
    if (claiming && wave(now, 800) > 0.3f) text(ax + 24, ay - 34, "?", (rgb_t){80, 140, 255});
}

static void scene_label(const ksb_scene_t *s, char *out, size_t n)
{
    static const char *fx[9] = {"rainbow", "rgb fade", "red", "green", "blue", "yellow", "cyan", "purple", "white"};
    switch (s->kind) {
    case KSB_SCENE_OFF: snprintf(out, n, "off"); break;
    case KSB_SCENE_RGB: snprintf(out, n, "#%02X%02X%02X %u%%", s->r, s->g, s->b, s->bright); break;
    case KSB_SCENE_EFFECT:
        snprintf(out, n, "fx %s", (s->effect >= 0x82 && s->effect <= 0x8A) ? fx[s->effect - 0x82] : "?");
        break;
    case KSB_SCENE_WHITE: snprintf(out, n, "white %u", s->white); break;
    default: out[0] = 0;
    }
}

static rgb_t scene_swatch(const ksb_scene_t *s)
{
    uint8_t r, g, b;
    ksb_scene_rgb(s, &r, &g, &b);
    return (rgb_t){r, g, b};
}

static void draw_lamp_row(const ksui_view_t *v, int x0, int y, uint32_t now)
{
    if (v->n_lamps == 0) {
        text(x0, y, "no lamp yet", DIM);
        return;
    }
    for (int l = 0; l < v->n_lamps && l < KSB_MAX_LAMPS; l++) {
        int x = x0 + l * 19;
        char digit[2] = {(char)('1' + l), 0};
        text(x, y, digit, DIM);
        rgb_t ring = v->lamps[l].link == KSUI_LINK_CONNECTED ? (rgb_t){90, 230, 130}
                   : mix(BG, (rgb_t){80, 140, 255}, wave(now + (uint32_t)l * 250, 1000) > 0.5f ? 1.0f : 0.3f);
        rect(x + 7, y - 1, 9, 9, ring);
        rect(x + 8, y, 7, 7, BG);
        rect(x + 9, y + 1, 5, 5, scene_swatch(&v->shown[l]));
    }
}

static bool draw_claim(const ksui_view_t *v, int x0, int y0, uint32_t now)
{
    const int y1 = y0 + 10;
    uint32_t since = now - v->claim_ms;
    char buf[24];
    if (v->button_held_ms >= KSUI_HOLD_FORGET_MS) {
        text(x0, y0, "let go to", (rgb_t){255, 70, 70});
        text(x0, y1, "FORGET ALL", (rgb_t){255, 70, 70});
        return true;
    }
    if (v->button_held_ms >= KSUI_HOLD_CLAIM_MS) {
        text(x0, y0, "let go to", AMBER);
        text(x0, y1, "add a lamp", TEXT);
        return true;
    }
    switch (v->claim) {
    case KSUI_CLAIM_SEARCHING:
        text(x0, y0, "hold a lamp", AMBER);
        text(x0, y1, "close by...", TEXT);
        return true;
    case KSUI_CLAIM_OK:
        if (since > 4000) return false;
        snprintf(buf, sizeof buf, "lamp %d!", v->claim_slot + 1);
        text(x0, y0, buf, (rgb_t){90, 230, 130});
        text_clip(x0, y1, v->lamps[v->claim_slot < KSB_MAX_LAMPS ? v->claim_slot : 0].name, 12, DIM);
        return true;
    case KSUI_CLAIM_NONE_FOUND:
        if (since > 4000) return false;
        text(x0, y0, "no new lamp", (rgb_t){255, 70, 70});
        text(x0, y1, "found nearby", DIM);
        return true;
    case KSUI_CLAIM_FULL:
        if (since > 4000) return false;
        text(x0, y0, "all 4 lamps", (rgb_t){255, 70, 70});
        text(x0, y1, "claimed", DIM);
        return true;
    case KSUI_CLAIM_FORGOT:
        if (since > 4000) return false;
        text(x0, y0, "forgot lamps", AMBER);
        text(x0, y1, "& devboxes", DIM);
        return true;
    default:
        return false;
    }
}

// Used sources, most urgent first. Returns the count.
static int sources_by_urgency(const ksb_model_t *m, int order[KSB_MAX_SOURCES])
{
    int n = 0;
    for (int i = 0; i < KSB_MAX_SOURCES; i++)
        if (m->src[i].used) order[n++] = i;
    for (int a = 1; a < n; a++) {
        for (int b = a; b > 0; b--) {
            const ksb_source_t *p = &m->src[order[b - 1]], *q = &m->src[order[b]];
            int pu = p->acked ? 0 : p->state, qu = q->acked ? 0 : q->state;
            if (qu <= pu) break;
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    }
    return n;
}

static void draw_source_row(const ksui_view_t *v, const ksb_source_t *s, int src_index, int x0, int y,
                            int max_chars, uint32_t now)
{
    rgb_t c = s->acked ? STATE_COLORS[0] : STATE_COLORS[s->state < KSA_SIG_COUNT ? s->state : 0];
    bool blink = !s->acked && s->state == KSA_SIG_NEEDS_YOU && wave(now, 600) < 0.35f;
    if (!blink) rect(x0, y + 1, 5, 5, c);
    int lamp = ksb_source_lamp(&v->model, src_index);
    char tag[2] = {lamp >= 0 ? (char)('1' + lamp) : '*', 0};
    if (v->n_lamps > 1) {
        text(x0 + 8, y, tag, DIM);
        text_clip(x0 + 16, y, s->name, max_chars - 1, s->acked ? DIM : TEXT);
    } else {
        text_clip(x0 + 8, y, s->name, max_chars, s->acked ? DIM : TEXT);
    }
}

static rgb_t link_dot(const ksui_view_t *v, uint32_t now)
{
    return v->link == KSUI_LINK_CONNECTED ? (rgb_t){90, 230, 130}
         : mix(BG, (rgb_t){80, 140, 255}, wave(now, 1000) > 0.5f ? 1.0f : 0.3f);
}

static const char *status_word(const ksui_view_t *v)
{
    return v->link == KSUI_LINK_CONNECTED ? "linked"
         : v->n_lamps == 0 ? "hold BOOT"
         : v->link == KSUI_LINK_SCANNING ? "searching" : "waking up";
}

// Portrait, 80 x 160, top = USB plug end (away from you): lamps and link dot
// on top, Lumi big in the upper half, then the headline and the agent list.
static void draw_portrait(const ksui_view_t *v, uint32_t now)
{
    const ksb_model_t *m = &v->model;
    draw_lamp_row(v, 2, 3, now); // each lamp's ring already shows its link
    draw_lumi(v, now, 40, 52);

    const int x0 = 3;
    if (!draw_claim(v, x0, 86, now)) {
        int top = ksb_top_source(m);
        if (top >= 0 && m->src[top].state >= KSA_SIG_DONE) {
            text_clip(x0, 86, m->src[top].name, 12, STATE_COLORS[m->src[top].state]);
            text(x0, 96, ksb_state_name(m->src[top].state), DIM);
        } else {
            text(x0, 86, status_word(v), TEXT);
            text(x0, 96, v->transport ? v->transport : "", DIM);
        }
    }
    rect(x0, 107, s_w - 2 * x0, 1, RULE);

    int order[KSB_MAX_SOURCES];
    int n = sources_by_urgency(m, order);
    if (n == 0) {
        char label[24];
        scene_label(&v->shown[0], label, sizeof label);
        text_clip(x0, 112, label, 12, TEXT);
        int lamps = v->n_lamps ? v->n_lamps : 1, w = (s_w - 2 * x0 - 2) / lamps;
        rect(x0, 124, s_w - 2 * x0, 6, RULE);
        for (int l = 0; l < lamps; l++) rect(x0 + 1 + l * w, 125, w - (l < lamps - 1 ? 1 : 0), 4, scene_swatch(&v->shown[l]));
        text(x0, 138, "no agents", DIM);
        return;
    }
    int rows = n > 5 ? 4 : n;
    for (int k = 0; k < rows; k++) draw_source_row(v, &m->src[order[k]], order[k], x0, 111 + k * 10, 10, now);
    if (n > 5) {
        char more[24];
        snprintf(more, sizeof more, "+%d more", n - 4);
        text(x0 + 8, 151, more, DIM);
    }
}

static void draw_panel(const ksui_view_t *v, uint32_t now)
{
    const ksb_model_t *m = &v->model;
    const int x0 = 84;
    text(x0, 3, v->transport ? v->transport : "", DIM);
    disc(153, 6, 2.5f, link_dot(v, now), 1.0f);

    if (!draw_claim(v, x0, 13, now)) {
        int top = ksb_top_source(m);
        if (top >= 0 && m->src[top].state == KSA_SIG_NEEDS_YOU) {
            text_clip(x0, 13, m->src[top].name, 12, AMBER);
        } else {
            text(x0, 13, status_word(v), TEXT);
        }
        draw_lamp_row(v, x0, 24, now);
    }
    rect(x0, 34, KSUI_W - x0 - 3, 1, RULE);

    int order[KSB_MAX_SOURCES];
    int n = sources_by_urgency(m, order);
    if (n == 0) {
        char label[24];
        scene_label(&v->shown[0], label, sizeof label);
        text_clip(x0, 40, label, 12, TEXT);
        int lamps = v->n_lamps ? v->n_lamps : 1, w = (KSUI_W - x0 - 5) / lamps;
        rect(x0, 51, KSUI_W - x0 - 3, 6, RULE);
        for (int l = 0; l < lamps; l++) rect(x0 + 1 + l * w, 52, w - (l < lamps - 1 ? 1 : 0), 4, scene_swatch(&v->shown[l]));
        text(x0, 65, "no agents", DIM);
        return;
    }
    int rows = n > 4 ? 3 : n;
    for (int k = 0; k < rows; k++) draw_source_row(v, &m->src[order[k]], order[k], x0, 38 + k * 10, 11, now);
    if (n > 4) {
        char more[24];
        snprintf(more, sizeof more, "+%d more", n - 3);
        text(x0 + 8, 68, more, DIM);
    }
}

void ksui_render(uint16_t *fb, const ksui_view_t *v, uint32_t now)
{
    s_fb = fb;
    s_w = v->portrait ? KSUI_H : KSUI_W;
    s_h = v->portrait ? KSUI_W : KSUI_H;
    uint16_t bg = pack(BG);
    for (int i = 0; i < KSUI_PIXELS; i++) fb[i] = bg;
    if (v->portrait) {
        draw_portrait(v, now);
    } else {
        draw_lumi(v, now, 40, 48);
        draw_panel(v, now);
    }
    int top = ksb_top_source(&v->model);
    if (top >= 0 && v->model.src[top].state == KSA_SIG_NEEDS_YOU && wave(now, 900) > 0.5f) {
        for (int x = 0; x < s_w; x++) { px(x, 0, AMBER); px(x, s_h - 1, AMBER); }
        for (int y = 0; y < s_h; y++) { px(0, y, AMBER); px(s_w - 1, y, AMBER); }
    }
}
