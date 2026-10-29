/*
 * logic.test.c - host tests for the launcher's navigation and activation.
 *
 * Runs the same launcher_handle_input the console image links, with synthetic
 * input states.  Covers selection wrapping, hold-repeat, confirm/cancel, the
 * about panel and touch.
 */
#include <stdio.h>
#include <string.h>

#include "hbx_app.h"
#include "launcher_ui.h"

static int failures;
static int checks;

#define EXPECT(condition) do { \
    checks++; \
    if (!(condition)) { failures++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

static struct launcher_state state;
static struct launcher_nav nav;
static struct hbx_input_state input;
static struct hbx_catalog catalog;

static void reset(uint32_t count) {
    memset(&state, 0, sizeof state);
    memset(&nav, 0, sizeof nav);
    nav.armed_index = 0xffffffffu;
    memset(&input, 0, sizeof input);
    memset(&catalog, 0, sizeof catalog);
    state.abi = 1;
    state.count = count;
    state.mode = LAUNCHER_MODE_LIBRARY;
    state.selected = 0;
    state.strings = "";
    state.catalog = &catalog;
}

/* One frame with the given held buttons; `pressed` is the edge. */
static void frame(uint32_t buttons, uint32_t pressed) {
    input.buttons = buttons;
    input.pressed = pressed;
    input.touch_count = 0;
    launcher_handle_input(&state, &input, &nav);
}

static void tap(uint32_t button) { frame(button, button); frame(0, 0); }

/* One tap: a frame with the finger down, then a frame with it lifted. */
static void touch_at(int32_t x, int32_t y) {
    input.buttons = 0;
    input.pressed = 0;
    input.touch_count = 1;
    input.touch[0].x = x;
    input.touch[0].y = y;
    launcher_handle_input(&state, &input, &nav);
    input.touch_count = 0;
    launcher_handle_input(&state, &input, &nav);
}

int main(void) {
    /* Selection moves with the D-pad and wraps at both ends. */
    reset(5);
    tap(HBX_DOWN);
    EXPECT(state.selected == 1);
    tap(HBX_DOWN);
    EXPECT(state.selected == 2);
    tap(HBX_UP);
    EXPECT(state.selected == 1);
    state.selected = 0;
    tap(HBX_UP);
    EXPECT(state.selected == 4);
    tap(HBX_DOWN);
    EXPECT(state.selected == 0);

    /* Sticks navigate the same way. */
    reset(5);
    input.buttons = 0;
    input.pressed = 0;
    input.stick_ly = 80;
    launcher_handle_input(&state, &input, &nav);
    EXPECT(state.selected == 1);
    input.stick_ly = 0;
    launcher_handle_input(&state, &input, &nav);

    /* Hold-repeat: a held D-pad steps once immediately, then after the delay,
     * then every repeat interval. */
    reset(8);
    frame(HBX_DOWN, HBX_DOWN);
    EXPECT(state.selected == 1);
    for (int i = 0; i < 16; i++) frame(HBX_DOWN, 0);
    EXPECT(state.selected == 1);
    frame(HBX_DOWN, 0);              /* the 18th held frame fires the repeat */
    EXPECT(state.selected == 2);
    for (int i = 0; i < 7; i++) frame(HBX_DOWN, 0);
    EXPECT(state.selected == 2);
    frame(HBX_DOWN, 0);              /* 8-frame interval */
    EXPECT(state.selected == 3);
    frame(0, 0);                     /* release resets the timer */
    frame(HBX_DOWN, HBX_DOWN);
    EXPECT(state.selected == 4);

    /* Confirm opens the selected title; nothing happens with an empty list. */
    reset(5);
    state.selected = 3;
    tap(HBX_A);
    EXPECT(state.mode == LAUNCHER_MODE_LAUNCH);
    EXPECT(state.last_index == 3);
    reset(0);
    tap(HBX_A);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);

    /* Plus opens the about panel; B and A return to the library. */
    reset(2);
    tap(HBX_PLUS);
    EXPECT(state.mode == LAUNCHER_MODE_ABOUT);
    tap(HBX_B);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);
    tap(HBX_PLUS);
    EXPECT(state.mode == LAUNCHER_MODE_ABOUT);
    tap(HBX_A);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);

    /* The error panel also closes on B. */
    reset(2);
    state.mode = LAUNCHER_MODE_ERROR;
    state.transition = 1.0f;
    tap(HBX_B);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);
    EXPECT(state.transition == 0.0f);

    /* Touch: a tap on another row selects it; a tap on the selected row opens
     * it.  Touch coordinates are 1280x720 and scale 3:2 into the layout. */
    reset(5);
    touch_at(400, 367);              /* row 0: y = 551 -> ~367 in touch space */
    EXPECT(state.selected == 0);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);
    touch_at(400, 367);              /* second tap on the same row opens it */
    EXPECT(state.mode == LAUNCHER_MODE_LAUNCH);
    EXPECT(state.last_index == 0);

    /* Resting a finger on a row never launches, however long it stays. */
    reset(5);
    input.touch_count = 1;
    input.touch[0].x = 400;
    input.touch[0].y = 367;
    for (int i = 0; i < 30; i++) launcher_handle_input(&state, &input, &nav);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);
    input.touch_count = 0;
    launcher_handle_input(&state, &input, &nav);

    reset(5);
    touch_at(400, 468);              /* row 1: y = 702 -> ~468 */
    EXPECT(state.selected == 1);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);
    touch_at(400, 468);
    EXPECT(state.mode == LAUNCHER_MODE_LAUNCH);
    EXPECT(state.last_index == 1);

    /* A tap outside the cards changes nothing. */
    reset(5);
    touch_at(100, 100);
    EXPECT(state.selected == 0);
    EXPECT(state.mode == LAUNCHER_MODE_LIBRARY);

    /* Page navigation: selecting index 4 shows the second page; a tap on the
     * first visible row of that page selects index 3. */
    reset(5);
    state.selected = 4;
    touch_at(400, 367);
    EXPECT(state.selected == 3);

    printf("logic_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
