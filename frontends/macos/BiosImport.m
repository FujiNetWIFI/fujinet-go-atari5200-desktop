/*
 * BiosImport -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "BiosImport.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

int A52ImportBiosPaths(a52session *session, NSArray<NSString *> *paths, int slot)
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSString *path in paths) {
        char msg[1024];
        msg[0] = '\0';
        const int n = a52session_rom_import(session, [path fileSystemRepresentation], slot, msg, (int)sizeof msg);
        if (n > 0) imported += n;
        if (msg[0]) {
            NSString *text = [NSString stringWithUTF8String:msg];
            if (text) [log addObject:text];
        }
    }
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:imported ? @"BIOS Imported" : @"Nothing Imported"];
    NSString *text = [log componentsJoinedByString:@"\n"];
    if (![text length])
        text = imported ? @"Imported." : @"Nothing there was a console BIOS this app recognises.";
    [a setInformativeText:text];
    [a runModal];
    return imported;
}

static void importChosen(a52session *session, NSArray<NSURL *> *urls, int slot, void (^done)(int))
{
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:[u path]];
    const int n = A52ImportBiosPaths(session, paths, slot);
    if (done) done(n);
}

static void runPanel(NSOpenPanel *panel, NSWindow *parent, void (^chosen)(NSArray<NSURL *> *))
{
    if (parent) {
        [panel beginSheetModalForWindow:parent completionHandler:^(NSModalResponse r) {
            if (r != NSModalResponseOK) return;
            NSArray<NSURL *> *urls = [panel URLs];
            /* after the sheet: the result is an alert of its own */
            dispatch_async(dispatch_get_main_queue(), ^{ chosen(urls); });
        }];
    } else if ([panel runModal] == NSModalResponseOK) {
        chosen([panel URLs]);
    }
}

void A52ImportBios(a52session *session, NSWindow *parent, A52BiosImportMode mode, void (^done)(int))
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    switch (mode) {
    case A52BiosImportZip: {
        [panel setMessage:@"Import BIOS from a ZIP (MAME's a5200.zip works)"];
        UTType *zip = [UTType typeWithFilenameExtension:@"zip"];
        if (zip) [panel setAllowedContentTypes:@[ zip ]];
        [panel setAllowsMultipleSelection:YES];
        break;
    }
    case A52BiosImportFolder:
        [panel setMessage:@"Import BIOS from a Folder (searched recursively; MAME's a5200 folder works)"];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        break;
    default:
        [panel setMessage:@"Import BIOS from ROM image files"];
        [panel setAllowsMultipleSelection:YES];
        break;
    }
    runPanel(panel, parent, ^(NSArray<NSURL *> *urls) { importChosen(session, urls, -1, done); });
}

void A52ImportBiosSlot(a52session *session, NSWindow *parent, int slot, void (^done)(int))
{
    const a52_rom_slot *rs = a52session_rom_slot(slot);
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:[NSString stringWithFormat:@"Import %s", rs ? rs->name : "BIOS"]];
    [panel setAllowsMultipleSelection:NO];
    runPanel(panel, parent, ^(NSArray<NSURL *> *urls) { importChosen(session, urls, slot, done); });
}
