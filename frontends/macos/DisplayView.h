/*
 * The Atari 5200's picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a52session.h"

@interface A52DisplayView : NSView
- (instancetype)initWithSession:(a52session *)session;
/* YES: a TV's shape (the frame's pixel aspect applied); NO: square pixels. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
/* Starts or stops the frame clock to match whether the window can be seen
 * (on screen, not occluded, not miniaturized, the app not hidden). It follows
 * the window's own notifications; call it after ordering the window in. */
- (void)updateVisibility;
/* The current frame as a PNG (taken from the session, so it works while the
 * frame clock is stopped). */
- (BOOL)savePng:(NSString *)path;
/* Frames the machine has run (A52_AUTOQUIT_FRAMES). */
- (uint64_t)frames;
/* Lets go of the analog stick a mouse drag holds (focus lost). */
- (void)endDrag;
- (void)stop;
@end
