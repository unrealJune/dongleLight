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

static const rgb_t BG    = {14, 17, 30};
static const rgb_t AMBER = {255, 176, 0};
static const rgb_t RED   = {255, 56, 64};
static const rgb_t GREEN = {40, 230, 105};
static const rgb_t BLUE  = {56, 140, 255};
static const rgb_t PINK  = {255, 95, 150};
static const rgb_t CYAN  = {65, 195, 220};

static const rgb_t STATE_COLORS[KSA_SIG_COUNT] = {
    {110, 110, 130}, {56, 140, 255}, {40, 230, 105}, {255, 56, 64}, {255, 176, 0},
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

static rgb_t boost_vivid(rgb_t c)
{
    uint8_t mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    if (mx < 8) return (rgb_t){70, 80, 115};
    float k = mx < 220 ? 220.0f / (float)mx : 1.0f;
    return (rgb_t){
        (uint8_t)fminf(255.0f, c.r * k),
        (uint8_t)fminf(255.0f, c.g * k),
        (uint8_t)fminf(255.0f, c.b * k),
    };
}

static rgb_t contrast_ink(rgb_t bg)
{
    int lum = (bg.r * 3 + bg.g * 6 + bg.b) / 10;
    return lum > 115 ? (rgb_t){16, 18, 28} : (rgb_t){250, 252, 255};
}

static void px(int x, int y, rgb_t c)
{
    if (x >= 0 && x < s_w && y >= 0 && y < s_h) s_fb[y * s_w + x] = pack(c);
}

static void px_blend(int x, int y, rgb_t c, float a)
{
    if (x < 0 || x >= s_w || y < 0 || y >= s_h || a <= 0.0f) return;
    if (a >= 1.0f) { s_fb[y * s_w + x] = pack(c); return; }
    s_fb[y * s_w + x] = pack(mix(unpack(s_fb[y * s_w + x]), c, a));
}

static void rect(int x, int y, int w, int h, rgb_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) px(i, j, c);
}

static void border(int thick, rgb_t c, float alpha)
{
    for (int y = 0; y < s_h; y++) {
        for (int x = 0; x < s_w; x++) {
            if (x < thick || x >= s_w - thick || y < thick || y >= s_h - thick)
                px_blend(x, y, c, alpha);
        }
    }
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

// Anti-aliased ring of radius r and stroke thickness `thick`.
static void ring(float cx, float cy, float r, float thick, rgb_t c, float alpha)
{
    float hw = thick * 0.5f;
    float outer = r + hw + 1.0f;
    for (int y = (int)(cy - outer); y <= (int)(cy + outer); y++) {
        for (int x = (int)(cx - outer); x <= (int)(cx + outer); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float cov = hw + 0.5f - fabsf(d - r);
            if (cov > 0) px_blend(x, y, c, alpha * (cov > 1 ? 1 : cov));
        }
    }
}

// Anti-aliased thick line segment with round caps of radius r.
static void capsule(float x0, float y0, float x1, float y1, float r, rgb_t c, float alpha)
{
    float minx = (x0 < x1 ? x0 : x1) - r - 1.0f;
    float maxx = (x0 > x1 ? x0 : x1) + r + 1.0f;
    float miny = (y0 < y1 ? y0 : y1) - r - 1.0f;
    float maxy = (y0 > y1 ? y0 : y1) + r + 1.0f;
    float dx = x1 - x0, dy = y1 - y0;
    float l2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++) {
        for (int x = (int)minx; x <= (int)maxx; x++) {
            float px_x = x + 0.5f, px_y = y + 0.5f;
            float t = l2 > 0.001f ? ((px_x - x0) * dx + (px_y - y0) * dy) / l2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float qx = x0 + t * dx, qy = y0 + t * dy;
            float d = sqrtf((px_x - qx) * (px_x - qx) + (px_y - qy) * (px_y - qy));
            float cov = r + 0.5f - d;
            if (cov > 0) px_blend(x, y, c, alpha * (cov > 1 ? 1 : cov));
        }
    }
}

static void ellipse(float cx, float cy, float rx, float ry, rgb_t c)
{
    for (int y = (int)(cy - ry - 2); y <= (int)(cy + ry + 2); y++) {
        for (int x = (int)(cx - rx - 2); x <= (int)(cx + rx + 2); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            float d = sqrtf(dx * dx + dy * dy);
            float r_min = rx < ry ? rx : ry;
            float cov = (1.0f - d) * r_min + 0.5f;
            if (cov > 0.0f) px_blend(x, y, c, cov > 1.0f ? 1.0f : cov);
        }
    }
}

static int text_scale(int x, int y, const char *s, int scale, rgb_t c)
{
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        for (int col = 0; col < 5; col++) {
            uint8_t bits = ksui_font5x7[ch - 0x20][col];
            for (int row = 0; row < 8; row++)
                if (bits & (1u << row)) rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
    return x;
}

static float wave(uint32_t now, uint32_t period_ms)
{
    return 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (float)(now % period_ms) / (float)period_ms);
}

// Lamp Lumi mirrors: the one showing the most urgent signal, else lamp 1.
static int focus_lamp(const ksui_view_t *v)
{
    int top = ksb_top_source(&v->model);
    int l = top >= 0 ? ksb_source_lamp(&v->model, top) : -1;
    return l >= 0 && l < KSB_MAX_LAMPS ? l : 0;
}

static rgb_t lamp_body_color(const ksui_view_t *v, int lamp, uint32_t now, bool *asleep)
{
    if (lamp < 0 || lamp >= KSB_MAX_LAMPS) lamp = focus_lamp(v);
    const ksb_scene_t *s = &v->shown[lamp];
    uint8_t r, g, b;
    ksb_scene_rgb(s, &r, &g, &b);
    rgb_t c = {r, g, b};
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    *asleep = s->kind == KSB_SCENE_OFF || mx < 8;
    if (*asleep) return (rgb_t){62, 68, 92};
    // Keep the hue but make it readable on a tiny screen.
    float k = mx < 165 ? 165.0f / mx : 1.0f;
    c.r = (uint8_t)fminf(255, c.r * k);
    c.g = (uint8_t)fminf(255, c.g * k);
    c.b = (uint8_t)fminf(255, c.b * k);
    if (s->kind == KSB_SCENE_EFFECT) {
        uint32_t period = 600 + (uint32_t)(100 - (s->speed > 100 ? 100 : s->speed)) * 30;
        c = mix((rgb_t){c.r / 3, c.g / 3, c.b / 3}, c, 0.35f + 0.65f * wave(now, period));
    }
    return c;
}

static rgb_t body_color(const ksui_view_t *v, uint32_t now, bool *asleep)
{
    return lamp_body_color(v, focus_lamp(v), now, asleep);
}

static rgb_t scene_swatch(const ksb_scene_t *s)
{
    uint8_t r, g, b;
    ksb_scene_rgb(s, &r, &g, &b);
    return (rgb_t){r, g, b};
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

// =============================================================================
// REWORKED DISTANCE-READABLE UI:
//   1. Full-screen color flash + giant high-contrast symbol on any update
//   2. Big text-free Lumi filling the screen otherwise
// =============================================================================

typedef enum {
    ICON_NONE,
    ICON_BANG,      // !  needs-you
    ICON_CHECK,     // v  done / claimed
    ICON_CROSS,     // X  error / bad frame / claim failed
    ICON_HEART,     // <3 ack
    ICON_WORK,      // .. working
    ICON_SPARK,     // *  manual color / remote change
    ICON_SLEEP,     // Z  power off / idle
    ICON_SEARCH,    // ?  claiming / searching
} flash_icon_t;

static void draw_giant_icon(flash_icon_t ic, float cx, float cy, float s, rgb_t ink, uint32_t now)
{
    switch (ic) {
    case ICON_BANG:
        capsule(cx, cy - 23.0f * s, cx, cy + 5.0f * s, 6.0f * s, ink, 1.0f);
        disc(cx, cy + 21.0f * s, 6.5f * s, ink, 1.0f);
        break;
    case ICON_CHECK:
        capsule(cx - 18.0f * s, cy + 2.0f * s, cx - 4.0f * s, cy + 16.0f * s, 5.5f * s, ink, 1.0f);
        capsule(cx - 4.0f * s, cy + 16.0f * s, cx + 20.0f * s, cy - 16.0f * s, 5.5f * s, ink, 1.0f);
        break;
    case ICON_CROSS:
        capsule(cx - 16.0f * s, cy - 16.0f * s, cx + 16.0f * s, cy + 16.0f * s, 5.5f * s, ink, 1.0f);
        capsule(cx - 16.0f * s, cy + 16.0f * s, cx + 16.0f * s, cy - 16.0f * s, 5.5f * s, ink, 1.0f);
        break;
    case ICON_HEART: {
        float d = 9.0f * s, r = 10.8f * s, y0 = cy - 7.5f * s;
        float r_s2 = r * 0.70710678f, r_2 = r * 1.41421356f;
        int reach = (int)(d + r_2 + 3.0f);
        for (int y = (int)(y0 - r - 2); y <= (int)(y0 + reach); y++) {
            for (int x = (int)(cx - reach); x <= (int)(cx + reach); x++) {
                float ax = fabsf((x + 0.5f) - cx);
                float dy = (y + 0.5f) - y0;
                float dist;
                if (dy >= r_s2) {
                    dist = ((ax - d) + dy - r_2) * 0.70710678f;
                } else if (dy >= 0.0f && ax <= d) {
                    dist = -r;
                } else {
                    dist = sqrtf((ax - d) * (ax - d) + dy * dy) - r;
                }
                float cov = 0.5f - dist;
                if (cov > 0.0f) px_blend(x, y, ink, cov > 1.0f ? 1.0f : cov);
            }
        }
        break;
    }
    case ICON_WORK: {
        float phase = (float)(now % 900) / 900.0f;
        ring(cx, cy, (14.0f + 12.0f * phase) * s, 4.0f * s, ink, 1.0f - phase * 0.7f);
        for (int k = -1; k <= 1; k++) {
            float bounce = 4.0f * s * sinf(2.0f * (float)M_PI * phase - (float)k * 1.1f);
            disc(cx + (float)k * 14.0f * s, cy + bounce, 4.8f * s, ink, 1.0f);
        }
        break;
    }
    case ICON_SPARK: {
        float rad = (22.0f + 2.5f * wave(now, 600)) * s;
        int ir = (int)(rad + 2.0f);
        for (int y = (int)cy - ir; y <= (int)cy + ir; y++) {
            for (int x = (int)cx - ir; x <= (int)cx + ir; x++) {
                float ax = fabsf((x + 0.5f) - cx) / rad;
                float ay = fabsf((y + 0.5f) - cy) / rad;
                float v = powf(ax, 0.58f) + powf(ay, 0.58f);
                float cov = (1.0f - v) * rad * 0.55f + 0.5f;
                if (cov > 0.0f) px_blend(x, y, ink, cov > 1.0f ? 1.0f : cov);
            }
        }
        break;
    }
    case ICON_SLEEP:
        capsule(cx - 13.0f * s, cy - 15.0f * s, cx + 13.0f * s, cy - 15.0f * s, 4.5f * s, ink, 1.0f);
        capsule(cx + 13.0f * s, cy - 15.0f * s, cx - 13.0f * s, cy + 15.0f * s, 4.5f * s, ink, 1.0f);
        capsule(cx - 13.0f * s, cy + 15.0f * s, cx + 13.0f * s, cy + 15.0f * s, 4.5f * s, ink, 1.0f);
        break;
    case ICON_SEARCH: {
        float p1 = (float)(now % 1000) / 1000.0f;
        float p2 = (float)((now + 500) % 1000) / 1000.0f;
        ring(cx, cy, (8.0f + 22.0f * p1) * s, 3.8f * s, ink, 1.0f - p1);
        ring(cx, cy, (8.0f + 22.0f * p2) * s, 3.8f * s, ink, 1.0f - p2);
        disc(cx, cy, 6.5f * s, ink, 1.0f);
        break;
    }
    default:
        break;
    }
}

// Full-screen BOOT hold gauge: fills the entire screen from bottom to top so
// you can tell when 3 s (add lamp = amber) and 10 s (forget all = red) are hit
// even with your thumb on the button.
static void draw_button_hold_screen(const ksui_view_t *v, uint32_t now)
{
    uint32_t held = v->button_held_ms;
    bool forget = held >= KSUI_HOLD_FORGET_MS;
    bool claim  = held >= KSUI_HOLD_CLAIM_MS;
    rgb_t col = forget ? RED : AMBER;

    if (claim) {
        // Threshold reached: strobe the entire screen in Amber (3s) or Red (10s)!
        bool hi = wave(now, forget ? 280 : 420) > 0.35f;
        rgb_t bg = hi ? col : mix(BG, col, 0.25f);
        rect(0, 0, s_w, s_h, bg);
        if (!hi) border(7, col, 1.0f);
        rgb_t ink = hi ? contrast_ink(col) : col;
        float cx = s_w * 0.5f, cy = v->portrait ? s_h * 0.38f : s_h * 0.5f;
        if (forget) {
            draw_giant_icon(ICON_CROSS, cx, cy, v->portrait ? 0.9f : 0.75f, ink, now);
        } else {
            capsule(cx - 16, cy, cx + 16, cy, 5.0f, ink, 1.0f);
            capsule(cx, cy - 16, cx, cy + 16, 5.0f, ink, 1.0f);
        }
        if (v->portrait) {
            const char *lbl = forget ? "RST" : "ADD";
            text_scale((s_w - 3 * 18) / 2, s_h - 42, lbl, 3, ink);
        }
        return;
    }

    // Rising fill toward 3 s
    float frac = (float)held / (float)KSUI_HOLD_CLAIM_MS;
    if (frac > 1.0f) frac = 1.0f;
    int fill_h = (int)(frac * (float)s_h);
    rect(0, 0, s_w, s_h, BG);
    rect(0, s_h - fill_h, s_w, fill_h, mix(BG, AMBER, 0.85f));
    border(4, AMBER, 0.8f);
}

// Render a full-screen color flash with strobe inversion and a giant icon.
static void draw_full_flash(const ksui_view_t *v, rgb_t c, flash_icon_t ic, int lamp_idx,
                            uint32_t elapsed_ms, uint32_t dur_ms, uint32_t now)
{
    c = boost_vivid(c);
    // High-visibility strobe: alternate full-screen flood and framed contrast beat
    float p = wave(elapsed_ms, 460);
    bool full_flood = p > 0.34f;
    float fade = dur_ms > 0 && elapsed_ms + 320 > dur_ms
               ? (float)(dur_ms - elapsed_ms) / 320.0f : 1.0f;
    if (fade < 0.15f) fade = 0.15f;

    rgb_t bg = full_flood ? mix(BG, c, fade) : mix(BG, c, 0.25f * fade);
    rect(0, 0, s_w, s_h, bg);
    if (full_flood) {
        border(4, (rgb_t){255, 255, 255}, 0.25f * fade);
    } else {
        border(7, c, fade);
    }

    rgb_t ink = full_flood ? contrast_ink(c) : mix(c, (rgb_t){255, 255, 255}, 0.45f);
    bool has_lamp_tag = v->n_lamps > 1 && lamp_idx >= 0 && lamp_idx < KSB_MAX_LAMPS;

    if (v->portrait) {
        float cy = has_lamp_tag ? s_h * 0.39f : s_h * 0.50f;
        draw_giant_icon(ic, s_w * 0.5f, cy, 1.05f, ink, now);
        if (has_lamp_tag) {
            char d[2] = {(char)('1' + lamp_idx), 0};
            float by = s_h - 32.0f;
            capsule(s_w * 0.5f - 6.0f, by, s_w * 0.5f + 6.0f, by, 15.5f, ink, 1.0f);
            text_scale((s_w - 15) / 2, (int)by - 10, d, 3, bg);
        }
    } else {
        float cx = has_lamp_tag ? s_w * 0.38f : s_w * 0.50f;
        draw_giant_icon(ic, cx, s_h * 0.50f, 0.85f, ink, now);
        if (has_lamp_tag) {
            char d[2] = {(char)('1' + lamp_idx), 0};
            float bx = s_w - 32.0f, by = s_h * 0.50f;
            capsule(bx - 4.0f, by, bx + 4.0f, by, 15.5f, ink, 1.0f);
            text_scale((int)bx - 7, (int)by - 10, d, 3, bg);
        }
    }
}

// Check if an update flash (button hold, claim, or model event) should take
// over the screen right now. Returns true if drawn.
static bool maybe_draw_update_flash(const ksui_view_t *v, uint32_t now)
{
    if (v->button_held_ms > 0) {
        draw_button_hold_screen(v, now);
        return true;
    }

    uint32_t since_claim = now - v->claim_ms;
    switch (v->claim) {
    case KSUI_CLAIM_SEARCHING:
        draw_full_flash(v, BLUE, ICON_SEARCH, v->claim_slot, now % 2000, 0, now);
        return true;
    case KSUI_CLAIM_OK:
        if (since_claim < 3200) {
            draw_full_flash(v, GREEN, ICON_CHECK, v->claim_slot, since_claim, 3200, now);
            return true;
        }
        break;
    case KSUI_CLAIM_NONE_FOUND:
    case KSUI_CLAIM_FULL:
        if (since_claim < 3200) {
            draw_full_flash(v, RED, ICON_CROSS, -1, since_claim, 3200, now);
            return true;
        }
        break;
    case KSUI_CLAIM_FORGOT:
        if (since_claim < 3200) {
            draw_full_flash(v, AMBER, ICON_SPARK, -1, since_claim, 3200, now);
            return true;
        }
        break;
    default:
        break;
    }

    const ksb_model_t *m = &v->model;
    if (now < v->error_until_ms) {
        uint32_t rem = v->error_until_ms - now;
        uint32_t el = rem < 1500 ? 1500 - rem : 0;
        draw_full_flash(v, RED, ICON_CROSS, -1, el, 1500, now);
        return true;
    }

    if (m->last_event != KSB_EV_NONE) {
        uint32_t since_ev = now - m->last_event_ms;
        uint32_t dur = m->last_event == KSB_EV_SIGNAL ? KSUI_FLASH_MS : 1900;
        if (since_ev < dur) {
            rgb_t col = AMBER;
            flash_icon_t ic = ICON_SPARK;
            int lamp_idx = -1;

            switch (m->last_event) {
            case KSB_EV_SIGNAL: {
                int si = m->last_event_src;
                if (si >= 0 && si < KSB_MAX_SOURCES && m->src[si].used) {
                    const ksb_source_t *s = &m->src[si];
                    uint8_t st = s->state < KSA_SIG_COUNT ? s->state : 0;
                    col = s->styled ? (rgb_t){s->r, s->g, s->b} : STATE_COLORS[st];
                    ic = st == KSA_SIG_NEEDS_YOU ? ICON_BANG
                       : st == KSA_SIG_ERROR     ? ICON_CROSS
                       : st == KSA_SIG_DONE      ? ICON_CHECK
                       : st == KSA_SIG_WORKING   ? ICON_WORK
                                                 : ICON_SLEEP;
                    lamp_idx = ksb_source_lamp(m, si);
                } else {
                    col = CYAN;
                    ic = ICON_SLEEP;
                }
                break;
            }
            case KSB_EV_ACK:
                col = PINK;
                ic = ICON_HEART;
                break;
            case KSB_EV_MANUAL: {
                bool asleep = false;
                int ml = (m->last_event_src >= 0 && m->last_event_src < KSB_MAX_LAMPS) ? m->last_event_src : -1;
                col = lamp_body_color(v, ml, now, &asleep);
                ic = asleep ? ICON_SLEEP : ICON_SPARK;
                lamp_idx = ml;
                break;
            }
            case KSB_EV_BAD:
                col = RED;
                ic = ICON_CROSS;
                break;
            default:
                break;
            }
            draw_full_flash(v, col, ic, lamp_idx, since_ev, dur, now);
            return true;
        }
    }

    return false;
}

// Chunky lamp strip: wide color bars at the edge of the screen framed by a
// dark border so they pop against Lumi's full-bleed body color.
static void draw_chunky_lamp_strip(const ksui_view_t *v, uint32_t now)
{
    int n = v->n_lamps > 0 ? v->n_lamps : 1;
    if (n > KSB_MAX_LAMPS) n = KSB_MAX_LAMPS;
    int pad = 2, gap = 3, thick = 8;
    if (v->portrait) {
        rect(0, 0, s_w, pad * 2 + thick, BG);
        int total_w = s_w - 2 * pad - (n - 1) * gap;
        int bw = total_w / n;
        for (int l = 0; l < n; l++) {
            int x = pad + l * (bw + gap);
            bool connected = v->n_lamps > 0 && v->lamps[l].link == KSUI_LINK_CONNECTED;
            rgb_t c = connected ? boost_vivid(scene_swatch(&v->shown[l]))
                                : mix(BG, BLUE, wave(now + (uint32_t)l * 250, 900) > 0.45f ? 1.0f : 0.25f);
            if (v->n_lamps > 0 && v->shown[l].kind == KSB_SCENE_OFF && connected) c = (rgb_t){52, 58, 82};
            rect(x, pad, bw, thick, c);
        }
    } else {
        rect(0, 0, pad * 2 + thick, s_h, BG);
        int total_h = s_h - 2 * pad - (n - 1) * gap;
        int bh = total_h / n;
        for (int l = 0; l < n; l++) {
            int y = pad + l * (bh + gap);
            bool connected = v->n_lamps > 0 && v->lamps[l].link == KSUI_LINK_CONNECTED;
            rgb_t c = connected ? boost_vivid(scene_swatch(&v->shown[l]))
                                : mix(BG, BLUE, wave(now + (uint32_t)l * 250, 900) > 0.45f ? 1.0f : 0.25f);
            if (v->n_lamps > 0 && v->shown[l].kind == KSB_SCENE_OFF && connected) c = (rgb_t){52, 58, 82};
            rect(pad, y, thick, bh, c);
        }
    }
}

// Chunky agent pills: large color blocks for active sources framed by BG.
static void draw_chunky_agent_pills(const ksui_view_t *v, uint32_t now)
{
    int order[KSB_MAX_SOURCES];
    int n = sources_by_urgency(&v->model, order);
    if (n == 0) return;
    int show = n > 4 ? 4 : n;
    int pad = 2, gap = 3, thick = 9;
    if (v->portrait) {
        int y = s_h - pad - thick;
        rect(0, y - pad, s_w, pad * 2 + thick, BG);
        int total_w = s_w - 2 * pad - (show - 1) * gap;
        int bw = total_w / show;
        for (int k = 0; k < show; k++) {
            const ksb_source_t *s = &v->model.src[order[k]];
            rgb_t c = s->acked ? STATE_COLORS[0]
                    : s->styled ? boost_vivid((rgb_t){s->r, s->g, s->b})
                    : STATE_COLORS[s->state < KSA_SIG_COUNT ? s->state : 0];
            bool pulse_off = !s->acked && s->state == KSA_SIG_NEEDS_YOU && wave(now, 600) < 0.28f;
            rect(pad + k * (bw + gap), y, bw, thick, pulse_off ? mix(BG, c, 0.25f) : c);
        }
    } else {
        int x = s_w - pad - thick;
        rect(x - pad, 0, pad * 2 + thick, s_h, BG);
        int total_h = s_h - 2 * pad - (show - 1) * gap;
        int bh = total_h / show;
        for (int k = 0; k < show; k++) {
            const ksb_source_t *s = &v->model.src[order[k]];
            rgb_t c = s->acked ? STATE_COLORS[0]
                    : s->styled ? boost_vivid((rgb_t){s->r, s->g, s->b})
                    : STATE_COLORS[s->state < KSA_SIG_COUNT ? s->state : 0];
            bool pulse_off = !s->acked && s->state == KSA_SIG_NEEDS_YOU && wave(now, 600) < 0.28f;
            rect(x, pad + k * (bh + gap), thick, bh, pulse_off ? mix(BG, c, 0.25f) : c);
        }
    }
}

// Full-Bleed Lumi ("Screen = Lumi"). Every pixel of the 80x160 / 160x80 screen
// is Lumi's glowing body color, with giant eyes and mouth across the whole panel.
static void draw_full_bleed_lumi_screen(const ksui_view_t *v, uint32_t now)
{
    const ksb_model_t *m = &v->model;
    int top = ksb_top_source(m);
    bool needs_you = top >= 0 && m->src[top].state == KSA_SIG_NEEDS_YOU;
    bool error = now < v->error_until_ms || (top >= 0 && m->src[top].state == KSA_SIG_ERROR);
    bool scanning = v->link != KSUI_LINK_CONNECTED || v->claim == KSUI_CLAIM_SEARCHING;

    bool asleep;
    rgb_t body = body_color(v, now, &asleep);
    asleep = asleep && !scanning && !needs_you && !error;

    if (scanning && !needs_you && !error) {
        body = mix((rgb_t){58, 132, 245}, (rgb_t){105, 180, 255}, wave(now, 1400));
    } else if (needs_you) {
        body = mix(AMBER, (rgb_t){255, 225, 110}, 0.35f * wave(now, 700));
    } else if (error) {
        body = mix(RED, (rgb_t){180, 25, 35}, 0.35f * (1.0f - wave(now, 700)));
    }

    // Fill 100% of screen with Lumi's color + soft top sheen and bottom shadow
    rect(0, 0, s_w, s_h, body);
    rgb_t hi = mix(body, (rgb_t){255, 255, 255}, asleep ? 0.10f : 0.22f);
    rgb_t lo = mix(body, BG, 0.28f);
    rect(0, 0, s_w, s_h / 7, hi);
    rect(0, s_h - s_h / 8, s_w, s_h / 8, lo);

    rgb_t ink = asleep ? (rgb_t){235, 238, 250} : (rgb_t){16, 18, 28};
    if (needs_you || error) {
        float b_alpha = 0.35f + 0.65f * wave(now, 650);
        border(5, ink, b_alpha * 0.85f);
    }

    if (v->n_lamps > 1 || scanning) draw_chunky_lamp_strip(v, now);
    draw_chunky_agent_pills(v, now);

    float bob = needs_you ? 4.0f * sinf((float)now / 85.0f)
                          : 2.5f * sinf(2.0f * (float)M_PI * (float)(now % 2400) / 2400.0f);
    float cx = s_w * 0.5f, cy = s_h * 0.52f + bob;
    float s = v->portrait ? 1.0f : 0.80f;

    float ey = cy - 13.0f * s;
    float ex = 19.5f * s;
    float look = scanning ? ((now / 850) % 2 ? 4.5f * s : -4.5f * s) : 0.0f;
    bool blink = (now % 3700) < 140;

    if (asleep || blink) {
        capsule(cx - ex - 7.5f * s, ey, cx - ex + 7.5f * s, ey, 2.8f * s, ink, 1.0f);
        capsule(cx + ex - 7.5f * s, ey, cx + ex + 7.5f * s, ey, 2.8f * s, ink, 1.0f);
    } else {
        float ry = (needs_you ? 11.5f : 9.0f) * s;
        float rx = 6.8f * s;
        ellipse(cx - ex + look, ey, rx, ry, ink);
        ellipse(cx + ex + look, ey, rx, ry, ink);
        disc(cx - ex + look - 2.0f * s, ey - ry * 0.38f, 2.8f * s, (rgb_t){255, 255, 255}, 0.95f);
        disc(cx + ex + look - 2.0f * s, ey - ry * 0.38f, 2.8f * s, (rgb_t){255, 255, 255}, 0.95f);
        disc(cx - ex + look + 2.2f * s, ey + ry * 0.36f, 1.3f * s, (rgb_t){255, 255, 255}, 0.75f);
        disc(cx + ex + look + 2.2f * s, ey + ry * 0.36f, 1.3f * s, (rgb_t){255, 255, 255}, 0.75f);
    }

    // Big rosy cheeks
    rgb_t cheek = error ? (rgb_t){255, 165, 185} : (rgb_t){255, 95, 145};
    disc(cx - 27.5f * s, cy + 7.0f * s, 7.0f * s, cheek, 0.62f);
    disc(cx + 27.5f * s, cy + 7.0f * s, 7.0f * s, cheek, 0.62f);

    // Big mouth
    float my = cy + 15.0f * s;
    if (needs_you) {
        ellipse(cx, my + 2.0f * s, 7.2f * s, 8.5f * s, ink);
    } else if (error) {
        capsule(cx - 11.5f * s, my + 6.5f * s, cx, my - 2.0f * s, 3.0f * s, ink, 1.0f);
        capsule(cx, my - 2.0f * s, cx + 11.5f * s, my + 6.5f * s, 3.0f * s, ink, 1.0f);
    } else if (asleep || scanning) {
        disc(cx, my + 2.0f * s, 4.0f * s, ink, 1.0f);
    } else {
        capsule(cx - 11.5f * s, my - 1.5f * s, cx, my + 6.0f * s, 3.0f * s, ink, 1.0f);
        capsule(cx, my + 6.0f * s, cx + 11.5f * s, my - 1.5f * s, 3.0f * s, ink, 1.0f);
    }

    if (asleep) {
        uint32_t t = now % 2200;
        float zx = cx + 21.0f * s + (float)(t / 450);
        float zy = cy - 34.0f * s - (float)(t / 260);
        capsule(zx - 6.0f * s, zy - 6.5f * s, zx + 6.0f * s, zy - 6.5f * s, 2.2f * s, ink, 0.75f);
        capsule(zx + 6.0f * s, zy - 6.5f * s, zx - 6.0f * s, zy + 6.5f * s, 2.2f * s, ink, 0.75f);
        capsule(zx - 6.0f * s, zy + 6.5f * s, zx + 6.0f * s, zy + 6.5f * s, 2.2f * s, ink, 0.75f);
    }
}

void ksui_render(uint16_t *fb, const ksui_view_t *v, uint32_t now)
{
    s_fb = fb;
    s_w = v->portrait ? KSUI_H : KSUI_W;
    s_h = v->portrait ? KSUI_W : KSUI_H;

    // 1. Any update (signal, manual color change, ack, bad frame, claim, button hold)
    //    flashes the ENTIRE screen with a high-contrast color + giant icon.
    if (maybe_draw_update_flash(v, now)) return;

    // 2. Otherwise, show Full-Bleed Lumi across the entire screen.
    draw_full_bleed_lumi_screen(v, now);
}
