/*
 * The Help window (Command+?): the topics of data/help/help.md, built in
 * (a52help.h). It is also the Help menu's search handler.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

@interface A52HelpWindow : NSObject <NSUserInterfaceItemSearching>
+ (A52HelpWindow *)shared;
/* Shows the Help at ident: a heading's slug or a context ("debugger/cpu",
 * "prefs/Machine"), or "?words" to search; nil stays where it was. */
- (void)showAt:(NSString *)ident;
/* The Help for what window shows (in the Help, its search). */
- (void)showFor:(NSWindow *)window;
/* A52_HELP_SCREENSHOT: the window as it looks, as a PNG. */
- (BOOL)savePng:(NSString *)path;
@end

/* What Command+? and a Help button with no context of its own open for a
 * window. */
void A52SetHelpContext(NSWindow *window, NSString *context);

/* The round Help button: opens context, or with nil its window's. */
NSButton *A52HelpButton(NSString *context);
