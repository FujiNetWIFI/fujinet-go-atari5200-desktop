/*
 * A52AppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * The same app as the GNOME, KDE and Win32 frontends: the Atari 5200's
 * picture with a status line under it (the FujiNet cartridge's link, the
 * console), menus for the console, the controller keys, the view and
 * FujiNet, and the Controllers, Settings and debugger windows. Command is
 * the Mac's shortcut key.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#import "BiosImport.h"
#import "DisplayView.h"
#import "KeyForward.h"
#import "SettingsWindow.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"
#import "help/HelpWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef A52_VERSION_STRING
#define A52_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go Atari 5200"
#define STATUS_HEIGHT 24.0
#define MESSAGE_SECONDS 4.0

static NSString *str(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

@class A52AppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface A52ContentView : NSView
@property (nonatomic) a52session *session;
@property (nonatomic, weak) A52AppDelegate *owner;
@end

@interface A52AppDelegate ()
- (void)loadMediaPaths:(NSArray<NSString *> *)paths;
- (void)runSysaction:(int)sa;
- (BOOL)handleAppKey:(NSEvent *)event;
@end

@implementation A52ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    /* Cmd shortcuts are the menus'. One no menu claims still arrives here,
     * but AppKit sends no keyUp while Command is down, so forwarding it
     * would leave the console holding the key. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    if (A52IsAppKey(e)) {
        if (![e isARepeat]) [self.owner handleAppKey:e];
        return;
    }
    if ([e isARepeat]) return;
    const uint32_t ks = A52KeysymFromEvent(e);
    const int sa = ks ? a52session_key_sysaction(self.session, ks) : -1;
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    A52ForwardKey(self.session, e, YES);
}

- (void)keyUp:(NSEvent *)e
{
    A52ForwardKey(self.session, e, NO);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    A52ForwardFlags(self.session, e);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
        options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if ([urls count] == 0) return NO;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:[u path]];
    /* after the drag session: what the drop does can be an alert */
    dispatch_async(dispatch_get_main_queue(), ^{ [self.owner loadMediaPaths:paths]; });
    return YES;
}
@end

/* A round light for the status line. */
@interface A52Light : NSView
@property (nonatomic) BOOL on;
@end
@implementation A52Light
- (void)setOn:(BOOL)on
{
    if (on == _on) return;
    _on = on;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = NSInsetRect([self bounds], 1, 1);
    [(_on ? A52AccentColor() : [NSColor tertiaryLabelColor]) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:b] fill];
}
@end

@implementation A52AppDelegate {
    a52session *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    NSWindow *_window;
    A52DisplayView *_display;
    A52ContentView *_content;
    A52Light *_linkLight;
    NSTextField *_statusText;
    NSString *_message;          /* a passing message in the status line */
    NSDate *_messageUntil;
    NSTimer *_statusTimer, *_fastTimer, *_hookTimer;
    unsigned _padGeneration, _fujinetGeneration;
    BOOL _noticeOpen;
    NSString *_shotPath;
    uint64_t _shotFrames, _quitFrames;

    NSMenuItem *_aspectItem, *_smoothItem, *_warpItem;

    A52ControllersWindow *_controllers;
    A52DebuggerWindow *_debugger;
    A52SettingsWindow *_settings;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(a52session *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

- (NSString *)lastError
{
    const char *e = a52session_last_error(_session);
    return (e && e[0]) ? str(e) : @"The Atari 5200 is not running.";
}

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

/* A passing message in the status line. */
- (void)say:(NSString *)message
{
    _message = message;
    _messageUntil = [NSDate dateWithTimeIntervalSinceNow:MESSAGE_SECONDS];
    [self updateStatus];
}

/* ---- launch ------------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* the 4:3 picture at three times */
    const NSRect frame = NSMakeRect(0, 0, 864, 648 + STATUS_HEIGHT);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    /* NSWindow's default is to release itself on close, which ARC then does
     * a second time: the window is kept, and closing it quits (below) */
    [_window setReleasedWhenClosed:NO];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window setContentMinSize:NSMakeSize(320, 240 + STATUS_HEIGHT)];
    A52SetHelpContext(_window, @"main");
    [_window center];

    _content = [[A52ContentView alloc] initWithFrame:frame];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    /* layer-backed, so the display's layer composites under the labels */
    [_content setWantsLayer:YES];

    _display = [[A52DisplayView alloc] initWithSession:_session];
    [_display setFrame:NSMakeRect(0, STATUS_HEIGHT, frame.size.width, frame.size.height - STATUS_HEIGHT)];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content addSubview:_display];

    /* the status line: the link light, the words */
    _linkLight = [[A52Light alloc] initWithFrame:NSMakeRect(8, 7, 10, 10)];
    _linkLight.toolTip = @"FujiNet link";
    [_content addSubview:_linkLight];
    _statusText = [NSTextField labelWithString:@""];
    _statusText.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _statusText.lineBreakMode = NSLineBreakByTruncatingTail;
    _statusText.frame = NSMakeRect(26, 4, frame.size.width - 34, 16);
    _statusText.autoresizingMask = NSViewWidthSizable;
    [_content addSubview:_statusText];

    [_window setContentView:_content];
    [self buildMenu];
    [self applyView];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];
    [_display updateVisibility];

    __weak A52AppDelegate *weakSelf = self;
    _controllers = [[A52ControllersWindow alloc] initWithSession:_session
                                                         appKeys:^BOOL(NSEvent *e) {
        return [weakSelf handleAppKey:e];
    }];
    _debugger = [[A52DebuggerWindow alloc] initWithSession:_session];

    /* the console (the built-in BIOS needs no ROMs), FujiNet first */
    [self start];
    _launched = YES;

    /* A file from the command line or from Finder goes where Open puts it. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;
    if (media) [self loadMediaPaths:@[media]];

    /* the current pads and FujiNet state are the baseline: no messages */
    _padGeneration = a52session_gamepad_generation(_session);
    _fujinetGeneration = a52session_fujinet_generation(_session);

    /* The family's launch hooks: A52_OPEN_CONTROLLERS / _DEBUGGER /
     * _SETTINGS; A52_SCREENSHOT=file.png after A52_SCREENSHOT_FRAMES
     * (300); A52_AUTOQUIT_FRAMES. */
    if (getenv("A52_OPEN_CONTROLLERS")) [self toggleControllers:nil];
    if (getenv("A52_OPEN_DEBUGGER")) [_debugger show];
    if (getenv("A52_OPEN_SETTINGS")) [self showSettings:nil];
    if (getenv("A52_SCREENSHOT") && *getenv("A52_SCREENSHOT")) {
        _shotPath = str(getenv("A52_SCREENSHOT"));
        _shotFrames = getenv("A52_SCREENSHOT_FRAMES") ? strtoull(getenv("A52_SCREENSHOT_FRAMES"), NULL, 10) : 300;
    }
    if (getenv("A52_AUTOQUIT_FRAMES")) _quitFrames = strtoull(getenv("A52_AUTOQUIT_FRAMES"), NULL, 10);
    /* the hooks count the machine's frames, not the screen's: they run even
     * when no one can see the window (CI) */
    if (_shotPath || _quitFrames)
        _hookTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
                                                       block:^(NSTimer *t) { (void)t; [weakSelf hookTick]; }];

    /* The timers below run only while the console's window can be seen: they
     * follow the same notices the display's frame clock does. */
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSString *name in @[NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                             NSWindowDidDeminiaturizeNotification, NSApplicationDidHideNotification,
                             NSApplicationDidUnhideNotification])
        [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:nil];
    [self updateTimers];
    [self updateStatus];
}

- (BOOL)windowCanBeSeen:(NSWindow *)w
{
    return w && [w isVisible] && ![w isMiniaturized] &&
           ([w occlusionState] & NSWindowOcclusionStateVisible) && ![NSApp isHidden];
}

/* The status and system-action timers, and the console log's, run while their
 * windows can be seen; they catch up once when they start again. */
- (void)updateTimers
{
    const BOOL want = _launched && [self windowCanBeSeen:_window];
    if (want && !_fastTimer) {
        __weak A52AppDelegate *weakSelf = self;
        _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf updateStatus]; }];
        _statusTimer.tolerance = 0.5;
        _fastTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf fastTick]; }];
        _fastTimer.tolerance = 0.05;
        [self updateStatus];
        [self fastTick];
    } else if (!want && _fastTimer) {
        [_statusTimer invalidate];
        [_fastTimer invalidate];
        _statusTimer = nil;
        _fastTimer = nil;
    }
    [self updateLogTimer];
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimers];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_fastTimer invalidate];
    [_hookTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    a52session_stop(_session);
    a52session_settings_flush(_session);
    a52session_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no console yet
     * to put it in: hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMediaPaths:@[filename]];
    return YES;
}

- (void)windowDidResignKey:(NSNotification *)note
{
    /* focus gone: nothing stays held (the Controllers panel takes typing too) */
    if (note.object == _window && ![_controllers isVisible]) {
        A52ForwardReleaseAll(_session);
        [_display endDrag];
    }
}

/* ---- the console ---------------------------------------------------------------- */

- (void)start
{
    if (a52session_is_running(_session)) return;
    a52session_start_opts opts;
    a52session_default_opts(_session, &opts);
    if (a52session_start(_session, &opts) != 0) {
        NSString *why = [self lastError];
        /* after launch finishes: a modal alert inside it stalls the menu bar */
        dispatch_async(dispatch_get_main_queue(), ^{ [self alert:@"Cannot Start the Atari 5200" text:why]; });
    }
    [self updateStatus];
}

/* The primary action: the console and the cartridge power off and on, so
 * CONFIG is back (an opened game is ejected). */
- (void)powerCycle:(id)sender
{
    (void)sender;
    if (!a52session_is_running(_session)) {
        [self start];
        return;
    }
    if (a52session_power_cycle(_session) != 0)
        [self alert:@"Cannot Power Cycle" text:[self lastError]];
    else
        [self say:@"Power cycled: back to FujiNet's CONFIG"];
    [self updateStatus];
}

/* The controller's RESET key, pressed and released over a few frames by the
 * session: the BIOS restarts the cartridge's current image. */
- (void)resetConsole:(id)sender
{
    (void)sender;
    a52session_reset(_session);
}

- (void)toggleWarp:(id)sender
{
    (void)sender;
    a52session_set_warp(_session, !a52session_warp(_session));
    [self updateStatus];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case A52_SYSACT_PAUSE: [_debugger show]; break;
    case A52_SYSACT_POWER: [self powerCycle:nil]; break;
    case A52_SYSACT_WARP: [self toggleWarp:nil]; break;
    default: break;
    }
}

/* F6, F9, F11, F12 and Shift+F5 are the app's, not bindable. YES when taken. */
- (BOOL)handleAppKey:(NSEvent *)e
{
    switch ([e keyCode]) {
    case kVK_F6: [self showHelp:nil]; return YES;
    case kVK_F9: [self toggleControllers:nil]; return YES;
    case kVK_F11: [self toggleFullScreen:nil]; return YES;
    case kVK_F12: [self toggleDebugger:nil]; return YES;
    case kVK_F5:
        if ([e modifierFlags] & NSEventModifierFlagShift) { [self powerCycle:nil]; return YES; }
        return NO;
    default: return NO;
    }
}

/* START, PAUSE or RESET from the menu: held for a tenth of a second, long
 * enough for the console's once-a-frame look. */
- (void)consoleKey:(NSMenuItem *)sender
{
    const int target = A52_TARGET_KEY((int)[sender tag]);
    a52session_press(_session, target, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (self->_session) a52session_press(self->_session, target, 0);
    });
}

/* ---- opening things -------------------------------------------------------------- */

- (void)rememberFolder:(NSString *)path
{
    a52session_set_str(_session, "last_dir", [[path stringByDeletingLastPathComponent] fileSystemRepresentation]);
}

- (NSURL *)mediaFolder
{
    NSString *last = str(a52session_get_str(_session, "last_dir", ""));
    BOOL dir = NO;
    if ([last length] && [[NSFileManager defaultManager] fileExistsAtPath:last isDirectory:&dir] && dir)
        return [NSURL fileURLWithPath:last];
    return [NSURL fileURLWithPath:str(a52session_media_path(_session))];
}

/* Open, a drop, Finder, the command line: a cartridge goes in (served by the
 * FujiNet cartridge, the console power cycled onto it), a BIOS is imported,
 * anything else is copied to FujiNet's SD folder. */
- (void)loadMediaPaths:(NSArray<NSString *> *)paths
{
    if (!a52session_is_running(_session)) {
        [self say:@"The Atari 5200 is off"];
        return;
    }
    for (NSString *path in paths) {
        char msg[1024];
        msg[0] = '\0';
        const int kind = a52session_open_or_import(_session, [path fileSystemRepresentation], msg, sizeof msg);
        if (kind < 0) {
            [self alert:@"Cannot Open It" text:msg[0] ? str(msg) : [self lastError]];
            continue;
        }
        [self rememberFolder:path];
        if (kind == A52_MEDIA_BIOS)
            [self alert:@"BIOS Imported" text:msg[0] ? str(msg) : [path lastPathComponent]];
        else
            [self say:msg[0] ? str(msg) : [path lastPathComponent]];
    }
}

- (void)openMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:@"Open a cartridge, or a BIOS (file, ZIP or folder) to import"];
    [panel setCanChooseDirectories:YES];
    [panel setAllowsMultipleSelection:YES];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSURL *u in [panel URLs]) [paths addObject:[u path]];
        /* after the sheet: what it does can be an alert */
        dispatch_async(dispatch_get_main_queue(), ^{ [self loadMediaPaths:paths]; });
    }];
}

- (void)importBiosFiles:(id)sender
{
    (void)sender;
    A52ImportBios(_session, _window, A52BiosImportFiles, nil);
}

- (void)importBiosZip:(id)sender
{
    (void)sender;
    A52ImportBios(_session, _window, A52BiosImportZip, nil);
}

- (void)importBiosFolder:(id)sender
{
    (void)sender;
    A52ImportBios(_session, _window, A52BiosImportFolder, nil);
}

- (void)importToSD:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:@"Copy to FujiNet's SD card"];
    [panel setAllowsMultipleSelection:YES];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        int ok = 0, failed = 0;
        for (NSURL *u in [panel URLs]) {
            char dest[1024];
            if (a52session_import_to_sd(self->_session, [[u path] fileSystemRepresentation], dest, sizeof dest) == 0)
                ok++;
            else
                failed++;
        }
        [self say:failed ? [NSString stringWithFormat:@"%d copied to FujiNet's SD card, %d could not be", ok, failed]
                         : [NSString stringWithFormat:@"%d copied to FujiNet's SD card: boot it from CONFIG", ok]];
    }];
}

- (void)openFolder:(NSString *)path
{
    [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)openSDFolder:(id)sender { (void)sender; [self openFolder:str(a52session_sd_path(_session))]; }
- (void)openMediaFolder:(id)sender { (void)sender; [self openFolder:str(a52session_media_path(_session))]; }

/* ---- the view ------------------------------------------------------------------------- */

- (void)applyView
{
    const int i = a52session_pref_find("aspect");
    const BOOL tv = i < 0 || a52session_pref_get_choice(_session, i) == 0;
    const BOOL smooth = a52session_get_int(_session, "smooth", 0) != 0;
    [_display setTvAspect:tv];
    [_display setSmooth:smooth];
    [_aspectItem setState:tv ? NSControlStateValueOn : NSControlStateValueOff];
    [_smoothItem setState:smooth ? NSControlStateValueOn : NSControlStateValueOff];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    const int i = a52session_pref_find("aspect");
    if (i >= 0) a52session_pref_set_choice(_session, i, a52session_pref_get_choice(_session, i) == 0 ? 1 : 0);
    [self applyView];
}

- (void)toggleSmooth:(id)sender
{
    (void)sender;
    a52session_set_int(_session, "smooth", !a52session_get_int(_session, "smooth", 0));
    [self applyView];
}

- (void)toggleFullScreen:(id)sender
{
    [_window toggleFullScreen:sender];
}

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

- (void)toggleDebugger:(id)sender
{
    (void)sender;
    [_debugger toggle];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) {
        __weak A52AppDelegate *weakSelf = self;
        _settings = [[A52SettingsWindow alloc] initWithSession:_session
            onLive:^{ [weakSelf applyView]; }
            onClosed:^(int applied) {
                A52AppDelegate *me = weakSelf;
                if (!me) return;
                [me applyView];
                [me->_controllers refresh];
                if (applied == A52_APPLY_POWER) [me say:@"Power cycled with the new configuration"];
                else if (applied == A52_APPLY_RESTART) [me say:@"Restarted with the new options"];
                [me updateStatus];
            }];
    }
    [_settings show];
}

/* ---- status, and news from the other threads ------------------------------------------ */

- (void)updateStatus
{
    if (!_session) return;
    NSString *text;
    if (_message && [_messageUntil timeIntervalSinceNow] > 0) {
        text = _message;
    } else {
        _message = nil;
        if (a52session_is_running(_session)) {
            char buf[512];
            a52session_status(_session, buf, sizeof buf);
            text = str(buf);
        } else {
            text = @"Off";
        }
    }
    [_statusText setStringValue:text];
    [_linkLight setOn:a52session_is_running(_session) && a52session_fujinet_link(_session) > 0];
}

- (void)showNotice:(NSString *)text
{
    _noticeOpen = YES;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"The Atari 5200 Stopped"];
    [a setInformativeText:text];
    [a addButtonWithTitle:@"Power Cycle"];
    [a addButtonWithTitle:@"Open the Debugger"];
    [a addButtonWithTitle:@"Leave It"];
    const NSModalResponse r = [a runModal];
    _noticeOpen = NO;
    if (r == NSAlertFirstButtonReturn) [self powerCycle:nil];
    else if (r == NSAlertSecondButtonReturn) [_debugger show];
}

/* The gamepad thread cannot touch AppKit; it posts system actions and this
 * timer takes them. It also says when gamepads come and go, when FujiNet
 * restarted itself, and when the console crashed with no debugger open. */
- (void)fastTick
{
    if (!_session) return;
    char text[512];
    int sa;
    while (a52session_sysaction_take(_session, &sa)) [self runSysaction:sa];
    unsigned gen = a52session_gamepad_generation(_session);
    if (gen != _padGeneration) {
        _padGeneration = gen;
        if (a52session_gamepad_last_event(_session, text, sizeof text) > 0) [self say:str(text)];
        [_controllers refresh];
    }
    gen = a52session_fujinet_generation(_session);
    if (gen != _fujinetGeneration) {
        _fujinetGeneration = gen;
        if (a52session_fujinet_last_event(_session, text, sizeof text) > 0) [self say:str(text)];
    }
    if (!_noticeOpen && a52session_take_notice(_session, text, sizeof text) > 0) {
        NSString *t = str(text);
        dispatch_async(dispatch_get_main_queue(), ^{ [self showNotice:t]; });
    }
}

/* A52_SCREENSHOT and A52_AUTOQUIT_FRAMES. */
- (void)hookTick
{
    if (!_session) return;
    const uint64_t frames = [_display frames];
    if (_shotPath && frames >= _shotFrames) {
        if (![_display savePng:_shotPath]) NSLog(@"A52_SCREENSHOT: could not write %@", _shotPath);
        _shotPath = nil;
    }
    if (_quitFrames && !_shotPath && frames >= _quitFrames) {
        _quitFrames = 0;
        [NSApp terminate:nil];
    }
}

/* ---- FujiNet ---------------------------------------------------------------------------- */

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (a52session_fujinet_external(_session)) {
        [self say:@"FujiNet is external: use its own web UI"];
        return;
    }
    if (!a52session_fujinet_running(_session)) {
        [self say:@"FujiNet is off (Settings ▸ FujiNet)"];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:str(a52session_fujinet_webui_url(_session))]];
}

- (void)restartFujiNet:(id)sender
{
    (void)sender;
    if (a52session_fujinet_external(_session)) {
        [self say:@"FujiNet is external: restart it where it runs"];
        return;
    }
    if (a52session_fujinet_restart(_session) != 0)
        [self alert:@"Cannot Restart FujiNet" text:[self lastError]];
    else
        [self say:@"FujiNet restarted"];
}

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = a52session_fujinet_copy_log(_session, buf, sizeof buf);
    NSString *text = nil;
    if (n > 0) {
        /* UTF-8, or failing that Latin-1: one stray byte must not blank it */
        text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSUTF8StringEncoding];
        if (!text) text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSISOLatin1StringEncoding];
    }
    NSString *shown = text ?: @"(no FujiNet output yet)";
    /* unchanged: leave the reader where they are */
    if ([shown isEqualToString:_logView.string]) return;
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    const NSPoint where = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [_logView setString:shown];
    if (atEnd) {
        [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
    } else if (scroll) {
        [scroll.contentView scrollToPoint:where];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

/* Running while the log window can be seen; one refresh when it starts. */
- (void)updateLogTimer
{
    const BOOL want = [self windowCanBeSeen:_logWindow];
    if (want && !_logTimer) {
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                   selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        _logTimer.tolerance = 0.5;
        [self refreshLog:nil];
    } else if (!want && _logTimer) {
        [_logTimer invalidate];
        _logTimer = nil;
    }
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (a52session_fujinet_external(_session)) {
        [self say:@"FujiNet is external: see its own console"];
        return;
    }
    if (!_logWindow) {
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 860, 564)];
        scroll.hasVerticalScroller = YES;
        scroll.autohidesScrollers = NO;
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
        _logView.editable = NO;
        _logView.richText = NO;
        _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _logView.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = _logView;
        _logWindow = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 860, 600)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _logWindow.title = @"FujiNet Console Log";
        _logWindow.releasedWhenClosed = NO;
        _logWindow.delegate = self;
        {
            /* the log over a strip with the Help button */
            NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
            NSButton *help = A52HelpButton(@"fujilog");
            [help setFrameOrigin:NSMakePoint(10, 6)];
            help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
            [box addSubview:scroll];
            [box addSubview:help];
            _logWindow.contentView = box;
        }
        A52SetHelpContext(_logWindow, @"fujilog");
        [_logWindow center];
    }
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
    [self updateLogTimer];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
    } else if (note.object == _window) {
        /* the console's window is the app: with the debugger or Settings
         * still open, "the last window closed" would never come */
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }
}

/* ---- help and about ----------------------------------------------------------------------- */

- (void)showHelp:(id)sender
{
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"Keys"];
    [a setInformativeText:
        @"Arrow keys: Controller 1 stick\n"
        @"Space: Fire    Left Shift: Fire 2\n"
        @"0-9, - for *, = for #: keypad\n"
        @"F4: START    F3: PAUSE    F5: RESET key\n"
        @"Shift+F5: Power Cycle (back to CONFIG)\n"
        @"F6: this help    F9: Controllers    F11: Full screen    F12: Debugger\n"
        @"Cmd+O: Open    Shift+Cmd+R: Power Cycle    Cmd+Q: Quit\n\n"
        @"Dragging with the mouse in the picture moves the analog stick of the controller chosen in "
        @"the Controllers window; the right button is its Fire. Gamepads are picked up as they are "
        @"plugged in. Drop a cartridge on the window to boot it, or a BIOS to import it."];
    [a runModal];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:[NSString stringWithFormat:@"%@ %s", APP_TITLE, A52_VERSION_STRING]];
    [a setInformativeText:
        @"The Atari 5200 SuperSystem with the FujiNet cartridge in its slot. The emulator is "
        @"atari800 (GPL-2.0-or-later, the Atari800 Development Team) in 5200 mode, with the FujiNet "
        @"cartridge firmware (GPL-3.0-or-later) over BoIP. The Altirra 5200 OS (Avery Lee) is "
        @"built in; Atari's own BIOS can be imported.\n\n"
        @"Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later\nhttps://fujinet.online/"];
    [a runModal];
}

/* ---- the menu bar ------------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (NSMenu *)addMenu:(NSString *)title to:(NSMenu *)bar
{
    NSMenuItem *top = [[NSMenuItem alloc] init];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    [top setSubmenu:m];
    [bar addItem:top];
    return m;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenu *appMenu = [self addMenu:APP_TITLE to:bar];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];

    NSMenu *machine = [self addMenu:@"Console" to:bar];
    [self item:machine title:@"Open…" action:@selector(openMedia:) key:@"o"];
    [machine addItem:[NSMenuItem separatorItem]];
    NSMenuItem *pc = [self item:machine title:@"Power Cycle (Shift+F5)" action:@selector(powerCycle:) key:@"r"];
    [pc setKeyEquivalentModifierMask:NSEventModifierFlagShift | NSEventModifierFlagCommand];
    [self item:machine title:@"Reset Key (F5)" action:@selector(resetConsole:) key:@"r"];
    _warpItem = [self item:machine title:@"Warp Speed" action:@selector(toggleWarp:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Import BIOS from Files…" action:@selector(importBiosFiles:) key:@""];
    [self item:machine title:@"Import BIOS from a ZIP…" action:@selector(importBiosZip:) key:@""];
    [self item:machine title:@"Import BIOS from a Folder…" action:@selector(importBiosFolder:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Import to SD Card…" action:@selector(importToSD:) key:@""];
    [self item:machine title:@"Open SD Folder" action:@selector(openSDFolder:) key:@""];
    [self item:machine title:@"Open Media Folder" action:@selector(openMediaFolder:) key:@""];

    NSMenu *keys = [self addMenu:@"Keys" to:bar];
    const int ks[] = { A52_KEY_START, A52_KEY_PAUSE, A52_KEY_RESET };
    NSArray<NSString *> *titles = @[ @"START (F4)", @"PAUSE (F3)", @"RESET (F5)" ];
    for (NSUInteger i = 0; i < [titles count]; i++)
        [[self item:keys title:titles[i] action:@selector(consoleKey:) key:@""] setTag:ks[i]];

    NSMenu *view = [self addMenu:@"View" to:bar];
    /* F9, F11 and F12 are the main window's (see A52ContentView): a menu
     * equivalent would take them from the Controllers and debugger windows. */
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"k"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    _smoothItem = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    NSMenuItem *fs = [self item:view title:@"Enter Full Screen (F11)" action:@selector(toggleFullScreen:) key:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];

    NSMenu *fuji = [self addMenu:@"FujiNet" to:bar];
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [self item:fuji title:@"Restart FujiNet" action:@selector(restartFujiNet:) key:@""];

    /* Window and Help: AppKit fills these in once registered as such. */
    NSMenu *windows = [self addMenu:@"Window" to:bar];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];

    NSMenu *help = [self addMenu:@"Help" to:bar];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [self item:help title:APP_TITLE @" Help" action:@selector(openHelp:) key:@"?"];
    [help addItem:[NSMenuItem separatorItem]];
    [self item:help title:@"Keys (F6)" action:@selector(showHelp:) key:@""];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[A52HelpWindow shared]];
}

/* For the window in front: Settings' page, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[A52HelpWindow shared] showFor:[NSApp keyWindow]];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    const BOOL running = _session && a52session_is_running(_session);
    if (a == @selector(toggleWarp:)) {
        [item setState:(running && a52session_warp(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return running;
    }
    if (a == @selector(openWebUI:))
        return _session && (a52session_fujinet_running(_session) || a52session_fujinet_external(_session));
    if (a == @selector(resetConsole:) || a == @selector(openMedia:) || a == @selector(consoleKey:))
        return running;
    return YES;
}
@end
