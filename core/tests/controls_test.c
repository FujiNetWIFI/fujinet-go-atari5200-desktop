/*
 * controls_test -- the Controllers window's layout, pure (no session): one
 * CX52 per port, 4 or 2, with its stick, four fire buttons (an upper FIRE pair
 * and a lower FIRE 2 pair), START / PAUSE / RESET and the 12-key pad; every
 * part inside the extent and driving a target of its own port; the hit test
 * (the stick is a circle, the topmost pressable part wins); the stick drag
 * maths; and the shared colour defines.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a52session.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static a52_control g_ctl[A52_CONTROLS_MAX];

static int count_kind(int num, int kind, int port)
{
    int idx, total = 0;
    for (idx = 0; idx < num; idx++)
        if (g_ctl[idx].kind == kind && g_ctl[idx].port == port) total++;
    return total;
}

static int find_target(int num, int target, int arg)
{
    int idx;
    for (idx = 0; idx < num; idx++)
        if (g_ctl[idx].target == target && (arg < 0 || g_ctl[idx].arg == arg)) return idx;
    return -1;
}

static void centre(const a52_control *ctl, float *xpos, float *ypos)
{
    *xpos = ctl->x + ctl->w / 2;
    *ypos = ctl->y + ctl->h / 2;
}

int main(void)
{
    float width, height, xpos, ypos, wid2, hgt2;
    int num, num2, port, idx, key, xval, yval, hit, pressable;

    /* four controllers */
    num = a52session_controls_layout(4, g_ctl, A52_CONTROLS_MAX, &width, &height);
    check(num > 0 && num <= A52_CONTROLS_MAX, "the layout fits A52_CONTROLS_MAX");
    check(num == 4 * 28 + 1, "28 parts per controller and the note");
    check(fabsf(width - (4 * 13.4f + 3 * 1.0f)) < 0.01f, "four 13.4 u cells with 1.0 u between");
    check(height > 33.0f && height < 36.0f, "about 34 u tall");

    for (port = 0; port < 4; port++) {
        check(count_kind(num, A52_CTL_BODY, port) == 1 && count_kind(num, A52_CTL_STICK, port) == 1
              && count_kind(num, A52_CTL_DISH, port) == 1 && count_kind(num, A52_CTL_BAND, port) == 1
              && count_kind(num, A52_CTL_WELL, port) == 1 && count_kind(num, A52_CTL_SLOT, port) == 1,
              "each controller has a body, band, dish, stick, well and slot");
        check(count_kind(num, A52_CTL_FIRE, port) == 4, "four fire buttons");
        check(count_kind(num, A52_CTL_CONSOLE_KEY, port) == 3, "START, PAUSE and RESET");
        check(count_kind(num, A52_CTL_KEYCAP, port) == 12, "twelve pad keys");
    }

    /* every part is inside the extent and drives its own port or the shared keys */
    for (idx = 0; idx < num; idx++) {
        const a52_control *ctl = &g_ctl[idx];
        if (ctl->x < -0.001f || ctl->y < -0.001f || ctl->x + ctl->w > width + 0.001f ||
            ctl->y + ctl->h > height + 0.001f) {
            printf("FAIL: part %d (%s) outside the extent\n", idx, ctl->label);
            failures++;
        }
        if (ctl->w <= 0 || ctl->h <= 0 || !ctl->label) {
            printf("FAIL: part %d has no size or label\n", idx);
            failures++;
        }
        if (ctl->kind == A52_CTL_FIRE) {
            const int act = ctl->target % A52_ACT_PER_PORT;
            if (ctl->target / A52_ACT_PER_PORT != ctl->port || (act != A52_ACT_FIRE && act != A52_ACT_FIRE2)) {
                printf("FAIL: fire %d drives another port or action\n", idx);
                failures++;
            }
        }
    }
    check(1, "all parts inside the extent, fires on their own port");

    /* the parts map to targets as DESIGN-5200.md says */
    check(find_target(num, A52_TARGET_PORT(0, A52_ACT_FIRE), 0) >= 0
          && find_target(num, A52_TARGET_PORT(0, A52_ACT_FIRE), 1) >= 0,
          "FIRE on both sides");
    idx = find_target(num, A52_TARGET_PORT(2, A52_ACT_FIRE), 0);
    hit = find_target(num, A52_TARGET_PORT(2, A52_ACT_FIRE2), 0);
    check(idx >= 0 && hit >= 0 && g_ctl[idx].y < g_ctl[hit].y, "FIRE is the upper pair, FIRE 2 the lower");
    idx = find_target(num, A52_TARGET_PORT(1, A52_ACT_FIRE), 0);
    hit = find_target(num, A52_TARGET_PORT(1, A52_ACT_FIRE), 1);
    check(g_ctl[idx].x < g_ctl[hit].x, "left side button left of the right one");
    for (key = 0; key < A52_KEY_COUNT; key++) {
        int total = 0;
        for (idx = 0; idx < num; idx++)
            if (g_ctl[idx].target == A52_TARGET_KEY(key) && g_ctl[idx].port == 0) total++;
        if (total != 1) {
            printf("FAIL: key %s appears %d times on controller 1\n", a52_key_label(key), total);
            failures++;
        }
    }
    check(1, "every one of the 15 keys once per controller");
    idx = find_target(num, A52_TARGET_KEY(A52_KEY_START), -1);
    hit = find_target(num, A52_TARGET_KEY(A52_KEY_RESET), -1);
    check(g_ctl[idx].x < g_ctl[hit].x && g_ctl[idx].y == g_ctl[hit].y, "START left of RESET, one row");
    {
        /* 1 2 3 / 4 5 6 / 7 8 9 / * 0 # */
        const int k1 = find_target(num, A52_TARGET_KEY(A52_KEY_1), -1);
        const int k5 = find_target(num, A52_TARGET_KEY(A52_KEY_5), -1);
        const int k0 = find_target(num, A52_TARGET_KEY(A52_KEY_0), -1);
        const int kstar = find_target(num, A52_TARGET_KEY(A52_KEY_STAR), -1);
        const int khash = find_target(num, A52_TARGET_KEY(A52_KEY_HASH), -1);
        check(g_ctl[k5].y > g_ctl[k1].y && g_ctl[k0].y == g_ctl[kstar].y && g_ctl[kstar].x < g_ctl[k0].x &&
              g_ctl[k0].x < g_ctl[khash].x, "the pad reads 1 2 3 / 4 5 6 / 7 8 9 / * 0 #");
        check(strcmp(g_ctl[kstar].label, "*") == 0 && strcmp(g_ctl[khash].label, "#") == 0, "legends * and #");
    }

    /* the note and the extent of two */
    check(g_ctl[num - 1].kind == A52_CTL_NOTE && g_ctl[num - 1].port == -1 &&
          !strcmp(g_ctl[num - 1].label, A52_SHARED_NOTE), "the shared-keypad note comes last");
    num2 = a52session_controls_layout(2, g_ctl, A52_CONTROLS_MAX, &wid2, &hgt2);
    check(num2 == 2 * 28 + 1 && fabsf(wid2 - (2 * 13.4f + 1.0f)) < 0.01f && fabsf(hgt2 - height) < 0.001f,
          "two controllers: half the parts, the same height");
    check(count_kind(num2, A52_CTL_STICK, 2) == 0 && count_kind(num2, A52_CTL_STICK, 1) == 1, "only ports 1 and 2");
    check(a52session_controls_layout(9, g_ctl, A52_CONTROLS_MAX, NULL, NULL) == num, "more than four is four");
    check(a52session_controls_layout(4, g_ctl, 10, NULL, NULL) == 10, "a short buffer is filled, not overrun");
    check(a52session_controls_layout(4, NULL, 0, NULL, NULL) == 0, "no buffer, no parts");

    /* hit test, on the four-controller layout */
    num = a52session_controls_layout(4, g_ctl, A52_CONTROLS_MAX, &width, &height);
    idx = find_target(num, A52_TARGET_KEY(A52_KEY_7), -1);
    centre(&g_ctl[idx], &xpos, &ypos);
    check(a52session_controls_hit(g_ctl, num, xpos, ypos) == idx, "a key centre hits the key");
    check(a52session_controls_hit(g_ctl, num, g_ctl[idx].x - 0.001f, ypos) != idx, "just outside it does not");
    for (idx = 0, pressable = 0; idx < num; idx++) {
        const int kind = g_ctl[idx].kind;
        if (kind == A52_CTL_STICK || kind == A52_CTL_FIRE || kind == A52_CTL_CONSOLE_KEY || kind == A52_CTL_KEYCAP) {
            centre(&g_ctl[idx], &xpos, &ypos);
            if (a52session_controls_hit(g_ctl, num, xpos, ypos) != idx) {
                printf("FAIL: part %d (%s) centre does not hit it\n", idx, g_ctl[idx].label);
                failures++;
            }
            pressable++;
        }
    }
    check(pressable == 4 * (1 + 4 + 3 + 12), "all 80 pressable parts hit by their centre");
    check(a52session_controls_hit(g_ctl, num, 0.7f + 6.0f, 1.0f + 15.0f) == -1,
          "the silver band and the body between the parts are not hit");
    check(a52session_controls_hit(g_ctl, num, -5.0f, -5.0f) == -1 && a52session_controls_hit(g_ctl, 0, 1.0f, 1.0f) == -1,
          "nothing outside, nothing in an empty layout");

    /* the stick is a circle: its box corner misses, the dish rim beyond its drag region misses */
    for (idx = 0; idx < num && g_ctl[idx].kind != A52_CTL_STICK; idx++) {}
    check(idx < num, "found a stick");
    centre(&g_ctl[idx], &xpos, &ypos);
    check(a52session_controls_hit(g_ctl, num, g_ctl[idx].x + 0.05f, g_ctl[idx].y + 0.05f) != idx, "the box corner is not the stick");
    check(a52session_controls_hit(g_ctl, num, xpos + g_ctl[idx].w * 0.45f, ypos) == idx, "inside the circle is");
    check(a52session_controls_hit(g_ctl, num, xpos + g_ctl[idx].w * 0.55f, ypos) != idx, "outside it is not");
    check(fabsf(g_ctl[idx].w - g_ctl[idx].h) < 0.0001f, "the stick's box is square");

    /* the stick drag */
    a52session_stick_from_vector(0, 0, 2.8f, &xval, &yval);
    check(xval == 0 && yval == 0, "centre is centred");
    a52session_stick_from_vector(0.05f, -0.05f, 2.8f, &xval, &yval);
    check(xval == 0 && yval == 0, "a hair off centre is centred");
    a52session_stick_from_vector(1.4f, 0, 2.8f, &xval, &yval);
    check(xval >= 63 && xval <= 64 && yval == 0, "half the radius is half deflection");
    a52session_stick_from_vector(100.0f, 0, 2.8f, &xval, &yval);
    check(xval == 127 && yval == 0, "beyond the radius is full");
    a52session_stick_from_vector(0, -100.0f, 2.8f, &xval, &yval);
    check(xval == 0 && yval == -127, "up is negative");
    a52session_stick_from_vector(-30.0f, 30.0f, 2.8f, &xval, &yval);
    check(xval < 0 && yval > 0 && abs(xval) == abs(yval) && (xval * xval + yval * yval) <= 128 * 128,
          "a diagonal keeps its direction and is clamped on the circle");
    a52session_stick_from_vector(1.0f, 1.0f, 0.0f, &xval, &yval);
    check(xval == 0 && yval == 0, "no radius, no stick");
    a52session_stick_from_vector(1.0f, 1.0f, 2.8f, NULL, NULL);
    check(1, "NULL outputs are accepted");

    /* Map mode picks one of the four directions */
    check(a52session_stick_map_target(1, 2.0f, 0.3f, 2.8f) == A52_TARGET_PORT(1, A52_ACT_RIGHT), "right");
    check(a52session_stick_map_target(1, -2.0f, 0.3f, 2.8f) == A52_TARGET_PORT(1, A52_ACT_LEFT), "left");
    check(a52session_stick_map_target(3, 0.2f, -2.0f, 2.8f) == A52_TARGET_PORT(3, A52_ACT_UP), "up");
    check(a52session_stick_map_target(0, 0.2f, 2.0f, 2.8f) == A52_TARGET_PORT(0, A52_ACT_DOWN), "down");
    check(a52session_stick_map_target(0, 0.1f, 0.1f, 2.8f) == -1, "the middle picks nothing");
    check(a52session_stick_map_target(4, 2.0f, 0, 2.8f) == -1, "no port 5");

    /* the colours are the photo-read ones, not the old skins' */
    check(A52SESSION_CASE_RGB == 0x1E1E1E && A52SESSION_BAND_HI_RGB == 0xD8D8DC &&
          A52SESSION_BAND_LO_RGB == 0x8E8E94 && A52SESSION_KEY_RGB == 0x17171A &&
          A52SESSION_LEGEND_RGB == 0xF2C21B && A52SESSION_START_RGB == 0x3FA37A &&
          A52SESSION_RESET_RGB == 0xC8302A && A52SESSION_FIRE_RGB == 0xD8322A &&
          A52SESSION_HELD_RGB == 0xFFB04A, "the DESIGN-5200 colours");

    /* the compat shim still gives the old keypad */
    {
        int shim_count = 0;
        float shim_w = 0, shim_h = 0;
        const a52_keycap *caps = a52session_keyboard_layout(&shim_count, &shim_w, &shim_h);
        check(caps && shim_count == A52_KEY_COUNT && shim_w > 0 && shim_h > 0 && caps[A52_KEY_RESET].key == A52_KEY_RESET,
              "a52session_keyboard_layout still answers");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
