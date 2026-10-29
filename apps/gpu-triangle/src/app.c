/*
 * GPU Triangle - real GPU geometry from an SDK app.
 *
 * The app builds its triangle on the CPU every frame and hands the vertices to
 * the launcher's Vulkan pipeline through the documented UI escape hatch.  The
 * rasterisation, blending and present are the same GPU path the launcher uses;
 * nothing is pre-baked and nothing is a full-screen shader.
 *
 * For an app that creates its own Vulkan pipeline, see
 * sdk/examples/raw-vulkan/.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"
#include "hbx_math.h"

struct vertex {
    float x, y;
    float u, v;
    uint32_t color;
    uint32_t tex;
};

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_input_state input;
    float angle = 0.0f;
    uint32_t frame = 0;
    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    hbx_memset(&input, 0, sizeof input);
    hbx_log(ctx, "gpu-triangle: entry");

    while (!hbx_stop_requested(ctx)) {
        struct vertex triangle[3];
        const struct hbx_render *render = hbx_ui_render(ctx);
        int i;
        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;

        hbx_ui_begin(ctx, HBX_RGB(12, 24, 38));
        /* A soft backdrop grid, also real geometry. */
        for (i = 0; i < 12; i++) {
            hbx_ui_line(ctx, 0.0f, 90.0f + (float)i * 80.0f, 1920.0f,
                        90.0f + (float)i * 80.0f, 1.0f, HBX_RGBA(80, 140, 190, 30), 0);
        }
        hbx_ui_text(ctx, 80.0f, 60.0f, HBX_FONT_H1, "GPU Triangle", HBX_RGB(235, 246, 255));
        hbx_ui_text(ctx, 80.0f, 112.0f, HBX_FONT_SMALL,
                    "CPU vertices, real Vulkan rasterisation.", HBX_RGB(140, 180, 210));

        angle += 0.02f;
        {
            float cx = 960.0f, cy = 560.0f, radius = 260.0f;
            const uint32_t colors[3] = {
                HBX_RGB(255, 92, 122), HBX_RGB(80, 220, 180), HBX_RGB(80, 160, 255)
            };
            for (i = 0; i < 3; i++) {
                float a = angle + (float)i * HBX_TAU / 3.0f;
                triangle[i].x = cx + radius * hbx_cosf(a);
                triangle[i].y = cy + radius * hbx_sinf(a);
                triangle[i].u = 0.0f;
                triangle[i].v = 0.0f;
                triangle[i].color = colors[i];
                triangle[i].tex = 0; /* solid vertex colour */
            }
        }
        if (render && render->ops->vertices)
            render->ops->vertices(render->self, triangle, 3);

        hbx_ui_text(ctx, 80.0f, 1000.0f, HBX_FONT_SMALL, "B returns to the launcher",
                    HBX_RGB(140, 180, 210));
        if (hbx_ui_present(ctx) != HBX_OK) break;
        frame++;
        /* Launcher loop discipline: one present per frame with a fixed gap. */
        hbx_sleep_ns(15000000ull);
    }
    hbx_logf(ctx, "gpu-triangle: exit after %u frames", frame);
    return HBX_OK;
}
