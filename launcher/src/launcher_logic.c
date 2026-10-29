/*
 * launcher_logic.c - navigation and activation for the library screen.
 *
 * Kept separate from rendering and from the Vulkan bring-up so it can be
 * unit-tested on the host (tests/ui/logic.test.c): selection wrapping,
 * press-and-hold repeat, confirm/cancel, the about panel and touch.
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#include "launcher_ui.h"
#include "hbx_app.h"

void launcher_handle_input(struct launcher_state *state, const struct hbx_input_state *input,
                           struct launcher_nav *nav) {
    uint32_t *held_direction = &nav->held_direction;
    uint32_t *repeat = &nav->repeat;
    int dx = 0, dy = 0, confirm = 0, cancel = 0;

    hbx_input_nav(input, &dx, &dy, &confirm, &cancel);
    (void)dx;
    (void)dy;

    if (state->mode == LAUNCHER_MODE_LIBRARY) {
        /* Direction comes from the *held* state so a held D-pad repeats like a
         * held stick; activation still uses the edge. */
        int up = (input->buttons & HBX_UP) != 0 || input->stick_ly < -55;
        int down = (input->buttons & HBX_DOWN) != 0 || input->stick_ly > 55;
        uint32_t direction = up != down ? (up ? 1u : 2u) : 0u;

        if (direction) {
            /* Any key navigation disarms a pending touch selection. */
            nav->armed_index = 0xffffffffu;
            /* First step on a new direction, then an 18-frame delay and an
             * 8-frame repeat while it stays held. */
            if (*held_direction != direction) {
                *held_direction = direction;
                *repeat = 18;
                if (state->count) {
                    int next = (int)state->selected + (direction == 1u ? -1 : 1);
                    if (next < 0) next = (int)state->count - 1;
                    if (next >= (int)state->count) next = 0;
                    state->selected = (uint32_t)next;
                }
            }
        } else {
            *held_direction = 0;
            *repeat = 0;
        }
        if (*held_direction && *repeat > 0) {
            (*repeat)--;
            if (*repeat == 0 && state->count) {
                int next = (int)state->selected + (*held_direction == 1u ? -1 : 1);
                if (next < 0) next = (int)state->count - 1;
                if (next >= (int)state->count) next = 0;
                state->selected = (uint32_t)next;
                *repeat = 8;
            }
        }
        if (confirm && state->count) {
            state->mode = LAUNCHER_MODE_LAUNCH;
            state->last_index = state->selected;
            state->transition = 0.0f;
        } else if (input->pressed & HBX_PLUS) {
            state->mode = LAUNCHER_MODE_ABOUT;
        }
        /* Y asks the host to rescan the catalog.  The request stays latched
         * (and the emblem spins) until launcher_main sees the catalog block
         * change, so a second press while pending is a no-op. */
        if (state->mode == LAUNCHER_MODE_LIBRARY && (input->pressed & HBX_Y))
            state->refreshing = 1;
        /* Touch: the first contact selects, and a *new* tap on the row that is
         * already selected opens it.  Requiring a fresh contact means resting
         * a finger on the screen cannot launch a title by itself. */
        if (input->touch_count && !nav->touch_active) {
            /* Only the first contact activates; a second finger must not
               re-select while the first one opens a title. */
            uint32_t i;
            for (i = 0; i < 1u && i < input->touch_count; i++) {
                uint32_t page = (state->selected / 3u) * 3u;
                int row;
                for (row = 0; row < 3; row++) {
                    uint32_t index = page + (uint32_t)row;
                    float y = 551.0f + (float)row * 151.0f;
                    if (index >= state->count) continue;
                    if (hbx_touch_hit(&input->touch[i], 300, (int32_t)y - 62, 1320, 124)) {
                        if (nav->armed_index == index) {
                            /* Second tap on the same row opens it. */
                            state->mode = LAUNCHER_MODE_LAUNCH;
                            state->last_index = index;
                            state->transition = 0.0f;
                        } else {
                            state->selected = index;
                            nav->armed_index = index;
                        }
                        break;
                    }
                }
            }
        }
    } else if (state->mode == LAUNCHER_MODE_ABOUT || state->mode == LAUNCHER_MODE_ERROR) {
        if (cancel || confirm) {
            state->mode = LAUNCHER_MODE_LIBRARY;
            state->transition = 0.0f;
        }
    }
    nav->touch_active = input->touch_count > 0;
}

void launcher_step_animation(struct launcher_state *state) {
    uint32_t page = state->count ? (state->selected / LAUNCHER_ROWS) * LAUNCHER_ROWS : 0;
    uint32_t i;

    /* Selection highlight eases towards its target over ~8 frames instead of
     * snapping when the cursor moves. */
    for (i = 0; i < LAUNCHER_ROWS; i++) {
        uint32_t index = page + i;
        int active = state->count && index < state->count && index == state->selected;
        float target = active ? 1.0f : 0.0f;
        float value = state->row_highlight[i];
        if (value < target) {
            value += LAUNCHER_HIGHLIGHT_STEP;
            if (value > target) value = target;
        } else if (value > target) {
            value -= LAUNCHER_HIGHLIGHT_STEP;
            if (value < target) value = target;
        }
        state->row_highlight[i] = value;
    }

    /* The emblem is a still badge normally and only turns while the host is
     * being asked to rescan. */
    if (state->refreshing) {
        state->refresh_spin += 0.09f;
        if (state->refresh_spin > 6.2831853f) state->refresh_spin -= 6.2831853f;
    }

    /* Quick fade out on the way to a title, and back in when the launcher is
     * resumed after the app returns. */
    if (state->mode == LAUNCHER_MODE_LAUNCH) {
        state->fade += 0.16f;
        if (state->fade > 1.0f) state->fade = 1.0f;
    } else if (state->fade > 0.0f) {
        state->fade -= 0.15f;
        if (state->fade < 0.0f) state->fade = 0.0f;
    }
}
