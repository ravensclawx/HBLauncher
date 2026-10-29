/*
 * launcher_ui.c - the HBLauncher Native screen, generated on the CPU.
 *
 * This is a reimplementation of the original HBLauncher wave screen.  The
 * original composed the whole UI in one full-screen fragment shader; here the
 * CPU evaluates the same crest/sheet/strand/ribbon/bubble equations, lays out
 * the same grid, and emits triangles.  The GPU only rasterises them.
 *
 * Reference equations (original launcher.frag):
 *   crest(x,t)  = 336 + 30 sin(.0027x + .72t) + 14 sin(.0053x - .91t)
 *   sheet_edge  = crest + d*11 + 7 sin(x(.003+.0004d) - t(.42+.19d) + d)
 *   strand      = crest - i*25 + 13 sin(.003x + .4i + .76t)
 *   ribbon      = crest - i*67 - 25 + 22 sin(.0021x - .53t + .8i)
 *   echo        = 930 + i*24 + 48 sin(.0028x - .57t + .24i)
 * Everything is in 1920x1080 pixels, +y down; water is above the crest.
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#include "launcher_ui.h"
#include "hbx_math.h"

#define W 1920.0f
#define H 1080.0f
#define C(r, g, b) HBX_RGB((r), (g), (b))
#define A(r, g, b, a) HBX_RGBA((r), (g), (b), (a))

/* Palette, transcribed from the reference shader. */
static const uint32_t COLOR_PAPER   = A(249, 252, 254, 255);
static const uint32_t COLOR_INK     = C(26, 69, 102);
static const uint32_t COLOR_BLUE    = C(5, 145, 224);
static const uint32_t COLOR_WATER_0 = C(5, 102, 196);
static const uint32_t COLOR_WATER_1 = C(8, 184, 232);
static const uint32_t COLOR_STRAND  = C(140, 235, 255);
static const uint32_t COLOR_RIBBON  = C(102, 224, 250);
static const uint32_t COLOR_ECHO    = C(143, 214, 240);
static const uint32_t COLOR_LABEL   = C(107, 145, 166);
static const uint32_t COLOR_DESK    = C(100, 146, 175);
static const uint32_t COLOR_CREDIT  = C(97, 133, 153);
static const uint32_t COLOR_CARD    = A(255, 255, 255, 255);
static const uint32_t COLOR_CARD_INK = C(212, 230, 240);

/* ------------------------------------------------------------------ */
/* Waves                                                              */
/* ------------------------------------------------------------------ */

static float crest_at(float x, float t) {
    return 336.0f + 30.0f * hbx_sinf(x * 0.0027f + t * 0.72f) +
           14.0f * hbx_sinf(x * 0.0053f - t * 0.91f);
}

static uint32_t mix_color(uint32_t a, uint32_t b, float t) {
    int r;
    t = hbx_clampf(t, 0.0f, 1.0f);
    r = (int)((float)(a & 0xffu) + ((float)(b & 0xffu) - (float)(a & 0xffu)) * t);
    int g = (int)((float)((a >> 8) & 0xffu) + ((float)((b >> 8) & 0xffu) - (float)((a >> 8) & 0xffu)) * t);
    int bl = (int)((float)((a >> 16) & 0xffu) + ((float)((b >> 16) & 0xffu) - (float)((a >> 16) & 0xffu)) * t);
    int al = (int)((float)((a >> 24) & 0xffu) + ((float)((b >> 24) & 0xffu) - (float)((a >> 24) & 0xffu)) * t);
    return HBX_RGBA(r, g, bl, al);
}

static uint32_t with_alpha(uint32_t color, float alpha) {
    int a = (int)(alpha * 255.0f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (color & 0x00ffffffu) | ((uint32_t)a << 24);
}

/* A band from the top of the screen down to a height function. */
static void draw_band_above(struct hbx_ui_impl *impl, float time, int layer) {
    float depth = (float)layer;
    float x;
    float color = 0.0f;
    (void)time;
    for (x = 0.0f; x < W; x += 16.0f) {
        float x1 = x + 16.0f;
        float e0 = crest_at(x, time) + depth * 11.0f +
                   7.0f * hbx_sinf(x * (0.003f + depth * 0.0004f) -
                                   time * (0.42f + depth * 0.19f) + depth);
        float e1 = crest_at(x1, time) + depth * 11.0f +
                   7.0f * hbx_sinf(x1 * (0.003f + depth * 0.0004f) -
                                   time * (0.42f + depth * 0.19f) + depth);
        uint32_t sheet = with_alpha(mix_color(A(110, 214, 245, 255),
                                              A(194, 240, 252, 255), depth / 3.0f), 0.65f);
        uint32_t edge = with_alpha(C(184, 245, 255), 0.55f);
        hbx_ui_engine_band(impl, x, x1, 0.0f, e0, e1, sheet, sheet, sheet, sheet);
        /* The crest line of each sheet: a thin quad hugging the edge. */
        hbx_ui_engine_triangle(impl, x, e0 - 0.9f, x1, e1 - 0.9f, x1, e1 + 0.9f,
                               edge, edge, edge);
        hbx_ui_engine_triangle(impl, x, e0 - 0.9f, x1, e1 + 0.9f, x, e0 + 0.9f,
                               edge, edge, edge);
    }
    (void)color;
}

static void draw_water(struct hbx_ui_impl *impl, float time) {
    float x;
    for (x = 0.0f; x < W; x += 8.0f) {
        float x1 = x + 8.0f;
        float y0 = crest_at(x, time);
        float y1 = crest_at(x1, time);
        float g0 = hbx_clampf(x / 2300.0f, 0.0f, 1.0f);
        float g1 = hbx_clampf(x1 / 2300.0f, 0.0f, 1.0f);
        float ripple = 0.0f;
        uint32_t top0 = mix_color(COLOR_WATER_0, COLOR_WATER_1, g0);
        uint32_t top1 = mix_color(COLOR_WATER_0, COLOR_WATER_1, g1);
        uint32_t bot0, bot1;
        g0 = hbx_clampf(x / 2300.0f + y0 / 1500.0f, 0.0f, 1.0f);
        g1 = hbx_clampf(x1 / 2300.0f + y1 / 1500.0f, 0.0f, 1.0f);
        bot0 = mix_color(COLOR_WATER_0, COLOR_WATER_1, g0);
        bot1 = mix_color(COLOR_WATER_0, COLOR_WATER_1, g1);
        ripple = 0.023f * hbx_sinf(x * 0.003f - y0 * 0.004f + time * 0.17f);
        bot0 = mix_color(bot0, C(255, 255, 255), ripple > 0.0f ? ripple : -ripple);
        bot1 = mix_color(bot1, C(255, 255, 255), ripple > 0.0f ? ripple : -ripple);
        hbx_ui_engine_band(impl, x, x1, 0.0f, y0, y1, top0, top1, bot0, bot1);
    }
}

static void draw_strand(struct hbx_ui_impl *impl, float time, int index) {
    float step = index == 0 ? 16.0f : 24.0f;
    float x;
    float alpha = index == 0 ? 0.65f : 0.12f;
    float width = index == 0 ? 1.6f : 0.75f;
    uint32_t color = with_alpha(COLOR_STRAND, alpha);
    for (x = 0.0f; x < W; x += step) {
        float x1 = x + step;
        float h0 = crest_at(x, time) - (float)index * 25.0f +
                   13.0f * hbx_sinf(x * 0.003f + (float)index * 0.4f + time * 0.76f);
        float h1 = crest_at(x1, time) - (float)index * 25.0f +
                   13.0f * hbx_sinf(x1 * 0.003f + (float)index * 0.4f + time * 0.76f);
        hbx_ui_engine_line(impl, x, h0, x1, h1, width, color, HBX_LINE_SOFT);
    }
}

static void draw_ribbons(struct hbx_ui_impl *impl, float time) {
    int index;
    for (index = 0; index < 6; index++) {
        float x;
        for (x = 0.0f; x < W; x += 32.0f) {
            float x1 = x + 32.0f;
            float h0 = crest_at(x, time) - (float)index * 67.0f - 25.0f +
                       22.0f * hbx_sinf(x * 0.0021f - time * 0.53f + (float)index * 0.8f);
            float h1 = crest_at(x1, time) - (float)index * 67.0f - 25.0f +
                       22.0f * hbx_sinf(x1 * 0.0021f - time * 0.53f + (float)index * 0.8f);
            /* Three nested strokes approximate the reference gaussian ribbon. */
            hbx_ui_engine_line(impl, x, h0, x1, h1, 30.0f,
                               with_alpha(COLOR_RIBBON, 0.026f), HBX_LINE_SOFT);
            hbx_ui_engine_line(impl, x, h0, x1, h1, 9.0f,
                               with_alpha(COLOR_RIBBON, 0.030f), HBX_LINE_SOFT);
        }
    }
}

static void draw_echoes(struct hbx_ui_impl *impl, float time) {
    int index;
    for (index = 0; index < 8; index++) {
        float x;
        for (x = 0.0f; x < W; x += 24.0f) {
            float x1 = x + 24.0f;
            float h0 = 930.0f + (float)index * 24.0f +
                       48.0f * hbx_sinf(x * 0.0028f - time * 0.57f + (float)index * 0.24f);
            float h1 = 930.0f + (float)index * 24.0f +
                       48.0f * hbx_sinf(x1 * 0.0028f - time * 0.57f + (float)index * 0.24f);
            hbx_ui_engine_line(impl, x, h0, x1, h1, 1.3f,
                               with_alpha(COLOR_ECHO, 0.28f), HBX_LINE_SOFT);
        }
    }
}

static void draw_bubbles(struct hbx_ui_impl *impl, float time) {
    int index;
    for (index = 0; index < 32; index++) {
        float seed = (float)index;
        int rear = index >= 24;
        float speed = rear ? 30.0f + hbx_fmodf(seed, 4.0f) * 7.0f
                           : 48.0f + hbx_fmodf(seed, 6.0f) * 11.0f;
        float travel = rear ? 1280.0f : 600.0f;
        float base = rear ? 1180.0f : 480.0f;
        float cx = 50.0f + hbx_fmodf(seed * 179.0f, 1820.0f) +
                   (14.0f + hbx_fmodf(seed, 5.0f) * 5.0f) *
                       hbx_sinf(time * (0.6f + hbx_fmodf(seed, 3.0f) * 0.13f) + seed) +
                   9.0f * hbx_sinf(time * 1.7f + seed * 2.0f);
        float cy = base - hbx_fmodf(seed * 97.0f + time * speed, travel);
        float radius = 5.0f + hbx_fmodf(seed * 7.0f, 22.0f);
        float opacity = (rear ? 0.40f : 1.0f) * (0.18f + 0.05f * hbx_sinf(time + seed));
        uint32_t ring_color = with_alpha(mix_color(C(122, 204, 235), C(199, 245, 255),
                                                   rear ? 0.35f : 0.8f), opacity);
        uint32_t fill_color = with_alpha(C(207, 247, 255), opacity * 0.07f);
        hbx_ui_engine_circle(impl, cx, cy, radius, 0.0f, fill_color);
        hbx_ui_engine_circle(impl, cx, cy, radius, 0.9f, ring_color);
    }
}

void hbx_launcher_waves(struct hbx_ui_impl *impl, float time) {
    int layer;
    int index;
    if (!impl) return;
    hbx_ui_engine_rect(impl, 0.0f, 0.0f, W, H, 0.0f, COLOR_PAPER);
    for (layer = 3; layer > 0; layer--) draw_band_above(impl, time, layer);
    draw_water(impl, time);
    for (index = 0; index < 20; index++) draw_strand(impl, time, index);
    draw_ribbons(impl, time);
    draw_echoes(impl, time);
    draw_bubbles(impl, time);
}

/* ------------------------------------------------------------------ */
/* Chrome                                                             */
/* ------------------------------------------------------------------ */

static void draw_emblem(struct hbx_ui_impl *impl, float time, float spin) {
    float cx = 1660.0f, cy = 179.0f;
    float angle = spin;
    float arc[2][2];
    int i;
    hbx_ui_engine_circle(impl, cx, cy, 65.0f, 1.2f, with_alpha(C(179, 242, 255), 0.60f));

    /* Arrow arc from angle-0.6pi to angle+0.55pi, 26 segments. */
    for (i = 0; i < 26; i++) {
        float a0 = angle - 2.1f + 4.2f * (float)i / 26.0f;
        float a1 = angle - 2.1f + 4.2f * (float)(i + 1) / 26.0f;
        arc[0][0] = cx + 33.0f * hbx_cosf(a0);
        arc[0][1] = cy + 33.0f * hbx_sinf(a0);
        arc[1][0] = cx + 33.0f * hbx_cosf(a1);
        arc[1][1] = cy + 33.0f * hbx_sinf(a1);
        hbx_ui_engine_line(impl, arc[0][0], arc[0][1], arc[1][0], arc[1][1],
                           2.7f, C(255, 255, 255), HBX_LINE_SOFT);
    }
    {
        /* Arrowhead at the clockwise end of the arc, pointing along the
         * tangent. */
        float end = angle + 2.1f;
        float ex = cx + 33.0f * hbx_cosf(end);
        float ey = cy + 33.0f * hbx_sinf(end);
        float tx = -hbx_sinf(end), ty = hbx_cosf(end);
        float nx = hbx_cosf(end), ny = hbx_sinf(end);
        hbx_ui_engine_triangle(impl,
                               ex + tx * 10.0f, ey + ty * 10.0f,
                               ex - tx * 1.0f + nx * 7.0f, ey - ty * 1.0f + ny * 7.0f,
                               ex - tx * 1.0f - nx * 7.0f, ey - ty * 1.0f - ny * 7.0f,
                               C(255, 255, 255), C(255, 255, 255), C(255, 255, 255));
    }
    (void)time;
}

static void draw_header(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    hbx_ui_engine_text(impl, 164.0f, 66.0f, HBX_FONT_TINY,
                       "N I N T E N D O   S W I T C H   2", C(207, 245, 255));
    hbx_ui_engine_text(impl, 158.0f, 126.0f, HBX_FONT_TITLE, "HBLauncher", C(255, 255, 255));
    hbx_ui_engine_text(impl, 164.0f, 250.0f, HBX_FONT_BODY, "v.1.0.0", C(217, 247, 255));
    draw_emblem(impl, state->time, state->refresh_spin);
    if (state->refreshing) {
        /* Subtle, slightly breathing label under the turning emblem. */
        float alpha = 0.68f + 0.22f * hbx_sinf(state->time * 3.4f);
        hbx_ui_engine_text_center(impl, 1660.0f, 256.0f, HBX_FONT_SMALL,
                                  "Refreshing...", with_alpha(COLOR_INK, alpha));
    }

    hbx_ui_engine_text(impl, 302.0f, 430.0f, HBX_FONT_H2, "Your library", COLOR_INK);
    {
        char count_text[24];
        int n = (int)state->count;
        int i = 0;
        char digits[8];
        int d = 0;
        if (n == 0) {
            const char *zero = "0 titles";
            while (*zero) count_text[i++] = *zero++;
        } else {
            while (n > 0 && d < 7) { digits[d++] = (char)('0' + n % 10); n /= 10; }
            while (d > 0) count_text[i++] = digits[--d];
            count_text[i++] = ' ';
            if (state->count == 1) {
                const char *one = "title";
                while (*one) count_text[i++] = *one++;
            } else {
                const char *many = "titles";
                while (*many) count_text[i++] = *many++;
            }
        }
        count_text[i] = 0;
        /* Right-align the count with the card edge (x = 300 .. 1620). */
        hbx_ui_engine_text(impl, 1620.0f - hbx_ui_engine_text_width(HBX_FONT_SMALL, count_text),
                           437.0f, HBX_FONT_SMALL, count_text, COLOR_LABEL);
    }
}

/* Draw a single line truncated to `max_width`, with a plain "..." suffix. */
static void draw_text_fit(struct hbx_ui_impl *impl, float x, float y, uint32_t font,
                          const char *text, uint32_t color, float max_width) {
    char buffer[168];
    uint32_t length = 0;
    if (!text) return;
    while (text[length] && length + 1u < sizeof buffer) {
        buffer[length] = text[length];
        length++;
    }
    buffer[length] = 0;
    if (hbx_ui_engine_text_width(font, buffer) <= max_width) {
        hbx_ui_engine_text(impl, x, y, font, buffer, color);
        return;
    }
    while (length > 3u) {
        buffer[length - 1u] = 0;
        length--;
        if (hbx_ui_engine_text_width(font, buffer) <= max_width) break;
    }
    /* Replace the tail with an ellipsis made of periods (the atlas has no
     * single ellipsis glyph). */
    while (length > 3u && hbx_ui_engine_text_width(font, buffer) > max_width - 30.0f) {
        buffer[length - 1u] = 0;
        length--;
    }
    {
        uint32_t base = length > 2u ? length - 2u : 0u;
        if (base + 4u > sizeof buffer) base = (uint32_t)sizeof buffer - 4u;
        buffer[base] = '.';
        buffer[base + 1] = '.';
        buffer[base + 2] = '.';
        buffer[base + 3] = 0;
    }
    hbx_ui_engine_text(impl, x, y, font, buffer, color);
}

static const char *entry_name(const struct launcher_state *state, uint32_t index) {
    if (!state->catalog || !state->strings) return "";
    if (index >= state->count) return "";
    return state->strings + state->catalog->entries[index].name_off;
}

static const char *entry_desc(const struct launcher_state *state, uint32_t index) {
    if (!state->catalog || !state->strings) return "";
    if (index >= state->count) return "";
    return state->strings + state->catalog->entries[index].desc_off;
}

static void draw_row(struct hbx_ui_impl *impl, const struct launcher_state *state,
                     uint32_t index, uint32_t slot, float y) {
    const struct hbx_catalog_entry *entry = &state->catalog->entries[index];
    /* Eased 0..1 selection for this visible row, so the highlight glides
     * instead of snapping when the cursor moves. */
    float hi = state->row_highlight[slot];
    float pulse = 0.6f + 0.4f * hbx_sinf(state->time * 2.4f);
    float cx = 960.0f;
    float card_x = cx - 660.0f, card_y = y - 62.0f, card_w = 1320.0f, card_h = 124.0f;
    uint32_t border;

    if (hi < 0.0f) hi = 0.0f;
    if (hi > 1.0f) hi = 1.0f;
    border = mix_color(COLOR_CARD_INK, C(15, 166, 235), hi);

    /* Soft drop shadow, then the card. */
    hbx_ui_engine_rect(impl, card_x, card_y + 7.0f, card_w, card_h, 18.0f,
                       A(97, 158, 189, 20));
    hbx_ui_engine_rect(impl, card_x, card_y, card_w, card_h, 18.0f, COLOR_CARD);
    hbx_ui_engine_stroke(impl, card_x, card_y, card_w, card_h, 18.0f,
                         1.0f + 0.4f * hi, border);
    if (hi > 0.001f) {
        /* A wide, faint glow ring plus a pulsing inner wash, both eased in. */
        float bar_h = 10.0f + 8.0f * hi;
        hbx_ui_engine_stroke(impl, card_x - 5.0f, card_y - 5.0f, card_w + 10.0f,
                             card_h + 10.0f, 21.0f, 6.0f,
                             with_alpha(COLOR_BLUE, 0.045f * pulse * hi));
        hbx_ui_engine_rect(impl, card_x + 2.0f, card_y + 2.0f, card_w - 4.0f,
                           card_h - 4.0f, 16.0f,
                           with_alpha(COLOR_BLUE, 0.04f * pulse * hi));
        hbx_ui_engine_rect(impl, 288.0f, y - bar_h * 0.5f, 3.0f, bar_h, 1.5f,
                           with_alpha(COLOR_BLUE, hi));
    }

    /* Tile + icon */
    hbx_ui_engine_rect(impl, 327.0f, y - 43.0f, 86.0f, 86.0f, 12.0f,
                       mix_color(C(240, 247, 251), C(224, 245, 255), hi));
    if (entry->icon_layer != HBX_ICON_NONE && entry->icon_layer < HBX_MAX_ICON_LAYERS) {
        hbx_ui_engine_icon(impl, 334.0f, y - 36.0f, 72.0f, entry->icon_layer,
                           C(255, 255, 255));
    } else {
        char initial[2];
        float width;
        initial[0] = (char)(entry_name(state, index)[0] ? entry_name(state, index)[0] : '?');
        initial[1] = 0;
        width = hbx_ui_engine_text_width(HBX_FONT_H1, initial);
        hbx_ui_engine_text(impl, 370.0f - width * 0.5f, y - 22.0f, HBX_FONT_H1, initial,
                           mix_color(COLOR_DESK, COLOR_BLUE, hi));
    }

    draw_text_fit(impl, 450.0f, y - 37.0f, HBX_FONT_H1, entry_name(state, index),
                  COLOR_INK, 1060.0f);
    draw_text_fit(impl, 452.0f, y + 14.0f, HBX_FONT_SMALL, entry_desc(state, index),
                  C(99, 145, 173), 1058.0f);

    /* End caps: three quiet dots on the right. */
    {
        int j;
        for (j = 0; j < 3; j++)
            hbx_ui_engine_circle(impl, 1540.0f + (float)j * 9.0f, y, 1.5f, 0.0f,
                                 C(161, 194, 209));
    }
}

static void draw_empty(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    /* Centred in the same band the three rows occupy (y 489..915). */
    float cx = 960.0f, cy = 654.0f;
    hbx_ui_engine_circle(impl, cx, cy, 86.0f, 0.0f, C(230, 245, 251));
    /* A card outline with a magnifier, drawn from primitives. */
    hbx_ui_engine_stroke(impl, cx - 46.0f, cy - 36.0f, 78.0f, 72.0f, 8.0f, 1.8f,
                         C(59, 153, 201));
    {
        int i;
        for (i = 0; i < 4; i++)
            hbx_ui_engine_rect(impl, cx - 26.0f + (float)i * 11.0f, cy - 20.0f, 3.0f, 8.0f,
                               1.5f, C(122, 191, 222));
    }
    hbx_ui_engine_circle(impl, cx + 34.0f, cy + 34.0f, 20.0f, 0.0f, COLOR_CARD);
    hbx_ui_engine_circle(impl, cx + 34.0f, cy + 34.0f, 17.0f, 2.0f, COLOR_BLUE);
    hbx_ui_engine_line(impl, cx + 46.0f, cy + 46.0f, cx + 58.0f, cy + 58.0f, 3.0f,
                       COLOR_BLUE, HBX_LINE_ROUND);
    hbx_ui_engine_text_center(impl, 960.0f, 764.0f, HBX_FONT_H1, "No titles found", COLOR_INK);
    hbx_ui_engine_text_center(impl, 960.0f, 819.0f, HBX_FONT_BODY,
                              "Copy an app package into apps/ and press Y to refresh.",
                              C(107, 148, 168));
    (void)state;
}

/* Full-screen fade used on the way into a title and back out of it.  It sits
 * under the launch/error panel so the panel stays readable while the library
 * fades to the handoff colour. */
static void draw_fade(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    float fade = state->fade;
    if (fade <= 0.001f) return;
    if (fade > 1.0f) fade = 1.0f;
    hbx_ui_engine_rect(impl, 0.0f, 0.0f, W, H, 0.0f,
                       with_alpha(C(10, 44, 72), fade));
}

/* One button hint: a ringed key glyph followed by its label.  `x` is the left
 * edge of the 28 px key disc; returns the advance used (disc + gap + label). */
static float draw_hint(struct hbx_ui_impl *impl, float x, const char *key,
                       const char *label, uint32_t key_color, uint32_t text_color) {
    float width = 40.0f + hbx_ui_engine_text_width(HBX_FONT_BODY, label);
    hbx_ui_engine_circle(impl, x + 14.0f, 1017.0f, 14.0f, 1.2f, key_color);
    hbx_ui_engine_text(impl, x + 8.0f, 1010.0f, HBX_FONT_TINY, key, key_color);
    hbx_ui_engine_text(impl, x + 40.0f, 1006.0f, HBX_FONT_BODY, label, text_color);
    return width;
}

static void draw_footer(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    /* Button hints, right-aligned to the card edge (x = 1620) with an even
     * 36 px gutter.  The Y hint dims while a rescan is pending. */
    {
        static const char *const keys[3] = { "A", "B", "Y" };
        static const char *const labels[3] = { "Open", "Back", "Refresh" };
        float widths[3];
        float total = 72.0f;   /* two 36 px gutters */
        float x;
        uint32_t i;
        for (i = 0; i < 3; i++) {
            widths[i] = 40.0f + hbx_ui_engine_text_width(HBX_FONT_BODY, labels[i]);
            total += widths[i];
        }
        x = 1620.0f - total;
        draw_hint(impl, x, keys[0], labels[0], COLOR_BLUE, COLOR_INK);
        x += widths[0] + 36.0f;
        draw_hint(impl, x, keys[1], labels[1], COLOR_BLUE, COLOR_INK);
        x += widths[1] + 36.0f;
        draw_hint(impl, x, keys[2], labels[2],
                  state->refreshing ? COLOR_LABEL : COLOR_BLUE,
                  state->refreshing ? COLOR_LABEL : COLOR_INK);
    }

    hbx_ui_engine_text(impl, 36.0f, 1016.0f, HBX_FONT_TINY,
                       "@ravensclawx", COLOR_CREDIT);
    if (state->toast_frames > 0) {
        float width = hbx_ui_engine_text_width(HBX_FONT_SMALL, state->toast) + 44.0f;
        hbx_ui_engine_rect(impl, 960.0f - width * 0.5f, 296.0f, width, 40.0f, 20.0f,
                           A(255, 255, 255, 235));
        hbx_ui_engine_stroke(impl, 960.0f - width * 0.5f, 296.0f, width, 40.0f, 20.0f,
                             1.0f, C(196, 226, 242));
        hbx_ui_engine_text_center(impl, 960.0f, 305.0f, HBX_FONT_SMALL, state->toast,
                                  COLOR_INK);
    }
}

static void draw_overlay(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    float t = state->transition;
    if (t <= 0.001f && state->mode != LAUNCHER_MODE_ABOUT &&
        state->mode != LAUNCHER_MODE_ERROR)
        return;
    if (t > 0.001f) {
        hbx_ui_engine_rect(impl, 0.0f, 0.0f, W, H, 0.0f, with_alpha(C(15, 59, 94), 0.30f * t));
    }
    if (state->mode == LAUNCHER_MODE_LAUNCH) {
        float panel_w = 780.0f, panel_h = 308.0f;
        float x = 960.0f - panel_w * 0.5f, y = 540.0f - panel_h * 0.5f;
        char line[96];
        int i = 0;
        const char *prefix = "Launching ";
        const char *name = entry_name(state, state->last_index);
        while (*prefix && i < 80) line[i++] = *prefix++;
        while (*name && i < 90) line[i++] = *name++;
        line[i] = 0;

        hbx_ui_engine_rect(impl, x, y, panel_w, panel_h, 24.0f,
                           with_alpha(C(255, 255, 255), t));
        hbx_ui_engine_stroke(impl, x, y, panel_w, panel_h, 24.0f, 1.0f,
                             with_alpha(C(191, 230, 245), t));
        hbx_ui_engine_text_center(impl, 960.0f, y + 76.0f, HBX_FONT_H1, line,
                                  with_alpha(COLOR_INK, t));
        hbx_ui_engine_text_center(impl, 960.0f, y + 140.0f, HBX_FONT_SMALL,
                                  "Mapping code, data and assets", with_alpha(C(94, 135, 161), t));
        /* Spinner arc, away from the copy. */
        {
            int k;
            float angle = state->time * 3.0f;
            for (k = 0; k < 18; k++) {
                float a0 = angle + 4.0f * (float)k / 18.0f;
                float a1 = angle + 4.0f * (float)(k + 1) / 18.0f;
                hbx_ui_engine_line(impl, 960.0f + 24.0f * hbx_cosf(a0),
                                   y + 218.0f + 24.0f * hbx_sinf(a0),
                                   960.0f + 24.0f * hbx_cosf(a1),
                                   y + 218.0f + 24.0f * hbx_sinf(a1),
                                   4.0f, with_alpha(COLOR_BLUE, t * (0.15f + 0.85f * (float)k / 18.0f)),
                                   HBX_LINE_SOFT);
            }
        }
    } else if (state->mode == LAUNCHER_MODE_ERROR) {
        float panel_w = 780.0f, panel_h = 240.0f;
        float x = 960.0f - panel_w * 0.5f, y = 540.0f - panel_h * 0.5f;
        hbx_ui_engine_rect(impl, x, y, panel_w, panel_h, 24.0f,
                           with_alpha(C(255, 255, 255), t));
        hbx_ui_engine_stroke(impl, x, y, panel_w, panel_h, 24.0f, 1.0f,
                             with_alpha(C(240, 190, 200), t));
        hbx_ui_engine_text_center(impl, 960.0f, y + 54.0f, HBX_FONT_H1,
                                  "App unavailable", with_alpha(C(150, 40, 70), t));
        hbx_ui_engine_text_center(impl, 960.0f, y + 108.0f, HBX_FONT_SMALL,
                                  state->error_text, with_alpha(COLOR_INK, t));
        hbx_ui_engine_text_center(impl, 960.0f, y + 170.0f, HBX_FONT_SMALL,
                                  "Press B to return", with_alpha(COLOR_BLUE, t));
    } else if (state->mode == LAUNCHER_MODE_ABOUT) {
        float panel_w = 780.0f, panel_h = 308.0f;
        float x = 960.0f - panel_w * 0.5f, y = 540.0f - panel_h * 0.5f;
        hbx_ui_engine_rect(impl, x, y, panel_w, panel_h, 24.0f, COLOR_CARD);
        hbx_ui_engine_stroke(impl, x, y, panel_w, panel_h, 24.0f, 1.0f, C(191, 230, 245));
        hbx_ui_engine_text_center(impl, 960.0f, y + 50.0f, HBX_FONT_H1, "HBLauncher",
                                  COLOR_INK);
        hbx_ui_engine_text_center(impl, 960.0f, y + 110.0f, HBX_FONT_SMALL,
                                  "CPU layout and animation, real Vulkan GPU drawing.",
                                  C(94, 135, 161));
        hbx_ui_engine_text_center(impl, 960.0f, y + 146.0f, HBX_FONT_SMALL,
                                  "Apps load from apps/<id>/manifest.json.", C(94, 135, 161));
        hbx_ui_engine_text_center(impl, 960.0f, y + 210.0f, HBX_FONT_SMALL,
                                  "Press B to return", COLOR_BLUE);
    }
}

void hbx_launcher_draw(struct hbx_ui_impl *impl, const struct launcher_state *state) {
    uint32_t page;
    int i;
    hbx_launcher_waves(impl, state->time);
    draw_header(impl, state);

    if (state->count == 0) {
        draw_empty(impl, state);
    } else {
        page = (state->selected / LAUNCHER_ROWS) * LAUNCHER_ROWS;
        for (i = 0; i < (int)LAUNCHER_ROWS; i++) {
            uint32_t index = page + (uint32_t)i;
            float y = 551.0f + (float)i * 151.0f;
            if (index >= state->count) continue;
            draw_row(impl, state, index, (uint32_t)i, y);
        }
        /* Page dots when the library needs more than one screen; a single
         * three-row page (the shipped catalog) shows none. */
        if (state->count > LAUNCHER_ROWS) {
            uint32_t pages = (state->count + 2u) / 3u;
            uint32_t shown = pages < 8u ? pages : 8u;
            uint32_t active = page / 3u;
            uint32_t first = 0;
            uint32_t p;
            if (active >= shown) first = active - shown + 1u;
            if (first > pages - shown) first = pages - shown;
            for (p = 0; p < shown; p++) {
                uint32_t index = first + p;
                float cx = 960.0f - (float)(shown - 1u) * 9.0f + (float)p * 18.0f;
                hbx_ui_engine_circle(impl, cx, 948.0f, 3.5f, 0.0f,
                                     index == active ? COLOR_BLUE : C(198, 224, 236));
            }
        }
    }
    draw_footer(impl, state);
    draw_fade(impl, state);
    draw_overlay(impl, state);
}
