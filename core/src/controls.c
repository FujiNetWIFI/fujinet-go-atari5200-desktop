/*
 * controls -- the on-screen Controllers: one layout for every toolkit and its
 * hit test. Drawn from the real CX52 controller (controllers-port/DESIGN-5200.md):
 * a long black wedge, wide at the stick end and narrower at the keypad end,
 * with START / PAUSE / RESET along the top, the analog stick in its dish, a
 * silver band, two pairs of red fire buttons on the side edges and a recessed
 * well with the 12-key pad. One controller per port the console has (four, or
 * two with the 2-port BIOS) side by side, and one line under them saying the
 * keypad and FIRE 2 are shared (atari800 scans one of each).
 *
 * Units (u): one controller's body is 12 x 31 u, +y down, the stick end at
 * the top. Nothing here is a colour (the A52SESSION_* defines in the public
 * header are). Entries are in paint order; only STICK, FIRE, CONSOLE_KEY and
 * KEYCAP are pressable (hit test: exact shapes, topmost first).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "a52session.h"

/* ---- geometry (layout units) ------------------------------------------------ */

#define BODY_W        12.0f
#define BODY_H        31.0f
#define FIRE_W        1.2f
#define FIRE_H        2.2f
#define FIRE_OUT      0.7f        /* a fire button sticks out of the body by this much */
#define CELL_W        (BODY_W + 2 * FIRE_OUT)   /* 13.4 with the fire buttons */
#define CELL_GAP      1.0f
#define CELL_PITCH    (CELL_W + CELL_GAP)
#define TOP_MARGIN    1.0f        /* the cable leaves above the body */
#define CABLE_X       9.0f
#define CABLE_W       1.0f
#define KEY_ROW_Y     1.0f
#define KEY_ROW_H     2.0f
#define KEY_ROW_W     3.0f
#define DISH_CX       6.0f
#define DISH_CY       9.5f
#define DISH_R        3.4f
#define DRAG_R        2.8f
#define BAND_Y        14.0f
#define BAND_H        2.5f
#define WELL_X        1.0f
#define WELL_Y        17.5f
#define WELL_W        10.0f
#define WELL_H        12.5f
#define PAD_KEY_W     2.6f
#define PAD_KEY_H     2.4f
#define PAD_PITCH_X   3.1f
#define PAD_PITCH_Y   3.0f
#define PAD_TAPER     0.15f       /* the outer columns move in by this much per row */
#define PAD_TOP       18.05f
#define SLOT_X        4.0f
#define SLOT_Y        30.35f
#define SLOT_W        4.0f
#define SLOT_H        0.35f
#define LABEL_Y       (TOP_MARGIN + BODY_H + 0.2f)
#define LABEL_H       0.8f
#define NOTE_Y        (LABEL_Y + LABEL_H + 0.4f)
#define NOTE_H        0.8f


static const char *const k_port_label[A52_PORTS] = {
    "Controller 1", "Controller 2", "Controller 3", "Controller 4",
};
static const char *const k_pad_label[12] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#",
};
static const int k_pad_key[12] = {
    A52_KEY_1, A52_KEY_2, A52_KEY_3, A52_KEY_4, A52_KEY_5, A52_KEY_6,
    A52_KEY_7, A52_KEY_8, A52_KEY_9, A52_KEY_STAR, A52_KEY_0, A52_KEY_HASH,
};
static const int k_console_key[3] = { A52_KEY_START, A52_KEY_PAUSE, A52_KEY_RESET };

typedef struct {
    a52_control *out;
    int max;
    int count;
} sink;

static void add(sink *dst, int kind, const char *label, int port, int target, int arg,
                float xpos, float ypos, float width, float height)
{
    a52_control *ctl;
    if (dst->count >= dst->max) return;
    ctl = &dst->out[dst->count++];
    ctl->kind = kind;
    ctl->label = label;
    ctl->port = port;
    ctl->target = target;
    ctl->arg = arg;
    ctl->x = xpos;
    ctl->y = ypos;
    ctl->w = width;
    ctl->h = height;
}

static void add_controller(sink *dst, int port, float orgx)
{
    const float orgy = TOP_MARGIN;
    int idx;

    add(dst, A52_CTL_CABLE, "cable", port, -1, 0, orgx + CABLE_X, orgy - 1.0f, CABLE_W, 1.0f);
    add(dst, A52_CTL_BODY, "CX52", port, -1, 0, orgx, orgy, BODY_W, BODY_H);
    add(dst, A52_CTL_BAND, "band", port, -1, 0, orgx, orgy + BAND_Y, BODY_W, BAND_H);
    add(dst, A52_CTL_CAPTION, "ATARI 5200", port, -1, 0,
        orgx + 3.2f, orgy + BAND_Y + 0.4f, 6.0f, 1.2f);

    for (idx = 0; idx < 3; idx++)
        add(dst, A52_CTL_CONSOLE_KEY, a52_key_label(k_console_key[idx]), port,
            A52_TARGET_KEY(k_console_key[idx]), 0,
            orgx + 1.2f + (float)idx * 3.4f, orgy + KEY_ROW_Y, KEY_ROW_W, KEY_ROW_H);

    add(dst, A52_CTL_DISH, "dish", port, -1, 0,
        orgx + DISH_CX - DISH_R, orgy + DISH_CY - DISH_R, 2 * DISH_R, 2 * DISH_R);
    add(dst, A52_CTL_STICK, "stick", port, -1, 0,
        orgx + DISH_CX - DRAG_R, orgy + DISH_CY - DRAG_R, 2 * DRAG_R, 2 * DRAG_R);

    /* the side buttons: the upper pair is FIRE, the lower pair FIRE 2 */
    for (idx = 0; idx < 2; idx++) {
        const float fire_x = idx ? BODY_W - 0.5f : -FIRE_OUT;
        add(dst, A52_CTL_FIRE, "FIRE", port, A52_TARGET_PORT(port, A52_ACT_FIRE), idx,
            orgx + fire_x, orgy + 5.0f, FIRE_W, FIRE_H);
        add(dst, A52_CTL_FIRE, "FIRE 2", port, A52_TARGET_PORT(port, A52_ACT_FIRE2), idx,
            orgx + fire_x, orgy + 7.5f, FIRE_W, FIRE_H);
    }

    add(dst, A52_CTL_WELL, "keypad", port, -1, 0, orgx + WELL_X, orgy + WELL_Y, WELL_W, WELL_H);
    for (idx = 0; idx < 12; idx++) {
        const int row = idx / 3, col = idx % 3;
        const float inward = (float)row * PAD_TAPER;   /* the outer columns move in */
        const float centre = DISH_CX + (float)(col - 1) * PAD_PITCH_X
                             + (col == 0 ? inward : col == 2 ? -inward : 0.0f);
        add(dst, A52_CTL_KEYCAP, k_pad_label[idx], port, A52_TARGET_KEY(k_pad_key[idx]), 0,
            orgx + centre - PAD_KEY_W / 2, orgy + PAD_TOP + (float)row * PAD_PITCH_Y,
            PAD_KEY_W, PAD_KEY_H);
    }
    add(dst, A52_CTL_SLOT, "overlay slot", port, -1, 0, orgx + SLOT_X, orgy + SLOT_Y, SLOT_W, SLOT_H);
    add(dst, A52_CTL_CAPTION, k_port_label[port], port, -1, 0,
        orgx + 1.0f, LABEL_Y, BODY_W - 2.0f, LABEL_H);
}

int a52session_controls_layout(int nports, a52_control *out, int max, float *width, float *height)
{
    sink dst;
    int port;
    float total;

    if (nports < 1) nports = 1;
    if (nports > A52_PORTS) nports = A52_PORTS;
    dst.out = out;
    dst.max = out ? max : 0;
    dst.count = 0;
    for (port = 0; port < nports; port++)
        add_controller(&dst, port, FIRE_OUT + (float)port * CELL_PITCH);
    total = (float)nports * CELL_W + (float)(nports - 1) * CELL_GAP;
    add(&dst, A52_CTL_NOTE, A52_SHARED_NOTE, -1, -1, 0, 0.0f, NOTE_Y, total, NOTE_H);
    if (width) *width = total;
    if (height) *height = NOTE_Y + NOTE_H;
    return dst.count;
}

static int pressable(int kind)
{
    return kind == A52_CTL_STICK || kind == A52_CTL_FIRE || kind == A52_CTL_CONSOLE_KEY
           || kind == A52_CTL_KEYCAP;
}

int a52session_controls_hit(const a52_control *ctl, int count, float xpos, float ypos)
{
    int idx;
    for (idx = count - 1; idx >= 0; idx--) {
        const a52_control *cur = &ctl[idx];
        if (!pressable(cur->kind)) continue;
        if (cur->kind == A52_CTL_STICK) {
            /* the drag region is a circle */
            const float rad = cur->w / 2;
            const float dxv = xpos - (cur->x + rad), dyv = ypos - (cur->y + rad);
            if (dxv * dxv + dyv * dyv <= rad * rad) return idx;
            continue;
        }
        if (xpos >= cur->x && xpos < cur->x + cur->w && ypos >= cur->y && ypos < cur->y + cur->h)
            return idx;
    }
    return -1;
}

void a52session_stick_from_vector(float dxv, float dyv, float radius, int *outx, int *outy)
{
    const float len = sqrtf(dxv * dxv + dyv * dyv);
    int xval = 0, yval = 0;
    if (radius > 0 && len > radius * 0.05f) {
        float frac = len / radius;
        if (frac > 1.0f) frac = 1.0f;
        xval = (int)lroundf(dxv / len * frac * 127.0f);
        yval = (int)lroundf(dyv / len * frac * 127.0f);
    }
    if (outx) *outx = xval;
    if (outy) *outy = yval;
}

int a52session_stick_map_target(int port, float dxv, float dyv, float radius)
{
    if (port < 0 || port >= A52_PORTS || radius <= 0) return -1;
    if (sqrtf(dxv * dxv + dyv * dyv) < radius * 0.2f) return -1;
    if (fabsf(dxv) >= fabsf(dyv))
        return A52_TARGET_PORT(port, dxv < 0 ? A52_ACT_LEFT : A52_ACT_RIGHT);
    return A52_TARGET_PORT(port, dyv < 0 ? A52_ACT_UP : A52_ACT_DOWN);
}

/* ---- the compat shim: the keypad as the frontends drew it before the Controllers
 * were skinned (a52session_keyboard_layout); the frontends leave it as they are
 * ported to a52session_controls_layout. ---------------------------------------- */

const a52_keycap *a52session_keyboard_layout(int *count, float *width, float *height)
{
    static a52_keycap caps[A52_KEY_COUNT];
    static int built;
    if (!built) {
        int key;
        /* START PAUSE RESET along the top, then the 3 x 4 pad */
        for (key = 0; key < A52_KEY_COUNT; key++) {
            a52_keycap *cap = &caps[key];
            cap->label = a52_key_label(key);
            cap->key = key;
            if (key >= A52_KEY_START) {
                cap->x = (float)(key - A52_KEY_START);
                cap->y = 0.0f;
                cap->w = 1.0f;
                cap->h = 0.6f;
                cap->kind = A52_KEYCAP_FUNCTION;
            }
            else {
                /* 1 2 3 / 4 5 6 / 7 8 9 / * 0 # */
                int row, col;
                if (key == A52_KEY_STAR || key == A52_KEY_0 || key == A52_KEY_HASH) {
                    row = 3;
                    col = key == A52_KEY_STAR ? 0 : key == A52_KEY_0 ? 1 : 2;
                }
                else {
                    row = (key - 1) / 3;
                    col = (key - 1) % 3;
                }
                cap->x = (float)col;
                cap->y = 0.8f + (float)row;
                cap->w = 1.0f;
                cap->h = 1.0f;
                cap->kind = A52_KEYCAP_NORMAL;
            }
        }
        built = 1;
    }
    if (count) *count = A52_KEY_COUNT;
    if (width) *width = 3.0f;
    if (height) *height = 4.8f;
    return caps;
}
