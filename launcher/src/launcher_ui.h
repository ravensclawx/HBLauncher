/*
 * launcher_ui.h - launcher presentation state and the CPU UI drawing entry.
 *
 * The drawing code in launcher_ui.c is shared with the offline host preview,
 * so the exact geometry that ships to the console can be rendered with real
 * Vulkan on a workstation and diffed frame by frame.
 */
#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include "hbx_sdk.h"
#include "hbx_app.h"
#include "../../sdk/src/hbx_ui_internal.h"

#define LAUNCHER_MODE_LIBRARY 0u
#define LAUNCHER_MODE_ABOUT   1u
#define LAUNCHER_MODE_LAUNCH  2u
#define LAUNCHER_MODE_ERROR   3u

/* Control-page flags the launcher writes (the SDK header owns bit 0; the
 * launcher must never clear it because it is the host's stop request). */
#define HBX_CTL_FLAG_REFRESH  0x00000002u  /* launcher -> host: rescan catalog */
#define HBX_CTL_FLAG_RELEASE_DISPLAY 0x00000010u  /* host -> launcher: free the vi
                                                     surface + swapchain for an
                                                     exclusive app, then return */

/* Visible rows per page and the highlight easing rate (1/8 per frame). */
#define LAUNCHER_ROWS         3u
#define LAUNCHER_HIGHLIGHT_STEP 0.125f

struct launcher_state {
    uint32_t abi;
    uint32_t count;
    uint32_t selected;
    uint32_t mode;
    float    time;            /* seconds since the launcher started */
    float    transition;      /* 0..1 overlay opacity for launch/error */
    float    fade;            /* 0 clear .. 1 fully faded (launch/resume) */
    float    refresh_spin;    /* emblem angle; advances only while refreshing */
    uint32_t refreshing;      /* a catalog rescan is pending at the host */
    float    row_highlight[LAUNCHER_ROWS]; /* eased 0..1 selection per row */
    uint32_t frame;
    uint32_t toast_frames;
    uint32_t error_status;
    uint32_t last_index;
    uint32_t fps_milli;       /* measured frame rate x 1000 */
    int32_t  toast_y;
    char     toast[64];
    char     error_text[96];
    const struct hbx_catalog *catalog;
    const char *strings;
    uint32_t strings_bytes;
};

/* Press-and-hold bookkeeping for navigation. */
struct launcher_nav {
    uint32_t held_direction;   /* 0 none, 1 up, 2 down */
    uint32_t repeat;           /* frames until the next repeat step */
    uint32_t touch_active;     /* a finger was down on the previous frame */
    uint32_t armed_index;      /* row selected by touch, awaiting a second tap */
};

/* Apply one frame of input to the launcher state (selection, modes,
 * activation).  Returns nothing; the caller handles launch/exit signalling. */
void launcher_handle_input(struct launcher_state *state, const struct hbx_input_state *input,
                           struct launcher_nav *nav);

/* Advance per-frame presentation animation: eased row highlights, the refresh
 * emblem (only spins while a rescan is pending) and the launch/resume fade. */
void launcher_step_animation(struct launcher_state *state);

/* Draw one complete launcher frame into the engine's vertex buffer.
 * The caller owns begin/present. */
void hbx_launcher_draw(struct hbx_ui_impl *impl, const struct launcher_state *state);

/* Draw only the animated wave background (exported for tests). */
void hbx_launcher_waves(struct hbx_ui_impl *impl, float time);

#endif
