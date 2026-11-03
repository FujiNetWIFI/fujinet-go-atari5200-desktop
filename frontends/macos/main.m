/*
 * FujiNet Go Atari 5200 -- the macOS (AppKit) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "a52session.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        /* The runtime is packed beside the executable (see CMakeLists.txt);
         * tell the session where, so a fresh install provisions its tree
         * from the bundle rather than hunting. */
        a52session_paths paths = { NULL, NULL, NULL, NULL };
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString *runtime = [exeDir stringByAppendingPathComponent:@"fujinet"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:runtime])
            paths.fujinet_runtime_src = [runtime fileSystemRepresentation];

        a52session *session = a52session_new(&paths);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* A path on the command line is routed as a dropped file would be:
         * a cartridge boots, a BIOS is imported, anything else goes to
         * FujiNet's SD folder (see -[A52AppDelegate loadMediaPaths:]). macOS
         * adds -psn_ and -NS* arguments of its own: those are not files. */
        A52AppDelegate *delegate = [[A52AppDelegate alloc]
            initWithSession:session mediaPath:(argc > 1 && argv[1][0] != '-' ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
