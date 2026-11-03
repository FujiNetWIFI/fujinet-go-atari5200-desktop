/*
 * A52SettingsWindow -- see the header.
 *
 * A tab per page of a52session_pref, a heading per group, a row per
 * preference, so the four frontends show the same settings. The BIOS tab
 * also carries Import BIOS (files, a ZIP or a folder) and every BIOS slot
 * with its own Import and Remove. Rows that apply LIVE (the picture, the
 * sound, the controllers) take effect at once; the console's own options
 * wait and are applied together when the window closes (one power cycle),
 * as are the host options (FujiNet, sound, gamepads: a session restart).
 * Rows follow each other as they change: the BIOS choices by what has been
 * imported.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#import "BiosImport.h"

#include <string.h>

#define PAGE_W 600.0
#define ROW_H 30.0
#define HEAD_H 30.0
#define LABEL_W 220.0
#define CTRL_X 240.0

/* A page's document view: rows from the top. */
@interface A52FlippedView : NSView
@end
@implementation A52FlippedView
- (BOOL)isFlipped { return YES; }
@end

static NSString *str(const char *s)
{
    return s ? [NSString stringWithUTF8String:s] : @"";
}

@implementation A52SettingsWindow {
    a52session *_session;
    void (^_onLive)(void);
    void (^_onClosed)(int);
    NSWindow *_window;
    NSMutableDictionary<NSNumber *, NSView *> *_controls;  /* by pref */
    NSMutableDictionary<NSNumber *, NSTextField *> *_labels;
    NSMutableDictionary<NSNumber *, NSTextField *> *_paths;
    NSMutableDictionary<NSNumber *, NSString *> *_choiceLabels;
    NSMutableArray<NSTextField *> *_romStates;
    NSMutableArray<NSButton *> *_romRemove;
    BOOL _syncing;
    int _pending;
}

- (instancetype)initWithSession:(a52session *)session
                         onLive:(void (^)(void))onLive
                       onClosed:(void (^)(int applied))onClosed
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _onLive = onLive;
    _onClosed = onClosed;
    return self;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)r bold:(BOOL)bold
{
    NSTextField *l = [NSTextField labelWithString:text];
    [l setFrame:r];
    if (bold) [l setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [l setLineBreakMode:NSLineBreakByTruncatingTail];
    return l;
}

- (void)changed:(int)i
{
    const a52_pref *p = a52session_pref(i);
    if (p->applies > _pending) _pending = p->applies;
    if (_pending == A52_APPLY_LIVE) {
        a52session_apply_settings(_session);
        if (_onLive) _onLive();
    }
    [self refresh];
}

/* ---- the rows' actions ---------------------------------------------------- */

- (void)boolChanged:(NSButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    a52session_set_int(_session, a52session_pref(i)->key, [b state] == NSControlStateValueOn);
    [self changed:i];
}

- (void)choiceChanged:(NSPopUpButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    const int c = (int)[b indexOfSelectedItem];
    if (c < 0) return;
    if (!a52session_pref_choice_enabled(_session, i, c)) {
        /* not imported, or not on this machine: put it back */
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Not Available"];
        [a setInformativeText:@"Import that BIOS first (the BIOS tab)."];
        [a beginSheetModalForWindow:_window completionHandler:nil];
        [self refresh];
        return;
    }
    a52session_pref_set_choice(_session, i, c);
    [self changed:i];
}

- (void)intChanged:(id)sender
{
    if (_syncing) return;
    const int i = (int)[sender tag];
    const a52_pref *p = a52session_pref(i);
    int v = [sender intValue];
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    if (v == a52session_get_int(_session, p->key, p->def)) return;
    a52session_set_int(_session, p->key, v);
    [self changed:i];
}

/* A text row: valid text is stored and marks the change; anything else is
 * shown in red and not stored, so the last good text stays in force. */
- (void)textChanged:(NSTextField *)f
{
    if (_syncing) return;
    const int i = (int)[f tag];
    NSString *text = [[f stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    char err[200];
    if (a52session_pref_set_text(_session, i, [text UTF8String], err, sizeof err) != 0) {
        [f setTextColor:[NSColor systemRedColor]];
        [f setToolTip:str(err)];
        return;
    }
    [f setTextColor:[NSColor controlTextColor]];
    [f setToolTip:str(a52session_pref(i)->help)];
    [self changed:i];
}

- (void)controlTextDidChange:(NSNotification *)note
{
    if ([[note object] isKindOfClass:[NSTextField class]]) [self textChanged:[note object]];
}

- (void)choosePath:(NSButton *)b
{
    const int i = (int)[b tag];
    const a52_pref *p = a52session_pref(i);
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:str(p->label)];
    [panel setCanChooseDirectories:p->type == A52_PREF_DIR];
    [panel setCanChooseFiles:p->type != A52_PREF_DIR];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        a52session_set_str(self->_session, p->key, [[[panel URL] path] fileSystemRepresentation]);
        [self changed:i];
    }];
}

- (void)clearPath:(NSButton *)b
{
    const int i = (int)[b tag];
    a52session_set_str(_session, a52session_pref(i)->key, "");
    [self changed:i];
}

- (void)importRom:(NSButton *)b
{
    __weak A52SettingsWindow *weakSelf = self;
    A52ImportBiosSlot(_session, _window, (int)[b tag], ^(int imported) {
        (void)imported;
        [weakSelf refresh];
    });
}

- (void)removeRom:(NSButton *)b
{
    a52session_rom_remove(_session, (int)[b tag]);
    /* the console falls back to the built-in BIOS at its next power cycle */
    if (_pending < A52_APPLY_POWER) _pending = A52_APPLY_POWER;
    [self refresh];
}

- (void)importBiosFiles:(id)sender
{
    (void)sender;
    __weak A52SettingsWindow *weakSelf = self;
    A52ImportBios(_session, _window, A52BiosImportFiles, ^(int n) { (void)n; [weakSelf refresh]; });
}

- (void)importBiosZip:(id)sender
{
    (void)sender;
    __weak A52SettingsWindow *weakSelf = self;
    A52ImportBios(_session, _window, A52BiosImportZip, ^(int n) { (void)n; [weakSelf refresh]; });
}

- (void)importBiosFolder:(id)sender
{
    (void)sender;
    __weak A52SettingsWindow *weakSelf = self;
    A52ImportBios(_session, _window, A52BiosImportFolder, ^(int n) { (void)n; [weakSelf refresh]; });
}

/* ---- everything from the store ---------------------------------------------- */

- (NSString *)choiceLabels:(int)i
{
    NSMutableString *all = [NSMutableString string];
    const char *l;
    for (int c = 0; (l = a52session_pref_choice(_session, i, c)) != NULL; c++) {
        [all appendString:str(l)];
        if (!a52session_pref_choice_enabled(_session, i, c)) [all appendString:@" (not available)"];
        [all appendString:@"\n"];
    }
    return all;
}

- (void)refresh
{
    _syncing = YES;
    for (int i = 0; i < a52session_pref_count(); i++) {
        const a52_pref *p = a52session_pref(i);
        NSNumber *key = @(i);
        NSView *ctrl = _controls[key];
        if (!ctrl) continue;
        const BOOL on = a52session_pref_enabled(_session, i) != 0;
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setEnabled:on];
        [_labels[key] setTextColor:on ? [NSColor labelColor] : [NSColor disabledControlTextColor]];
        switch (p->type) {
        case A52_PREF_BOOL:
            [(NSButton *)ctrl setState:a52session_get_int(_session, p->key, p->def)
                                           ? NSControlStateValueOn : NSControlStateValueOff];
            break;
        case A52_PREF_INT: {
            const int v = a52session_get_int(_session, p->key, p->def);
            [(NSTextField *)ctrl setIntValue:v];
            for (NSView *sib in [[ctrl superview] subviews])
                if ([sib isKindOfClass:[NSStepper class]] && [(NSStepper *)sib tag] == i) {
                    [(NSStepper *)sib setIntValue:v];
                    [(NSStepper *)sib setEnabled:on];
                }
            break;
        }
        case A52_PREF_CHOICE: {
            NSPopUpButton *pop = (NSPopUpButton *)ctrl;
            NSString *labels = [self choiceLabels:i];
            if (![labels isEqualToString:_choiceLabels[key] ? _choiceLabels[key] : @""]) {
                [pop removeAllItems];
                for (NSString *l in [labels componentsSeparatedByString:@"\n"])
                    if ([l length]) [pop addItemWithTitle:l];
                _choiceLabels[key] = labels;
            }
            const int sel = a52session_pref_get_choice(_session, i);
            [pop selectItemAtIndex:sel >= 0 && sel < [pop numberOfItems] ? sel : 0];
            break;
        }
        case A52_PREF_TEXT: {
            /* the user's own text: filled in once, never rewritten mid-edit */
            if (![[ctrl identifier] isEqualToString:@"a52-text-set"]) {
                [(NSTextField *)ctrl setStringValue:str(a52session_pref_get_text(_session, i))];
                [ctrl setIdentifier:@"a52-text-set"];
            }
            break;
        }
        default: {
            NSString *v = str(a52session_get_str(_session, p->key, ""));
            NSTextField *name = _paths[key];
            [name setStringValue:[v length] ? [v lastPathComponent] : @"None"];
            [name setToolTip:[v length] ? v : nil];
            break;
        }
        }
    }
    for (NSUInteger slot = 0; slot < [_romStates count]; slot++) {
        const BOOL have = a52session_rom_available(_session, (int)slot) != 0;
        [_romStates[slot] setStringValue:have ? @"Imported" : @"Not imported"];
        [_romRemove[slot] setEnabled:have];
    }
    _syncing = NO;
}

/* ---- building it ------------------------------------------------------------------ */

/* A page's rows, as a scrolling column. */
- (NSScrollView *)pageForRows:(NSArray<NSNumber *> *)rows note:(NSString *)note roms:(BOOL)roms
{
    A52FlippedView *doc = [[A52FlippedView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 100)];
    CGFloat y = 12;
    const char *lastGroup = NULL;
    if (note) {
        NSTextField *n = [NSTextField wrappingLabelWithString:note];
        [n setFrame:NSMakeRect(16, y, PAGE_W - 32, 34)];
        [doc addSubview:n];
        y += 40;
    }
    for (NSNumber *num in rows) {
        const int i = [num intValue];
        const a52_pref *p = a52session_pref(i);
        if (!lastGroup || strcmp(lastGroup, p->group)) {
            [doc addSubview:[self label:str(p->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = p->group;
        }
        NSTextField *label = [self label:str(p->label) frame:NSMakeRect(28, y + 5, LABEL_W - 12, 20) bold:NO];
        if (p->help) [label setToolTip:str(p->help)];
        [doc addSubview:label];
        _labels[num] = label;
        NSView *ctrl = nil;
        switch (p->type) {
        case A52_PREF_BOOL: {
            NSButton *b = [NSButton checkboxWithTitle:p->help ? str(p->help) : @""
                                               target:self action:@selector(boolChanged:)];
            [b setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
            ctrl = b;
            break;
        }
        case A52_PREF_INT: {
            NSTextField *f = [NSTextField textFieldWithString:@""];
            [f setFrame:NSMakeRect(CTRL_X, y + 3, 70, 22)];
            [f setTarget:self];
            [f setAction:@selector(intChanged:)];
            NSStepper *st = [[NSStepper alloc] initWithFrame:NSMakeRect(CTRL_X + 74, y + 2, 19, 24)];
            [st setMinValue:p->min];
            [st setMaxValue:p->max];
            [st setIncrement:p->step > 0 ? p->step : 1];
            [st setTag:i];
            [st setTarget:self];
            [st setAction:@selector(intChanged:)];
            [doc addSubview:st];
            if (p->help)
                [doc addSubview:[self label:str(p->help) frame:NSMakeRect(CTRL_X + 104, y + 5, PAGE_W - CTRL_X - 120, 20) bold:NO]];
            ctrl = f;
            break;
        }
        case A52_PREF_CHOICE: {
            NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(CTRL_X, y + 1, PAGE_W - CTRL_X - 16, 26)
                                                            pullsDown:NO];
            [pop setTarget:self];
            [pop setAction:@selector(choiceChanged:)];
            ctrl = pop;
            break;
        }
        case A52_PREF_TEXT: {
            NSTextField *f = [NSTextField textFieldWithString:@""];
            [f setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
            [f setPlaceholderString:str(a52session_pref_placeholder(i))];
            [f setToolTip:str(p->help)];
            [f setDelegate:self];
            [f setTarget:self];
            [f setAction:@selector(textChanged:)];
            ctrl = f;
            break;
        }
        default: {
            NSTextField *name = [self label:@"None" frame:NSMakeRect(CTRL_X, y + 5, PAGE_W - CTRL_X - 200, 20) bold:NO];
            [doc addSubview:name];
            _paths[num] = name;
            NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(choosePath:)];
            [choose setFrame:NSMakeRect(PAGE_W - 196, y + 1, 96, 26)];
            [choose setTag:i];
            NSButton *clear = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearPath:)];
            [clear setFrame:NSMakeRect(PAGE_W - 96, y + 1, 80, 26)];
            [clear setTag:i];
            [doc addSubview:clear];
            ctrl = choose;
            break;
        }
        }
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setTag:i];
        [doc addSubview:ctrl];
        _controls[num] = ctrl;
        y += ROW_H;
    }
    if (roms) y = [self addRomsTo:doc at:y];
    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 560)];
    [scroll setHasVerticalScroller:YES];
    [scroll setDrawsBackground:NO];
    [scroll setDocumentView:doc];
    return scroll;
}

/* The BIOS page's lower half: Import BIOS (files, ZIP, folder) and every BIOS
 * slot with its state, Import and Remove. Returns the y below it. */
- (CGFloat)addRomsTo:(NSView *)doc at:(CGFloat)y
{
    NSTextField *n = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:
        @"The built-in Altirra 5200 OS runs everything; no Atari ROM is needed. Import BIOS recognises "
        @"the real BIOS images by size and checksum, in files, a ZIP (MAME's a5200.zip works) or a "
        @"folder, and ignores the rest. Kept in %@.", str(a52session_roms_path(_session))]];
    [n setFrame:NSMakeRect(16, y + 6, PAGE_W - 32, 52)];
    [doc addSubview:n];
    y += 64;
    NSButton *files = [NSButton buttonWithTitle:@"Import Files…" target:self action:@selector(importBiosFiles:)];
    [files setFrame:NSMakeRect(28, y, 130, 26)];
    NSButton *zip = [NSButton buttonWithTitle:@"Import ZIP…" target:self action:@selector(importBiosZip:)];
    [zip setFrame:NSMakeRect(166, y, 130, 26)];
    NSButton *folder = [NSButton buttonWithTitle:@"Import Folder…" target:self action:@selector(importBiosFolder:)];
    [folder setFrame:NSMakeRect(304, y, 130, 26)];
    [doc addSubview:files];
    [doc addSubview:zip];
    [doc addSubview:folder];
    y += 40;

    const char *lastGroup = NULL;
    _romStates = [NSMutableArray array];
    _romRemove = [NSMutableArray array];
    for (int s = 0; s < a52session_rom_slot_count(); s++) {
        const a52_rom_slot *slot = a52session_rom_slot(s);
        if (!lastGroup || strcmp(lastGroup, slot->group)) {
            [doc addSubview:[self label:str(slot->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = slot->group;
        }
        [doc addSubview:[self label:str(slot->name) frame:NSMakeRect(28, y + 5, 230, 20) bold:NO]];
        NSTextField *state = [self label:@"" frame:NSMakeRect(262, y + 5, 100, 20) bold:NO];
        [doc addSubview:state];
        [_romStates addObject:state];
        NSButton *b = [NSButton buttonWithTitle:@"Import…" target:self action:@selector(importRom:)];
        [b setFrame:NSMakeRect(PAGE_W - 212, y + 1, 96, 26)];
        [b setTag:s];
        [b setToolTip:[NSString stringWithFormat:@"%u bytes", slot->size]];
        [doc addSubview:b];
        NSButton *rm = [NSButton buttonWithTitle:@"Remove" target:self action:@selector(removeRom:)];
        [rm setFrame:NSMakeRect(PAGE_W - 112, y + 1, 96, 26)];
        [rm setTag:s];
        [doc addSubview:rm];
        [_romRemove addObject:rm];
        y += ROW_H;
    }
    return y;
}

- (void)show
{
    if (_window) {
        [_window makeKeyAndOrderFront:nil];
        return;
    }
    _controls = [NSMutableDictionary dictionary];
    _labels = [NSMutableDictionary dictionary];
    _paths = [NSMutableDictionary dictionary];
    _choiceLabels = [NSMutableDictionary dictionary];
    _pending = A52_APPLY_LIVE;

    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W + 20, 600)];
    NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
    const char *page = NULL;
    for (int i = 0; i <= a52session_pref_count(); i++) {
        const a52_pref *p = i < a52session_pref_count() ? a52session_pref(i) : NULL;
        if (page && (!p || strcmp(page, p->page))) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:str(page)];
            [item setLabel:str(page)];
            [item setView:[self pageForRows:rows note:nil roms:!strcmp(page, "BIOS")]];
            [tabs addTabViewItem:item];
            [rows removeAllObjects];
        }
        if (!p) break;
        page = p->page;
        [rows addObject:@(i)];
    }

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, PAGE_W + 20, 600)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setTitle:@"Settings"];
    [_window setReleasedWhenClosed:NO];
    [_window setDelegate:self];
    [_window setContentView:tabs];
    [_window center];
    [self refresh];
    [_window makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    a52session_settings_flush(_session);
    const int did = _pending > A52_APPLY_LIVE ? a52session_apply_settings(_session) : 0;
    _window = nil;
    if (_onClosed) _onClosed(did);
}
@end
