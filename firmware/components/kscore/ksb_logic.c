#include "ksb_logic.h"
#include <string.h>

static const uint8_t EFFECT_COLORS[9][3] = {
    {255, 255, 255}, // 0x82 seven-color fade
    {255, 255, 255}, // 0x83 RGB fade
    {255, 0, 0},     // 0x84 red breathing
    {0, 255, 0},     // 0x85 green
    {0, 0, 255},     // 0x86 blue
    {255, 200, 0},   // 0x87 yellow
    {0, 255, 255},   // 0x88 cyan
    {160, 0, 255},   // 0x89 purple
    {255, 255, 255}, // 0x8A white
};

static ksb_scene_t rgb_scene(uint8_t r, uint8_t g, uint8_t b, uint8_t bright)
{
    ksb_scene_t s = {.kind = KSB_SCENE_RGB, .r = r, .g = g, .b = b, .bright = bright};
    return s;
}

void ksb_init(ksb_model_t *m)
{
    memset(m, 0, sizeof(*m));
    for (int l = 0; l < KSB_MAX_LAMPS; l++) {
        m->base[l].kind = KSB_SCENE_OFF;
        m->last_on[l] = rgb_scene(255, 255, 255, 100);
    }
    m->n_lamps = 1;
    m->last_event_src = -1;
}

void ksb_set_lamp_count(ksb_model_t *m, uint8_t n)
{
    m->n_lamps = n > KSB_MAX_LAMPS ? KSB_MAX_LAMPS : n;
}

void ksb_clear_map(ksb_model_t *m)
{
    m->lamp_src_valid = 0;
    m->map_dirty = true;
}

static int mapped_lamp(const ksb_model_t *m, uint8_t id)
{
    for (int l = 0; l < KSB_MAX_LAMPS; l++) {
        if ((m->lamp_src_valid & (1u << l)) && m->lamp_src[l] == id) return l;
    }
    return -1;
}

static void map_source(ksb_model_t *m, uint8_t id, int lamp)
{
    int old = mapped_lamp(m, id);
    if (old == lamp) return;
    if (old >= 0) m->lamp_src_valid &= (uint8_t)~(1u << old);
    m->lamp_src[lamp] = id;
    m->lamp_src_valid |= (uint8_t)(1u << lamp);
    m->map_dirty = true;
}

int ksb_source_lamp(const ksb_model_t *m, int i)
{
    if (i < 0 || i >= KSB_MAX_SOURCES || !m->src[i].used) return -1;
    return mapped_lamp(m, m->src[i].id);
}

static void event(ksb_model_t *m, uint8_t kind, int src, uint32_t now)
{
    m->last_event = kind;
    m->last_event_src = (int8_t)src;
    m->last_event_ms = now;
}

static void set_base(ksb_model_t *m, uint8_t mask, ksb_scene_t s, bool restore_last)
{
    for (int l = 0; l < KSB_MAX_LAMPS; l++) {
        if (!(mask & (1u << l))) continue;
        m->base[l] = restore_last ? m->last_on[l] : s;
        if (m->base[l].kind != KSB_SCENE_OFF) m->last_on[l] = m->base[l];
    }
}

static int find_source(const ksb_model_t *m, uint8_t id)
{
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        if (m->src[i].used && m->src[i].id == id) return i;
    }
    return -1;
}

static int alloc_source(ksb_model_t *m)
{
    int victim = -1;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        if (!m->src[i].used) return i;
    }
    // Full: evict the oldest quiet source (working or acked), else the oldest.
    for (int pass = 0; pass < 2 && victim < 0; pass++) {
        for (int i = 0; i < KSB_MAX_SOURCES; i++) {
            const ksb_source_t *s = &m->src[i];
            bool quiet = s->state == KSA_SIG_WORKING || s->acked;
            if (pass == 0 && !quiet) continue;
            if (victim < 0 || (int32_t)(s->since_ms - m->src[victim].since_ms) < 0) victim = i;
        }
    }
    return victim;
}

static void apply_signal(ksb_model_t *m, const ksa_cmd_t *c, uint32_t now)
{
    uint8_t id = c->args[0], state = c->args[1] & 0x0F, pref = c->args[1] >> 4;
    int i = find_source(m, id);
    if (state == KSA_SIG_IDLE) {
        if (i >= 0) m->src[i].used = false;
        event(m, KSB_EV_SIGNAL, -1, now);
        return;
    }
    if (i < 0) {
        i = alloc_source(m);
        memset(&m->src[i], 0, sizeof(m->src[i]));
        m->src[i].used = true;
        m->src[i].id = id;
        m->src[i].state = 0xFF; // force "changed" below
    }
    ksb_source_t *s = &m->src[i];
    // Lamp mapping: a pinned devbox takes its lamp; otherwise the first free
    // claimed lamp; otherwise it stays unmapped and shows on every lamp.
    if (pref && pref <= KSB_MAX_LAMPS) {
        map_source(m, id, pref - 1);
    } else if (mapped_lamp(m, id) < 0) {
        for (int l = 0; l < m->n_lamps; l++) {
            if (!(m->lamp_src_valid & (1u << l))) {
                map_source(m, id, l);
                break;
            }
        }
    }
    size_t n = c->nargs - 2;
    if (n) {
        memcpy(s->name, c->args + 2, n);
        s->name[n] = 0;
    } else if (!s->name[0]) {
        static const char hex[] = "0123456789ABCDEF";
        s->name[0] = '#';
        s->name[1] = hex[id >> 4];
        s->name[2] = hex[id & 15];
        s->name[3] = 0;
    }
    // Re-sending the same state refreshes it but doesn't re-alert after an ack,
    // except needs-you: a Claude asking again deserves a fresh nudge.
    if (s->state != state || state == KSA_SIG_NEEDS_YOU) {
        s->state = state;
        s->acked = false;
    }
    s->since_ms = now;
    event(m, KSB_EV_SIGNAL, i, now);
}

bool ksb_apply(ksb_model_t *m, const ksa_cmd_t *c, uint32_t now)
{
    uint8_t mask = ksa_cmd_lamps(c);
    switch (c->op) {
    case KSA_OP_POWER:
        set_base(m, mask, (ksb_scene_t){.kind = KSB_SCENE_OFF}, c->args[0] != 0);
        break;
    case KSA_OP_RGB:
        set_base(m, mask, rgb_scene(c->args[0], c->args[1], c->args[2], c->args[3] > 100 ? 100 : c->args[3]), false);
        break;
    case KSA_OP_EFFECT:
        if (c->args[0] < 0x82 || c->args[0] > 0x8A || c->args[1] > 100 || c->args[2] < 1 || c->args[2] > 100) {
            return false;
        }
        set_base(m, mask, (ksb_scene_t){.kind = KSB_SCENE_EFFECT, .effect = c->args[0], .speed = c->args[1],
                                        .bright = c->args[2]}, false);
        break;
    case KSA_OP_WHITE:
        set_base(m, mask, (ksb_scene_t){.kind = KSB_SCENE_WHITE, .white = c->args[0]}, false);
        break;
    case KSA_OP_SIGNAL:
        apply_signal(m, c, now);
        return true;
    case KSA_OP_ACK:
        ksb_ack(m, mask, now);
        return true;
    default:
        return false;
    }
    event(m, KSB_EV_MANUAL, -1, now);
    return true;
}

void ksb_ack(ksb_model_t *m, uint8_t mask, uint32_t now)
{
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        ksb_source_t *s = &m->src[i];
        int lamp = ksb_source_lamp(m, i);
        bool shown_here = lamp < 0 || (mask & (1u << lamp)); // unmapped shows everywhere
        if (s->used && shown_here && s->state >= KSA_SIG_DONE && !s->acked) {
            s->acked = true;
            s->since_ms = now;
        }
    }
    event(m, KSB_EV_ACK, -1, now);
}

void ksb_tick(ksb_model_t *m, uint32_t now)
{
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        ksb_source_t *s = &m->src[i];
        if (!s->used) continue;
        uint32_t age = now - s->since_ms;
        if ((s->state == KSA_SIG_DONE && age >= KSB_DONE_TTL_MS) ||
            (s->state == KSA_SIG_WORKING && age >= KSB_WORKING_TTL_MS) ||
            (s->acked && age >= KSB_ACKED_TTL_MS)) {
            s->used = false;
        }
    }
}

int ksb_top_source_for(const ksb_model_t *m, int lamp)
{
    int top = -1;
    for (int i = 0; i < KSB_MAX_SOURCES; i++) {
        const ksb_source_t *s = &m->src[i];
        if (!s->used || s->acked || s->state < KSA_SIG_DONE) continue;
        if (lamp >= 0) {
            int sl = ksb_source_lamp(m, i);
            if (sl >= 0 && sl != lamp) continue;
        }
        if (top < 0 || s->state > m->src[top].state ||
            (s->state == m->src[top].state && (int32_t)(s->since_ms - m->src[top].since_ms) > 0)) {
            top = i;
        }
    }
    return top;
}

void ksb_desired(const ksb_model_t *m, int lamp, ksb_scene_t *out)
{
    if (lamp < 0 || lamp >= KSB_MAX_LAMPS) lamp = 0;
    int top = ksb_top_source_for(m, lamp);
    if (top < 0) {
        *out = m->base[lamp];
        return;
    }
    switch (m->src[top].state) {
    case KSA_SIG_NEEDS_YOU:
        *out = (ksb_scene_t){.kind = KSB_SCENE_EFFECT, .effect = 0x87, .speed = 70, .bright = 100};
        break;
    case KSA_SIG_ERROR:
        *out = rgb_scene(255, 0, 0, 100);
        break;
    default: // DONE
        *out = rgb_scene(0, 255, 60, 100);
        break;
    }
}

bool ksb_scene_eq(const ksb_scene_t *a, const ksb_scene_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case KSB_SCENE_OFF:    return true;
    case KSB_SCENE_RGB:    return a->r == b->r && a->g == b->g && a->b == b->b && a->bright == b->bright;
    case KSB_SCENE_EFFECT: return a->effect == b->effect && a->speed == b->speed && a->bright == b->bright;
    case KSB_SCENE_WHITE:  return a->white == b->white;
    }
    return false;
}

static void effect_rgb(uint8_t effect, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint8_t *c = EFFECT_COLORS[(effect >= 0x82 && effect <= 0x8A) ? effect - 0x82 : 0];
    *r = c[0];
    *g = c[1];
    *b = c[2];
}

void ksb_scene_rgb(const ksb_scene_t *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (s->kind) {
    case KSB_SCENE_RGB:
        *r = (uint8_t)(s->r * s->bright / 100);
        *g = (uint8_t)(s->g * s->bright / 100);
        *b = (uint8_t)(s->b * s->bright / 100);
        return;
    case KSB_SCENE_EFFECT:
        effect_rgb(s->effect, r, g, b);
        return;
    case KSB_SCENE_WHITE:
        *r = *g = s->white;
        *b = (uint8_t)(s->white * 9 / 10);
        return;
    default:
        *r = *g = *b = 0;
    }
}

int ksb_packets(const ks_profile_t *p, const ksb_scene_t *s, uint8_t pkt[2][KS_MAX_PACKET], size_t len[2])
{
    if (s->kind == KSB_SCENE_OFF) {
        len[0] = ks_pkt_power(false, pkt[0]);
        return 1;
    }
    len[0] = ks_pkt_power(true, pkt[0]);
    switch (s->kind) {
    case KSB_SCENE_RGB:
        len[1] = ks_pkt_rgb(p, s->r, s->g, s->b, s->bright, pkt[1]);
        break;
    case KSB_SCENE_EFFECT:
        len[1] = ks_pkt_effect(p, s->effect, s->speed, s->bright, pkt[1]);
        if (!len[1]) { // standard lamps: hold the effect's color instead
            uint8_t r, g, b;
            effect_rgb(s->effect, &r, &g, &b);
            len[1] = ks_pkt_rgb(p, r, g, b, s->bright, pkt[1]);
        }
        break;
    case KSB_SCENE_WHITE:
        len[1] = ks_pkt_white(p, s->white, pkt[1]);
        if (!len[1]) len[1] = ks_pkt_rgb(p, 255, 255, 230, (uint8_t)(s->white * 100 / 255), pkt[1]);
        break;
    }
    return len[1] ? 2 : 1; // power-only lamps still get the power packet
}

const char *ksb_state_name(uint8_t state)
{
    static const char *names[KSA_SIG_COUNT] = {"idle", "working", "done", "error", "needs you"};
    return state < KSA_SIG_COUNT ? names[state] : "?";
}
