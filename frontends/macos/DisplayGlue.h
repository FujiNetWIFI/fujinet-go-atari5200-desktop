/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#include "a52session.h"

#define FN_VIEW      A52DisplayView
#define FN_SESSION   a52session
#define FN_FB_W      A52SESSION_FB_MAX_WIDTH
#define FN_FB_H      A52SESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 864
#define FN_INITIAL_H 720

/* The frames carry their own pixel aspect. */
#define FN_VARIABLE_WIDTH 1

/* The mouse as the analog stick, and the screenshot and frame-count hooks
 * (DisplayExtras.inc). */
#define FN_EXTRA_IVARS \
    BOOL _dragging; \
    int _dragPort; \
    NSPoint _dragStart; \
    BOOL _fireHeld;
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* The session reports the rate x1000 (59923 or 49860). */
static inline int fn_machine_hz(FN_SESSION *s) { return (a52session_refresh_rate(s) + 500) / 1000; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    a52session_notify_vsync(s, ns);
}

/* The last frame's pixel aspect x1000 (1000 square). Written by the link's
 * thread, read by whoever draws or hit-tests. */
static _Atomic int s_fn_par = 1000;

static inline int fn_pull_frame_w(FN_SESSION *s, uint32_t *dst, int *width, int *lines, uint64_t *serial)
{
    int par = 1000;
    const int got = a52session_copy_frame(s, dst, width, lines, &par, serial);
    if (got && *width > 0 && *lines > 0) {
        atomic_store(&s_fn_par, par > 0 ? par : 1000);
        return 1;
    }
    return 0;
}

static inline double fn_picture_aspect_w(FN_SESSION *s, int width, int lines, BOOL tv)
{
    const double w = width * (atomic_load(&s_fn_par) / 1000.0), h = lines;
    if (h <= 0) return 4.0 / 3.0;
    if (!tv) return w / h;
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a52session_refresh_rate(s) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}
