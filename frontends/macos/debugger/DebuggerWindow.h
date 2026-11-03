/*
 * The debugger window (F12) over the debugger engine (a52debug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a52session.h"

@interface A52DebuggerWindow : NSObject
- (instancetype)initWithSession:(a52session *)session;
/* Show (attaching stops the machine), or hide (detaching lets it run). */
- (void)show;
- (void)toggle;
- (BOOL)isVisible;
@end
