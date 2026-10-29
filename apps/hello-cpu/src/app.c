/*
 * Hello CPU - the smallest real SDK app.
 *
 * Everything here is CPU logic: time, input, heap use and the animation phase.
 * The UI calls cross into the launcher's renderer through ctx->ui, so the GPU
 * draws the same glyphs and rectangles the launcher uses.  Press B to return.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"
#include "hbx_math.h"

static struct hbx_heap heap;
static uint32_t frames;

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_input_state input;
    float phase = 0.0f;
    int32_t *numbers;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    /* Diagnostic ladder (host: --app-stage N): return after each step. */
    {
        uint32_t stage = (uint32_t)ctx->args[1];
        if (stage >= 1u && stage < 6u) {
            struct hbx_input_state probe;
            hbx_memset(&probe, 0, sizeof probe);
            if (stage >= 2u) hbx_input_poll(ctx, &probe);
            if (stage >= 3u) {
                hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
                hbx_ui_rect(ctx, 100, 100, 300, 120, 16, HBX_RGB(5, 145, 224));
            }
            if (stage >= 4u)
                hbx_ui_text(ctx, 120, 240, HBX_FONT_H1, "stage", HBX_RGB(26, 69, 102));
            if (stage >= 5u) hbx_ui_present(ctx);
            return HBX_OK;
        }
        if (stage >= 11u && stage <= 13u) {
            /* Compare loop disciplines over 30 frames:
             * 11 = launcher-style: draw, present, plain 15 ms sleep
             * 12 = + hbx_frame_end
             * 13 = frame_end replaced by hbx_pace */
            struct hbx_input_state probe;
            uint64_t deadline = 0;
            uint32_t i;
            hbx_memset(&probe, 0, sizeof probe);
            for (i = 0; i < 30; i++) {
                hbx_input_poll(ctx, &probe);
                hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
                hbx_ui_rect(ctx, 100, 100, 300, 120, 16, HBX_RGB(5, 145, 224));
                hbx_ui_text(ctx, 120, 240, HBX_FONT_H1, "loop", HBX_RGB(26, 69, 102));
                hbx_ui_present(ctx);
                if (stage == 12u) hbx_frame_end(ctx);
                if (stage == 13u) hbx_pace(ctx, &deadline, 60);
                else hbx_sleep_ns(15000000ull);
                if (stage == 12u) hbx_frame_end(ctx);
            }
            return HBX_OK;
        }
        if (stage >= 6u && stage <= 10u) {
            struct hbx_input_state probe;
            hbx_memset(&probe, 0, sizeof probe);
            /* Loop ladder: 6 = one frame + frame_end; 7 = + pace;
             * 8 = 2 frames; 9 = 5 frames; 10 = 30 frames. */
            uint32_t count = stage == 6u ? 1u : stage == 7u ? 1u :
                             stage == 8u ? 2u : stage == 9u ? 5u : 30u;
            uint64_t deadline = 0;
            uint32_t i;
            for (i = 0; i < count; i++) {
                hbx_input_poll(ctx, &probe);
                hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
                hbx_ui_rect(ctx, 100, 100, 300, 120, 16, HBX_RGB(5, 145, 224));
                hbx_ui_text(ctx, 120, 240, HBX_FONT_H1, "loop", HBX_RGB(26, 69, 102));
                hbx_ui_present(ctx);
                hbx_frame_end(ctx);
                if (stage >= 7u) hbx_pace(ctx, &deadline, 60);
            }
            return HBX_OK;
        }
    }
    hbx_heap_init(&heap, (void *)(uintptr_t)ctx->heap_base, ctx->heap_size);
    hbx_log(ctx, "hello-cpu: entry");
    hbx_memset(&input, 0, sizeof input);
    hbx_input_poll(ctx, &input);

    numbers = (int32_t *)hbx_alloc(&heap, 64 * sizeof(int32_t));
    if (!numbers) return HBX_ERR_HEAP;
    {
        int i;
        for (i = 0; i < 64; i++) numbers[i] = i * i;
    }

    while (!hbx_stop_requested(ctx)) {
        float seconds = (float)frames * (1.0f / 60.0f);
        char line[96];
        int written = 0;
        uint32_t i;

        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;

        phase = seconds * 1.6f;

        hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
        /* Decorative bands so the sample still looks like a real screen. */
        for (i = 0; i < 5; i++) {
            float y = 120.0f + (float)i * 34.0f + 12.0f * hbx_sinf(phase + (float)i);
            hbx_ui_rect(ctx, 80.0f, y, 1760.0f, 2.0f, 1.0f,
                        HBX_RGBA(20, 140, 220, 40 + (uint32_t)i * 12u));
        }
        hbx_ui_rect(ctx, 120.0f, 260.0f, 1680.0f, 560.0f, 24.0f, HBX_RGB(255, 255, 255));
        hbx_ui_stroke(ctx, 120.0f, 260.0f, 1680.0f, 560.0f, 24.0f, 1.0f,
                      HBX_RGB(210, 232, 242));

        hbx_ui_text(ctx, 168.0f, 300.0f, HBX_FONT_H1, "Hello, CPU", HBX_RGB(26, 69, 102));
        hbx_ui_text(ctx, 168.0f, 352.0f, HBX_FONT_SMALL,
                    "SDK sample: frames, time, heap and input are CPU state.",
                    HBX_RGB(99, 145, 173));

        /* A small animated meter driven by sin(). */
        hbx_ui_rect(ctx, 168.0f, 420.0f, 800.0f, 18.0f, 9.0f, HBX_RGB(232, 243, 249));
        hbx_ui_rect(ctx, 168.0f, 420.0f, 400.0f + 380.0f * hbx_sinf(phase),
                    18.0f, 9.0f, HBX_RGB(5, 145, 224));

        for (written = 0; written < (int)sizeof line - 1 && "frames " [written]; written++)
            line[written] = "frames "[written];
        {
            char digits[12];
            uint32_t n = frames, d = 0;
            do { digits[d++] = (char)('0' + n % 10u); n /= 10u; } while (n && d < 11);
            while (d) line[written++] = digits[--d];
        }
        line[written] = 0;
        hbx_ui_text(ctx, 168.0f, 480.0f, HBX_FONT_BODY, line, HBX_RGB(26, 69, 102));

        hbx_ui_text(ctx, 168.0f, 520.0f, HBX_FONT_BODY,
                    heap.used ? "heap: allocated 256 B" : "", HBX_RGB(99, 145, 173));
        hbx_ui_text(ctx, 168.0f, 556.0f, HBX_FONT_BODY,
                    input.connected ? "controller: connected" : "controller: none",
                    HBX_RGB(99, 145, 173));
        hbx_ui_text(ctx, 168.0f, 620.0f, HBX_FONT_SMALL,
                    "Press B to return to the launcher.", HBX_RGB(5, 145, 224));

        if (hbx_ui_present(ctx) != HBX_OK) break;
        frames++;
        /* Same loop discipline as the proven launcher: a plain 15 ms sleep
         * after each present, no tick reads or control-page bookkeeping in the
         * per-frame path. */
        hbx_sleep_ns(15000000ull);
    }
    hbx_free(&heap, numbers);
    hbx_logf(ctx, "hello-cpu: exit after %u frames", frames);
    return HBX_OK;
}
