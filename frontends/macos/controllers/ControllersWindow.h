/*
 * The Controllers panel (F9): the Atari 5200's four controllers -- analog
 * stick, the two fire buttons, the 12-key pad and START / PAUSE / RESET --
 * drawn like the console's, the gamepads with their assignments, and Map
 * mode for rebinding. Hidden at start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a52session.h"

/* The keys the app itself owns (F6, F9, F11, F12, Shift+F5): called with the
 * event of one typed with the panel key; returns YES when it took it. */
typedef BOOL (^A52AppKeyHandler)(NSEvent *event);

@interface A52ControllersWindow : NSObject
- (instancetype)initWithSession:(a52session *)session appKeys:(A52AppKeyHandler)appKeys;
/* Show, or hide when showing. */
- (void)toggle;
- (void)hide;
- (BOOL)isVisible;
/* Settings changed: gamepads and bindings are read again. */
- (void)refresh;
@end
