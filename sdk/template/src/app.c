/*
 * {{NAME}} - HBLauncher Native SDK app.
 *
 * Entry point: hbx_app_main(const struct hbx_ctx *).
 * Return HBX_OK when the user backs out; the launcher takes over again.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_input_state input;
    uint64_t deadline = 0;
    uint32_t frame = 0;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    hbx_memset(&input, 0, sizeof input);
    hbx_log(ctx, "{{ID}}: entry");

    while (!hbx_stop_requested(ctx)) {
        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;

        hbx_ui_begin(ctx, HBX_RGB(250, 252, 254));
        hbx_ui_rect(ctx, 120.0f, 220.0f, 1680.0f, 640.0f, 24.0f, HBX_RGB(255, 255, 255));
        hbx_ui_stroke(ctx, 120.0f, 220.0f, 1680.0f, 640.0f, 24.0f, 1.0f,
                      HBX_RGB(210, 232, 242));
        hbx_ui_text(ctx, 168.0f, 268.0f, HBX_FONT_H1, "{{NAME}}", HBX_RGB(26, 69, 102));
        hbx_ui_text(ctx, 168.0f, 330.0f, HBX_FONT_BODY,
                    "Edit src/app.c and rebuild with sdk/tools/hbx-build.py.",
                    HBX_RGB(99, 145, 173));
        hbx_ui_text(ctx, 168.0f, 420.0f, HBX_FONT_SMALL, "Press B to return.",
                    HBX_RGB(5, 145, 224));
        hbx_ui_present(ctx);
        frame++;
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
    hbx_logf(ctx, "{{ID}}: exit after %u frames", frame);
    return HBX_OK;
}
