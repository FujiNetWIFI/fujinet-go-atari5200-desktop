/*
 * input_merge -- every way of holding a 5200 input is a separate SOURCE, and
 * what the console sees is their merge. So letting go of one source (a key
 * release, the Controllers window's mouse, a gamepad that is unplugged) can
 * never cancel another's hold, and two pads on one port do not fight.
 *
 * Sources: the host keyboard, the Controllers window's pointer, and for each
 * connected pad (by its stable owner number, 0 to A52_MAX_PADS - 1) its
 * buttons and its stick / D-pad separately.
 *
 * Per port the stick's four directions (opposed pairs cancel, as on a real
 * CX52 whose switches cannot close both), FIRE and FIRE 2 are merged; the
 * keypad and START / PAUSE / RESET are a plain OR. The analog stick is one
 * value per source and port: the strongest deflection wins, so a window drag
 * letting go (0,0) never cancels a pad's deflection.
 *
 * The functions are C so the gamepad thread (gamepad_sdl.c) can call them;
 * they live in input.cpp. This header is not input.h on purpose: atari800 has
 * an input.h of its own, and macOS file systems ignore case.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_INPUT_MERGE_H
#define A52_INPUT_MERGE_H

#include "a52session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A52_MAX_PADS 8

#define A52_SRC_KEYBOARD  0
#define A52_SRC_POINTER   1
#define A52_SRC_PAD0      2                                  /* + owner: its buttons */
#define A52_SRC_STICK0    (A52_SRC_PAD0 + A52_MAX_PADS)      /* + owner: stick, D-pad */
#define A52_SRC_COUNT     (A52_SRC_STICK0 + A52_MAX_PADS)
#define A52_SRC_PAD(owner)   (A52_SRC_PAD0 + (owner))
#define A52_SRC_STICK(owner) (A52_SRC_STICK0 + (owner))

/* Hold or release a stick direction / FIRE / FIRE 2 (target < A52_TARGET_KEY(0))
 * or a keypad / console key (target < A52_TARGET_SYSACT(0)) for a source.
 * Anything else is ignored. */
void input_press(struct a52session *s, int src, int target, int down);

/* The analog stick of a port for a source: x,y -127..127 (+ right, + down);
 * 0,0 lets go. */
void input_stick(struct a52session *s, int src, int port, int x, int y);

/* Let go of everything a connected pad holds (its buttons' and its stick's
 * sources, digital and analog). Called when a pad goes away or changes port. */
void input_pad_clear(struct a52session *s, int owner);

#ifdef __cplusplus
}
#endif

#endif /* A52_INPUT_MERGE_H */
