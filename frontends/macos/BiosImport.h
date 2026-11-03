/*
 * Importing console BIOS images: from files, a ZIP (MAME's a5200.zip works)
 * or a folder, through a52session_rom_import. Shared by the Machine menu and
 * the Settings BIOS page.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a52session.h"

typedef NS_ENUM(int, A52BiosImportMode) {
    A52BiosImportFiles = 0,
    A52BiosImportZip,
    A52BiosImportFolder,
};

/* Ask for files / a ZIP / a folder, import everything the session
 * recognises in each and say what came of it in a message dialog. `done`
 * (optional) runs afterwards on the main thread with the number of images
 * imported. */
void A52ImportBios(a52session *session, NSWindow *parent, A52BiosImportMode mode,
                   void (^done)(int imported));

/* The same for one slot: the file must be that slot's image. */
void A52ImportBiosSlot(a52session *session, NSWindow *parent, int slot, void (^done)(int imported));

/* Imports paths already chosen (a drop, the command line): slot < 0 takes
 * everything recognised. Shows the message dialog. Returns the count. */
int A52ImportBiosPaths(a52session *session, NSArray<NSString *> *paths, int slot);
